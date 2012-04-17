#include "downloader.h"
#include "threadpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <errno.h>

#include <sqlite3.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>

#include <fcntl.h>
#include <sys/mman.h>

#define TMP_DIR                    ".easy_downloader"
#define TMP_FILE_SUFFIX_FMT        "._tmp_%d"
#define DB_FILE_NAME               "downloader_db"


#define MAX_BUFFER_LEN     1024
#define MAX_REDIRECT_TIMES 5
#define MAX_PART_NUMBER    15
#define MIN_PART_SIZE      1024 * 256     // 256K
						
// SQL syntax
#define SQL_CREATE_TABLE  "create table if not exists d_breakpoints ("   \
                          "file_name varchar(255), "                     \
						  "file_url  varchar(255), "                     \
						  "file_saved_path varchar(255), "               \
						  "file_length INT, "                            \
						  "tmp_file_name_fmt varchar(255), "             \
						  "parts TINYINT, "                              \
						  "average_len INT, "                            \
						  "last_part_len INT)"                           \

#define SQL_INSERT_VALUES  "insert into d_breakpoints values ("           \
                           "'%s', '%s', '%s', '%d', '%s', %d, %d, %d)"              \

#define SQL_QUERY_TABLE    "select * from d_breakpoints where file_name='%s'"

#define SQL_QUERY_ALL      "select file_name, file_saved_path, file_length, tmp_file_name_fmt," \
                           " parts from d_breakpoints"                                           \

#define SQL_DEL_FILE       "delete from d_breakpoints where file_name is '%s' and file_saved_path is '%s'"

char download_tmp_path[PATH_MAX];
char file_saved_def_path[PATH_MAX];

// ERR MACRO
#define	ERR_OK              0
#define	ERR_FALSE          -1

#define ERR_URL            -2
#define ERR_CONNECT        -3
#define ERR_IO_WRITE       -4
#define ERR_IO_READ        -5
#define ERR_IO_CREATE      -12
#define ERR_RES_NOT_FOUND  -6

#define ERR_OPEN_TMP_FILE  -7
#define ERR_MERGE_FILES    -8
#define ERR_REQUEST_RANGE  -9
#define ERR_DB_CONNECT     -10
#define ERR_DB_EXCUTE      -11

#define ERR_RET_VAL        ((void*)(-1))
typedef struct _downloader_manager
{
	downloader            inst;
	easy_thread_pool      *tp;
	sqlite3               *db_key;
}d_manager_t;

typedef struct _downloader_task
{
	d_manager_t        *dm;

	char               url[MAX_URL_LEN];
	char               file_saved_path[PATH_MAX];
	char               tmp_file_name_fmt[PATH_MAX];    // "filename._tmp_%d"

	d_callback         finished_callback;
	d_callback         progress_callback;

	pthread_mutex_t    *mutex;
	pthread_cond_t     *cond;
	int                len_downloaded;
}d_task_t;

typedef struct _download_url
{
	char     buffer[MAX_URL_LEN];

	char     *scheme;
	char     *host;
	char     *port;
	char     *path;
	char     *filename;
}d_url_t;

typedef struct _file_info
{
	d_url_t       d_url;
	int           length;
	char          filename[PATH_MAX];
}file_info_t;

typedef struct _part_info
{
	file_info_t    *file;
	int            beg_pos;
	int            end_pos;
	d_task_t       *d_task;

	int            id;
}part_info_t;

static int db_connect(sqlite3 **pkey)
{
	char *err_msg;
	int rc = sqlite3_open(DB_FILE_NAME, pkey);
	if (rc)
	{
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(*pkey));
		sqlite3_close(*pkey);
		return ERR_DB_CONNECT;
	}
	rc = sqlite3_exec(*pkey, SQL_CREATE_TABLE, NULL, 0, &err_msg);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		sqlite3_close(*pkey);
		return ERR_DB_CONNECT;
	}
	return 0;
}

static int db_execute(sqlite3 *db_key, const char *sql_str, 
		int (*callback)(void*, int, char**, char**))
{
	char *err_msg;
	int rc = sqlite3_exec(db_key, sql_str, callback, 0, &err_msg);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return ERR_DB_EXCUTE;
	}
	return 0;
}

static void db_close(sqlite3 *db_key)
{
	sqlite3_close(db_key);
}

static int parse_url(const char *url, d_url_t *d_url)
{
	const char *begin = url;
	const char *end   = strstr(url, "//");
	
	// scheme
	d_url->scheme = d_url->buffer;
	if (end == NULL)
	{
		// http is default
		strcpy(d_url->buffer, "http:");
	}
	else
	{
		int n = end - begin;
		memcpy(d_url->buffer, url, n);
		d_url->buffer[n] = '\0';
	}

	// host
	begin = end + 2;
	if (!(end = strchr(begin, '/')))
		return ERR_URL;
	d_url->host = d_url->buffer + strlen(d_url->scheme) + 1;
	memcpy(d_url->host, begin, end - begin);

	d_url->path = d_url->host + (end - begin);
	*(d_url->path)++ = '\0';

	d_url->port = strstr(d_url->host, ":");
	if (d_url->port)
		*d_url->port++ = '\0';

	// path
	strcpy(d_url->path, end);

	if (strcmp(d_url->scheme, "http:") == 0 && d_url->port == NULL)
	{
		d_url->port = d_url->path + strlen(d_url->path) + 1;
		strcpy(d_url->port, "80");
	}

	if (!(d_url->filename = strrchr(d_url->path, '/')))
		return ERR_URL;
	d_url->filename++;
	return 0;
}

static void free_task(void *arg)
{
	task_desc *desc = (task_desc *)arg;
	d_task_t *d_task = (d_task_t *)desc->arg;
	pthread_mutex_destroy(d_task->mutex);
	pthread_cond_destroy(d_task->cond);
	free(d_task->mutex);
	free(d_task->cond);
	free(desc->arg);
	free(desc);
}

static int connect_server(const d_url_t *d_url)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int ret;
	int sockfd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_flags    = 0;

	ret = getaddrinfo(d_url->host, d_url->port, &hints, &result);
	if (ret != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		return ERR_CONNECT;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next)
	{
		sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sockfd == -1)
			continue;
		if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;
		close(sockfd);
	}

	if (rp == NULL)
	{
		fprintf(stderr, "could not connect\n");
		return ERR_CONNECT;
	}
	return sockfd;
}

static int write_n_chars(int fd, char *in_chars, int n)
{
	int ntotal = n;
	while (n > 0)
	{
		int nwritten = write(fd, in_chars, n);
		if (nwritten < 0)
			break;
		n -= nwritten;
		in_chars += nwritten;
	}
	return ntotal - n;
}

#define CONNECT_STR_FMT_1 "GET %s HTTP/1.1\r\n"   \
                          "Host: %s\r\n"          \
						  "User-Agent: Mozilla/5.0 (X11; Linux i686)\r\n"           \
						  "Accept: */*\r\n"       \
						  "Pragma: no-cache\r\n"  \
						  "Cache-control: no-cache\r\n"       \
						  "Connection: close\r\n\r\n" \

#define CONNECT_STR_FMT_2 "GET %s HTTP/1.1\r\n"   \
                          "Host: %s\r\n"          \
						  "User-Agent: Mozilla/5.0 (X11; Linux i686)\r\n"           \
						  "Accept: */*\r\n"       \
						  "Range: bytes=%d-%d\r\n"\
						  "Pragma: no-cache\r\n"  \
						  "Cache-control: no-cache\r\n"       \
						  "Connection: close\r\n\r\n"   \

static int request_download_file(const char *url, file_info_t *file, char *url_redirect)
{
	int connfd;
	d_url_t *d_url = &file->d_url;
	int ret = parse_url(url, d_url);
	static int redirect_times = 0;

	if (ret < 0)
		return ret;
	if ((connfd = connect_server(d_url)) < 0)
		return connfd;

	if (strcmp(d_url->scheme, "http:") == 0)
	{
		{
			char request[MAX_BUFFER_LEN];

			snprintf(request, MAX_BUFFER_LEN, CONNECT_STR_FMT_1, d_url->path, d_url->host);

//#if debug
			/*printf("request is %s", request);*/
//#endif
			if (write_n_chars(connfd, request, strlen(request)) != strlen(request))
			{	
				close(connfd);
				return ERR_IO_WRITE;
			}
		}

		{
			char *ptr = NULL;
			char response[MAX_BUFFER_LEN];
			char status[4] = {0};
			int nread = read(connfd, response, MAX_BUFFER_LEN); 
			close(connfd);
			nread = (nread >= MAX_BUFFER_LEN? MAX_BUFFER_LEN - 1  : nread);
			if (nread < 0)
				return ERR_IO_READ;
			response[nread] = '\0';
//#if debug
			/*printf("response is %s", response);*/
//#endif
			if (!(ptr = strstr(response, "HTTP/1.")))
				return ERR_FALSE;
			memcpy(status, ptr + strlen("HTTP/1.1 "), 3);
			status[3] = '\0';
			if (strcmp(status, "404") == 0)
			{
				fprintf(stderr, "downloader recieved http response with failed status code %s\n", status);
				return ERR_REQUEST_RANGE;
			}

			// redirect
			ptr = strstr(response, "Location:");
			if (ptr && redirect_times++ < MAX_REDIRECT_TIMES)
			{
				char url[MAX_URL_LEN];
				sscanf(ptr, "Location: %s", url);
				return request_download_file(url, file, url_redirect);
			}

			// file size
			ptr = strstr(response, "Content-Length:");
			if (ptr)
			{
				int length;
				ptr += strlen("Content-Length:");
				sscanf(ptr, "%d", &length);
				printf("file length is %d\n", length);
				if (length < 0)
					return ERR_REQUEST_RANGE;
				strcpy(url_redirect, url);
				file->length = length;
				return 0;
			}
			return ERR_RES_NOT_FOUND;
		}
	}
}

static void download_progress(d_task_t *d_task, int bytes_recv, int bytes_total)
{
	pthread_mutex_lock(d_task->mutex);
	d_task->len_downloaded += bytes_recv;
	pthread_mutex_unlock(d_task->mutex);

	if (d_task->progress_callback)
	{
		d_progress_t pt = { d_task->len_downloaded, bytes_total };
		d_task->progress_callback(&pt);
	}
	pthread_cond_signal(d_task->cond);
}

static int request_part_download_file(part_info_t *part)
{	
	int connfd = connect_server(&part->file->d_url);
	int ret = ERR_FALSE;
	fprintf(stderr, "download part id %d :  %d-%d \n", part->id, part->beg_pos, part->end_pos);
	if (connfd < 0)
	{
		fprintf(stderr, "download part %d-%d connect to srv failed\n", part->beg_pos, part->end_pos);
		return ret;
	}

	{
		char request[MAX_BUFFER_LEN + 1];
		char response[MAX_BUFFER_LEN + 1];
		int  nread;
		char *ptr;
		char status[4];
		snprintf(request,MAX_BUFFER_LEN, CONNECT_STR_FMT_2, part->file->d_url.path,part->file->d_url.host,
				part->beg_pos, part->end_pos);
		if (strlen(request) != write_n_chars(connfd, request, strlen(request)))
		{
			close(connfd);
			return ERR_FALSE;
		}
		if ((nread = read(connfd, response, MAX_BUFFER_LEN)) < 0)
		{
			close(connfd);
			return ERR_IO_READ;
		}
		response[nread] = '\0';
		if (!(ptr = strstr(response, "HTTP/1.")))
		{
			close(connfd);
			return ERR_FALSE;
		}
			
		memcpy(status, ptr + strlen("HTTP/1.1 "), 3);
		status[3] = '\0';
		if (strcmp(status, "206") != 0)
		{
			close(connfd);
			fprintf(stderr, "request range content failed with bad status code %s\n", status);
			return ERR_FALSE;
		}

		if ((ptr = strstr(ptr, "Content-Length:")))
		{
			int range_len, nbody_read, nleft;
			int flags;
			char tmp_file_name[PATH_MAX];
			FILE *fp;
			int start_read_body;
			char *body;

			fd_set rfds;

			ptr += strlen("Content-Length:");
			sscanf(ptr, "%d", &range_len);
			if ((range_len-1) != (part->end_pos - part->beg_pos))
			{
				fprintf(stderr, "response content-length is not suitable value %d", range_len);
				close(connfd);
				return ERR_FALSE;
			}

			snprintf(tmp_file_name, PATH_MAX, part->d_task->tmp_file_name_fmt, part->id);
			printf("tmp file name is %s\n", tmp_file_name);
			if (!(fp = fopen(tmp_file_name, "a")))
			{
				close(connfd);
				return ERR_FALSE;
			}

			nbody_read      = 0;
			body            = NULL;
			nleft           = range_len - nbody_read;
			start_read_body = 0;

			flags = fcntl(connfd, F_GETFL, 0);
			fcntl(connfd, F_SETFL, flags |= O_NONBLOCK);
			FD_ZERO(&rfds);
			FD_SET(connfd, &rfds);

			while (1)
			{	
				// #if debug
				char http_header[MAX_BUFFER_LEN+1];
				// #endif
				if (!start_read_body)
				{
					if (body = strstr(response, "\r\n\r\n"))
					{
						//#if debug
						*body = 0;
						strcpy(http_header, response);
						//#endif
						start_read_body = 1;
						body += 4;
						nbody_read = nread - (body - response);

						//#if debug
						printf("**********************\nhead length is %d, total nread is %d, nbody_read is %d\n*****************\n", body - response, nread, nbody_read);
						//#endif
						
					}
				}
				else
				{
					body = response;
					nbody_read = nread;
				}

				if (start_read_body)
				{
					fwrite(body, 1, nbody_read, fp);
					download_progress(part->d_task, nbody_read, part->file->length);
					if ((nleft -= nbody_read) <= 0)
						break;
				}

				fd_set active_fds = rfds;
				int retval = select(connfd + 1, &active_fds, NULL, NULL, NULL);
				if (retval == -1)
				{
					perror("select failed:");
					ret = ERR_FALSE;
					break;
				}

				nread = read(connfd, response, MAX_BUFFER_LEN);
				if (nread < 0)
				{
					perror("read failed:");
					ret = ERR_IO_READ;
					break;
				}
				else if (nread == 0)
				{
					fprintf(stderr, ">>this time only %d body read<<\n, left %d bytes to recieve, but srv close, is anything wrong about srv?\n", range_len - nleft, nleft);
					// #if debug
					printf("http header is %s\n", http_header);
					// #endif
					part->beg_pos = part->end_pos - nleft + 1;
					ret = 0;
					break;
				}

				response[nread] = '\0';
			}
			close(connfd);
			fclose(fp);
			return ret;
		}
		return ERR_FALSE;
	}
}

static void *download_part_entry(void *arg)
{
	part_info_t *part = (part_info_t *)arg;
	int ret = 0;
	while (ret == 0 && part->end_pos - part->beg_pos + 1 > 0)
		ret = request_part_download_file(part);
}

static int merge_files(const char *dst_file, int dst_len, char src_files[MAX_PART_NUMBER][PATH_MAX], int *src_lens, int nsrcs)
{
	int src_fd, dst_fd, i;
	void *src_buf, *dst_buf;
	int src_mapped_len, dst_mapped_len, dst_mem_beg_pos;
	int src_offset, dst_offset;

	int file_buffer_limit = sysconf(_SC_PAGE_SIZE) * 128 * 1024; // about 512MB

	if ((dst_fd = open(dst_file, O_RDWR)) < 0)
		return ERR_REQUEST_RANGE;
	ftruncate(dst_fd, dst_len);
	dst_mapped_len = dst_len > file_buffer_limit ? file_buffer_limit : dst_len;
	
	if ((dst_buf = mmap(NULL, dst_mapped_len, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0)) == MAP_FAILED)
	{
		close(dst_fd);
		return ERR_MERGE_FILES;
	}
	dst_len    -= dst_mapped_len;
	dst_offset += dst_mapped_len;
	dst_mem_beg_pos = 0;

	for ( i = 0; i < nsrcs; i++)
	{
		if ((src_fd = open(src_files[i], O_RDONLY)) < 0)
		{
			munmap(dst_buf, dst_mapped_len);
			close(dst_fd);
			return ERR_MERGE_FILES;
		}
		src_offset = 0;
		while (src_lens[i] > 0)
		{
			src_mapped_len = src_lens[i] > file_buffer_limit ? file_buffer_limit : src_lens[i];
			if ((src_buf = mmap(NULL, src_mapped_len, PROT_READ, MAP_SHARED, src_fd, src_offset)) == MAP_FAILED)
			{
				munmap(dst_buf, dst_mapped_len);
				close(dst_fd);
				close(src_fd);
				return ERR_MERGE_FILES;
			}

			src_lens[i] -= src_mapped_len;
			src_offset  += src_mapped_len;

			// copy to the dest file buffer
			if (dst_mem_beg_pos + src_mapped_len > file_buffer_limit)
			{
				munmap(dst_buf - dst_mem_beg_pos, dst_mapped_len);
				dst_mapped_len = dst_len > file_buffer_limit ? file_buffer_limit : dst_len;
				if ((dst_buf = mmap(NULL, dst_mapped_len, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, dst_offset)) == MAP_FAILED)
				{
					munmap(src_buf, src_mapped_len);
					close(src_fd);
					close(dst_fd);
					return ERR_MERGE_FILES;
				}

				dst_mem_beg_pos = 0;
				dst_len    -= dst_mapped_len;
				dst_offset += dst_mapped_len;
			}
			dst_mem_beg_pos += src_mapped_len;
			memcpy(dst_buf, src_buf, src_mapped_len);
			dst_buf = (char *)dst_buf + src_mapped_len;
			munmap(src_buf, src_mapped_len);
		}
		close(src_fd);
		unlink(src_files[i]);
	}
	munmap(dst_buf - dst_mem_beg_pos, dst_mapped_len);
	close(dst_fd);
	return 0;
}

static void *dispatch_part_download(d_task_t *d_task, file_info_t *file, 
		int per_part_len, int last_part_len, int parts)
{
	int i;
	int offset = 0;
	// allocate on stack
	task_desc descs[MAX_PART_NUMBER];
	part_info_t parts_info[MAX_PART_NUMBER];
	char tmp_files_name[MAX_PART_NUMBER][PATH_MAX];
	int part_lens[MAX_PART_NUMBER];
	int part_downloaded_len = 0;
	struct stat sb;
	int ret;
	char sql_buf[256];

	for (i = 0; i < parts; i++)
	{
		descs[i].arg = &parts_info[i];
		descs[i].fire_task_over = NULL;
		parts_info[i].beg_pos = offset;
		if (i == parts - 1 && last_part_len > 0)
			offset += last_part_len;
		else
			offset += per_part_len;
		parts_info[i].end_pos = offset - 1;
		parts_info[i].d_task  = d_task;
		parts_info[i].file    = file;
		parts_info[i].id      = i;
		part_lens[i]          = parts_info[i].end_pos - parts_info[i].beg_pos + 1;

		snprintf(tmp_files_name[i], PATH_MAX, d_task->tmp_file_name_fmt, i);
		ret = stat(tmp_files_name[i], &sb);
		if (ret != 0 && errno != ENOENT)
		{
			perror("stat file size failed:");
			return;
		}
		if (ret == 0)
		{
			parts_info[i].beg_pos += sb.st_size;
			d_task->len_downloaded += sb.st_size;
		}
		if (parts_info[i].end_pos - parts_info[i].beg_pos + 1 > 0)
			easy_thread_pool_add_task(d_task->dm->tp, download_part_entry, &descs[i]);
	}

	pthread_mutex_lock(d_task->mutex);
	while (d_task->len_downloaded < file->length)
		pthread_cond_wait(d_task->cond, d_task->mutex);
	pthread_mutex_unlock(d_task->mutex);

	// save downloaded file
	{
		char file_full_path[PATH_MAX];
		char *p;
		snprintf(file_full_path, PATH_MAX, "%s/%s", d_task->file_saved_path, file->filename);
		if(merge_files(file_full_path, file->length, tmp_files_name, part_lens, parts) < 0)
		{
			fprintf(stderr, "merge files failed\n");
			unlink(file_full_path);
		}
		if(p = strrchr(tmp_files_name[0], '/'))
		{
			*++p = 0;
			rmdir(tmp_files_name[0]);
		}
	}
	// delete file record from db
	snprintf(sql_buf, sizeof(sql_buf), SQL_DEL_FILE, file->filename, d_task->file_saved_path);
	db_execute(d_task->dm->db_key, sql_buf, NULL);

	if (d_task->finished_callback)
		d_task->finished_callback(NULL);
}

static int _create_unique_file(d_task_t *d_task, file_info_t *pfile)
{
	// create unique downloading file
	char file_full_path[PATH_MAX];
	char *ptr, *ptr2;
	int i;
	snprintf(file_full_path, PATH_MAX, "%s/%s", d_task->file_saved_path, pfile->d_url.filename);
	ptr = strrchr(file_full_path, '.');
	if (!ptr)
		ptr = file_full_path + strlen(file_full_path);
	ptr2 = strrchr(pfile->d_url.filename, '.');
	i = 0;
	while (1)
	{
		int ret = open(file_full_path, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if (ret == -1 && errno == EEXIST)
			sprintf(ptr, "(%d)%s", ++i, (ptr2 ? ptr2 : ""));
		else if (ret == -1)
			return ERR_IO_CREATE;
		else
		{
			close(ret);
			break;
		}
	}
	if (ptr = strrchr(file_full_path, '/'))
		strcpy(pfile->filename, ++ptr);
	return 0;
}


static void *download_entry(void *arg)
{
	d_task_t *d_task = (d_task_t *)arg;
	file_info_t file;
	char sql_buf[1024];
	char real_url[MAX_URL_LEN];
	char tmp_file_name_fmt[PATH_MAX];


	int ret, i;
	char *ptr;

	if (request_download_file(d_task->url, &file, real_url) == 0)
	{
		int i = 0;
		int parts = 0, per_part_len = 0, last_part_len = 0;
		if (MIN_PART_SIZE * MAX_PART_NUMBER >= file.length)
		{
			per_part_len = MIN_PART_SIZE;
			parts = file.length / MIN_PART_SIZE;
			last_part_len = file.length - parts * MIN_PART_SIZE;
			if (last_part_len > 0)
				parts++;
		}
		else
		{
			parts = MAX_PART_NUMBER;
			per_part_len = file.length / parts;
			last_part_len = file.length - parts * per_part_len;
			if (last_part_len > 0)
				last_part_len += per_part_len;
		}

		// create unique downloading file
		if (_create_unique_file(d_task, &file) != 0)
		{
			fprintf(stderr, "error when create download file\n");
			return ERR_RET_VAL;
		}
		// create unique tmp file dir
		strcpy(tmp_file_name_fmt, file.filename);
		ptr = tmp_file_name_fmt + strlen(tmp_file_name_fmt);
		i = 0;
		while (1)
		{
			ret = mkdir(tmp_file_name_fmt, S_IRWXU);
			if (ret != 0 && errno == EEXIST)
				sprintf(ptr, "(%d)", ++i);
			else if (ret != 0)
				return ERR_RET_VAL;
			else
				break;
		}
		snprintf(d_task->tmp_file_name_fmt, PATH_MAX, "%s/%s", tmp_file_name_fmt, TMP_FILE_SUFFIX_FMT); // filename.ext(n)/._tmp_%d

		// save this task to db file
		snprintf(sql_buf, sizeof(sql_buf), SQL_INSERT_VALUES, file.filename, 
				real_url, d_task->file_saved_path, file.length, d_task->tmp_file_name_fmt, parts,
				per_part_len, last_part_len);

		db_execute(d_task->dm->db_key, sql_buf, NULL);

		dispatch_part_download(d_task, &file, per_part_len, last_part_len, parts);
	}

}

static void *recover_entry(void *arg)
{
	d_task_t *d_task = (d_task_t *)arg;
	file_info_t file;

	char **results;
	int row,col;
	char *err_msg;
	char sql_buf[256];
	int ret, i, parts;
	int per_part_len, last_part_len;

	snprintf(sql_buf, sizeof(sql_buf), SQL_QUERY_TABLE, d_task->file_saved_path);
	ret = sqlite3_get_table(d_task->dm->db_key, sql_buf, &results, &row, &col, &err_msg);
	if (ret != SQLITE_OK)
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return ERR_RET_VAL;
	}
	if (row != 1 || col != 8)
		return ERR_RET_VAL;
	i = col + 1;

	// file_url
	if (parse_url(results[i++], &file.d_url) < 0)
		return ERR_RET_VAL;

	// file_saved_path
	strncpy(d_task->file_saved_path, results[i++], PATH_MAX);

	// file_length
	sscanf(results[i++], "%d", &file.length);

	// tmp_file_name_fmt
	strncpy(d_task->tmp_file_name_fmt, results[i++], PATH_MAX);

	// parts
	sscanf(results[i++], "%d", &parts);

	// average_len
	sscanf(results[i++], "%d", &per_part_len);

	// last_part_len
	sscanf(results[i++], "%d", &last_part_len);
	
	sqlite3_free_table(results);

	// create unique downloading file
	if (_create_unique_file(d_task, &file) != 0)
	{
		fprintf(stderr, "error when create download file\n");
		return ERR_RET_VAL;
	}
	dispatch_part_download(d_task, &file, per_part_len, last_part_len, parts);
}

static void download_task_init(d_task_t *d_task, d_callback finished, d_callback progress)
{
	d_task->mutex             = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	d_task->cond              = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_mutex_init(d_task->mutex, NULL);
	pthread_cond_init(d_task->cond, NULL);
	d_task->len_downloaded    = 0;
	d_task->finished_callback = finished;
	d_task->progress_callback = progress;
}

downloader *easy_downloader_init()
{
	int ret;

	sqlite3 *db_key;	
	snprintf(download_tmp_path, PATH_MAX, "%s/%s/", getenv("HOME"), TMP_DIR);

	ret = mkdir(download_tmp_path, S_IRWXU);
	if (ret != 0 && errno != EEXIST)
		return NULL;
	chdir(download_tmp_path);

	if ((ret = db_connect(&db_key)) != 0)
		return NULL;

	d_manager_t *manager = (d_manager_t *)malloc(sizeof(d_manager_t));
	manager->tp = easy_thread_pool_init(5, 60);
	manager->db_key = db_key;

	if (file_saved_def_path[0] == '\0')
	{
		strcpy(file_saved_def_path, getenv("HOME"));
		strcat(file_saved_def_path, "/");
	}
		
	return (downloader *)manager;
}

void easy_downloader_destroy(downloader *inst)
{
	d_manager_t *manager = (d_manager_t *)inst;
	easy_thread_pool_free(manager->tp);
	db_close(manager->db_key);
	free(manager);
}


void easy_downloader_add_task(downloader *inst, const char *url, const char *file_saved_path/*last char is '/'*/,
		d_callback finished, d_callback progress)
{
	d_manager_t *manager = (d_manager_t *)inst;

	d_task_t *d_task = (d_task_t *)malloc(sizeof(d_task_t));
	download_task_init(d_task, finished, progress);
	d_task->dm                = manager;

	strncpy(d_task->url, url, MAX_URL_LEN);
	d_task->url[MAX_URL_LEN - 1] = '\0';

	strncpy(d_task->file_saved_path, file_saved_path ? file_saved_path : file_saved_def_path, PATH_MAX);
	d_task->file_saved_path[PATH_MAX - 1] = '\0';

	task_desc *desc = (task_desc *)malloc(sizeof(task_desc));
	desc->arg = d_task;
	desc->fire_task_over = free_task;
	easy_thread_pool_add_task(manager->tp, download_entry, desc);
}

void easy_downloader_recover_task(downloader *inst, const char *file_name, d_callback finished,
		d_callback progress)
{
	d_manager_t *manager = (d_manager_t *)inst;
	d_task_t *d_task = (d_task_t *)malloc(sizeof(d_task_t));
	download_task_init(d_task, finished, progress);
	d_task->dm                = manager;
	strncpy(d_task->file_saved_path, file_name,PATH_MAX);

	task_desc *desc = (task_desc *)malloc(sizeof(task_desc));
	desc->arg = d_task;
	desc->fire_task_over = free_task;
	easy_thread_pool_add_task(manager->tp, recover_entry, desc);

}

static void get_ith_breakpoint(d_breakpoint_t *bp, char **results, int i)
{
	char tmp_file_name_fmt[PATH_MAX];
	int parts, j;
	// file_saved_path file_name
	snprintf(bp->file_name, PATH_MAX, "%s/%s", results[i+1], results[i]);
	i += 2;

	// file_length
	sscanf(results[i++], "%d", &bp->file_length);

	// tmp_file_name_fmt
	strncpy(tmp_file_name_fmt, results[i++], PATH_MAX);

	// parts
	sscanf(results[i++], "%d", &parts);

	bp->len_downloaded = 0;
	for (j = 0; j < parts; j++)
	{
		struct stat sb;
		char tmp_file_name[PATH_MAX];
		snprintf(tmp_file_name, PATH_MAX, tmp_file_name_fmt, j);
		if (stat(tmp_file_name, &sb) == 0)
			bp->len_downloaded += sb.st_size;
	}
}

int easy_downloader_get_breakpoints(downloader *inst, d_breakpoint_t *bps, int max)
{
	d_manager_t *manager = (d_manager_t *)inst;
	char **result;
	int row, col, i, j = 0;
	char *err_msg;

	if (SQLITE_OK != sqlite3_get_table(manager->db_key, SQL_QUERY_ALL, 
				&result, &row, &col, &err_msg))
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return ERR_DB_EXCUTE;
	}

	for (i = 0; i < row && i < max; i++)
	{
		j += col;
		get_ith_breakpoint(&bps[i], result, j);
	}
	sqlite3_free_table(result);
	return i;
}


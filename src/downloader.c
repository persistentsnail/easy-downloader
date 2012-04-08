#include "downloader.h"
#include "threadpool.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <errno.h>

#include <sqlite3.h>

#include <sys/types.h>

#define TMP_DIR                    ".easy_downloader"
#define TMP_FILE_SUFFIX_FMT        "._tmp_%d"
#define DB_FILE_NAME               "downloader_db"


#define MAX_URL_LEN        256 + 32
#define MAX_BUFFER_LEN     1024
#define MAX_REDIRECT_TIMES 5
#define MAX_PART_NUMBER    15
#define MIN_PART_SIZE      1024 * 256     // 256K
						
// SQL syntax
#define SQL_CREATE_TABLE  "create table if not exist d_breakpoints ("   \
                          "file_name varchar(255), "                     \
						  "file_url  varchar(255), "                     \
						  "file_saved_path varchar(255), "               \
						  "file_length INT, "                            \
						  "tmp_file_name_fmt varchar(255), "             \
						  "parts TINYINT, "                              \
						  "average_len INT, "                            \
						  "last_part_len INT)"                           \

#define SQL_INSERT_VALUES  "inert into d_breakpoint_i values ("           \
                           "%s, %s, %s, %d, %s, %d, %d, %d)"              \

#define SQL_QUERY_TABLE    "select * from d_breakpoints where file_name=%s"

#define SQL_QUERY_ALL      "select file_name, file_saved_path, file_length, tmp_file_name_fmt," \
                           "parts from d_breakpoints"                                           \

char download_tmp_path[PATH_MAX];
char file_saved_def_path[PATH_MAX];

typedef enum
{
	ERR_OK            = 0,
	ERR_FALSE         = -1,

	ERR_URL           = ERR_FALSE  - 1,
	ERR_CONNECT       = ERR_URL - 1,
	ERR_IO_WRITE      = ERR_CONNECT - 1,
	ERR_IO_READ       = ERR_IO_WRITE - 1,
	ERR_RES_NOT_FOUND = ERR_IO_READ - 1,

	ERR_OPEN_TMP_FILE = ERR_RES_NOT_FOUND - 1,
	ERR_MERGE_FILES   = ERR_OPEN_TMP_FILE - 1,
}d_err_t;


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
}file_info_t;

typedef struct _part_info
{
	file_info_t    *file;
	int            beg_pos;
	int            end_pos;
	d_task_t       *d_task;

	int            id;
}part_info_t;

static int db_connect(sqlite **pkey)
{
	char *err_msg;
	int rc = sqlite3_open(DB_FILE_NAME, pkey);
	if (rc)
	{
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(*pkey));
		sqlite3_close(*pkey);
		return -1;
	}
	rc = sqlite3_exec(*pkey, SQL_CREATE_TABLE, NULL, 0, &err_msg);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		sqlite3_close(*pkey);
		return -1;
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
		return -1;
	}
	return 0;
}

static void db_close(sqlite3 *db_key)
{
	sqlite3_close(db_key);
}

static int parse_url(const char *url, d_url_t *d_url)
{
	char *begin = url;
	char *end   = strstr(url, "//");
	
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

	d_url->path = d_url->host + end - begin;
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

static int write_n_char(int fd, char *in_chars, int n)
{
	int ntotal = n;
	while (n >= 0)
	{
		int nwritten = write(fd, n);
		if (nwritten < 0)
			break;
		n -= nwritten;
	}
	return n - total;
}

#define CONNECT_STR_FMT_1 "GET %s HTTP/1.1\r\n"   \
                          "Host: %s\r\n"          \
						  "Referer: %s\r\n"       \
						  "User-Agent: Mozilla/5.0 (X11; Linux i686)\r\n"           \
						  "Accept: */*\r\n"       \
						  "Pragma: no-cache\r\n"  \
						  "Cache-control: no-cache\r\n"       \
						  "Connection: close\r\n" \

#define CONNECT_STR_FMT_2 "GET %s HTTP/1.1\r\n"   \
                          "Host: %s\r\n"          \
						  "Referer: %s\r\n"       \
						  "User-Agent: Mozilla/5.0 (X11; Linux i686)\r\n"           \
						  "Accept: */*\r\n"       \
						  "Range: bytes=%d-%d\r\n"\
						  "Pragma: no-cache\r\n"  \
						  "Cache-control: no-cache\r\n"       \

static int request_download_file(const char *url, file_info_t *file, char *url_redirect)
{
	int connfd;
	d_url_t d_url;
	int ret = parse_url(url, &d_url);
	static int redirect_times = 0;

	if (ret < 0)
		return ret;
	if ((connfd = connect_server(&d_url)) < 0)
		return connfd;

	if (strcmp(d_url.scheme, "http:") == 0)
	{
		{
			char request[MAX_BUFFER_LEN];

			snprintf(request, MAX_BUFFER_LEN, CONNECT_STR_FMT_1, d_url.path, d_url.host, d_url.host);
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

			if (!(ptr = strstr(response, "HTTP/1.")))
				return ERR_FALSE;
			memcpy(status, ptr + strlen("HTTP/1.1 "), 3);
			status[3] = '\0';
			if (strcmp(status, "404") == 0)
			{
				fprintf(stderr, "downloader recieved http response with failed status code %s\n", status);
				return -1;
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
				sscanf(ptr, "Content-Length: %d", length);
				if (length < 0)
					return -1;
				strcpy(url_redirect, url);
				memcpy(&file->d_url, &d_url, sizeof(d_url));
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

static void *download_part_entry(void *arg)
{
	part_info_t *part = (part_info_t *)arg;
	int connfd = connect_server(part->file->d_url);
	if (connfd < 0)
	{
		fprintf(stderr, "download part %d-%d connect to srv failed\n", part->beg_pos, part->end_pos);
		return ERR_CONNECT;
	}
	{
		char request[MAX_BUFFER_LEN];
		char response[MAX_BUFFER_LEN];
		int  nread;
		char *body, *ptr;
		char status[4];
		snprintf(request,MAX_BUFFER_LEN, CONNECT_STR_FMT_2, part->file->d_url.path,part->file->d_url.host,
		part->file->d_url.host, part->beg_pos, part->end_pos);
		if (strlen(request) != write_n_chars(connfd, request, strlen(request)))
		{
			close(connfd);
			return ERR_IO_WRITE;
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
			return ERR_REQUEST_RANGE;
		}

		if ((ptr = strstr(ptr, "Content-Length")))
		{
			int range_len, nbody_read, nleft;
			int flags;
			char tmp_file_name[PATH_MAX];
			FILE *fp;


			sscanf(ptr, "Content-Length %d", &range_len);
			if (!(body = strstr(ptr, "\r\n\r\n")) || (range_len-1) != (part->end_pos - part->beg_pos))
			{
				close(connfd);
				return ERR_FALSE;
			}
			body += 4;
			nbody_read = strlen(body);
			snprintf(tmp_file_name, PATH_MAX, part->d_task->tmp_file_name_fmt, part->id);
			if (!(fp = fopen(tmp_file_name, "w+")))
			{
				close(connfd);
				return ERR_OPEN_TMP_FILE;
			}
			fwrite(body, 1, nbody_read, fp);
			download_progress(part->d_task, nbody_read, part->file->length);
			
			
			nleft = range_len - nbody_read;
			flags = fcntl(connfd, F_GETFL, 0);
			fcntl(connfd, F_SETFL, flags |= O_NONBLOCK);
			
			while (nleft > 0)
			{
				if ((nbody_read = read(connfd, response, MAX_BUFFER_LEN)) <= 0)
				{
					if (errno != EWOULDBLOCK)
					{
						fclose(fp);
						close(connfd);
						return ERR_FALSE;
					}
					continue;
				}
				fwrite(response, 1, nbody_read, fp);
				download_progress(part->d_task, nbody_read, part->file->length);
				nleft -= nbody_read;
			}
			fclose(fp);
			return 0;
		}
		return ERR_FALSE;
	}
}

static void make_unique_file_name(const char *name_in, char *name_out, int max)
{
	char fmt[64];
	char *psuffix = NULL, ptr;
	int n, i = 0;
	strncpy(name_out, name_in, max);
	if (access(name_out, F_OK) < 0)
		return;

	if (psuffix = strrchr(name_in, '.'))
		n = psuffix - name_in;
	else
		n = strlen(name_in);
	ptr = name_out + n;
	strcpy(fmt, "(%d)");
	while (true)
	{
		i++;
		snprintf(ptr, PATH_MAX, fmt, i);
		if (psuffix)
			strcat(name_out, psuffix);
		if (access(name_out, F_OK) < 0)
			break;
	}
}

static int merge_files(file_info_t *file, d_task_t *task, part_info_t *parts, int n)
{
	char tmp_file_path[PATH_MAX];
	char file_path[PATH_MAX];
	int file_fd;
	void *file_buf;
	int mem_len, file_offset, file_mapped_len, file_len;
	const int FILE_BUFFER_LIMIT = 512 * 1024 * 1024; // max file cache is 512MB

	// generate unique file name
	strcat(d_task->file_saved_path, file->d_url.filename);
	make_unique_file_name(d_task->file_saved_path, file_path);

	// open saved file, and map to RAM
	if ((file_fd = open(file_path, O_RDWR | O_CREAT, S_IRWXU | S_IROTH | S_IRGRP)) < 0)
		return ERR_FALSE;
	ftruncate(file_fd, file->length);
	file_len = file->length;
	file_mapped_len = file->length > FILE_BUFFER_LIMIT ? FILE_BUFFER_LIMIT : file->length;
	if ((file_buf = mmap(NULL, file_mapped_len, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd, 0))
			== MAP_FAILED)
		goto MERGE_FAILED;
	mem_len = 0;
	file_len    -= file_mapped_len;
	file_offset += file_mapped_len;

	// merge part files
	strcpy(tmp_file_path, download_tmp_path);
	ptr = tmp_file_path + strlen(tmp_file_path);
	for (i = 0; i < n; i++)
	{
		int fd, offset = 0;
		void *buffer;
		int part_len = parts_info[i].end_pos - parts_info[i].beg_pos + 1;

		sprintf(ptr, d_task->tmp_file_name_fmt, i);
		if ((fd = open(tmp_file_path, O_RDONLY)) < 0)
			goto MERGE_FAILED;
		while (part_len > 0)
		{
			int mapped_len = part_len > FILE_BUFFER_LIMIT? FILE_BUFFER_LIMIT : part_len;
			if ((buffer = mmap(NULL, part_len, PROT_READ, MAP_SHARED, fd, offset)) == MAP_FAILED)
				goto MERGE_FAILED;

			part_len -= mapped_len;
			offset   += mapped_len;

			// copy to the dest file buffer
			if (mem_len + mapped_len > FILE_BUFFER_LIMIT)
			{
				munmap(file_buf - mem_len, file_mapped_len);
				mem_len = 0;
				file_mapped_len = file_len > FILE_BUFFER_LIMIT ? FILE_BUFFER_LIMIT : file_len;
				if ((file_buffer = mmap(NULL, file_mapped_len, PROT_READ | PROT_WRITE, 
						MAP_SHARED, file_fd, file_offset)) == MAP_FAILED)
					goto MERGE_FAILED;
				
				file_len    -= file_mapped_len;
				file_offset += file_mapped_len;
			}
			mem_len += mapped_len;
			memcpy(file_buf, buffer, mapped_len);
			file_buf = (char *)file_buf + mapped_len;

			munmap(buffer, mapped_len);
		}
		close(fd);
		unlink(tmp_file_path);
	}
	munmap(file_buffer - mem_len, file_mapped_len);
	close(file_fd);
	return 0;
MERGE_FAILED:
    close(file_fd);
	unlink(file_path);
	return ERR_FALSE;
}

static void dispatch_part_download(d_task_t *d_task, file_info_t *file, 
		int per_part_len, int last_part_len, int parts)
{
	int i;
	int offset = 0;
	// allocate on stack
	task_desc descs[MAX_PART_NUMBER];
	part_info_t parts_info[MAX_PART_NUMBER];
	char tmp_file_name[PATH_MAX];
	int part_downloaded_len = 0;
	struct stat sb;
	int ret;

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

		snprintf(tmp_file_name, PATH_MAX, d_task->tmp_file_name_fmt, i);
		ret = stat(tmp_file_name, &sb);
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
	while (d_task->len_downloaded < file.length)
		pthread_cond_wait(d_task->cond, d_task->mutex);
	pthread_mutex_unlock(d_task->mutex);

	// save downloaded file
	if (merge_files(file, d_task, parts_info, parts) < 0)
		return ERR_MERGE_FILES;
	if (d_task->finished_callback)
		d_task->finished_callback();
}

static void *download_entry(void *arg)
{
	d_task_t *d_task = (d_task_t *)arg;
	file_info_t file;
	char sql_buf[256];
	char real_url[MAX_URL_LEN];

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
		
		strcpy(d_task->tmp_file_name_fmt, file.d_url.filename);
		strcat(d_task->tmp_file_name_fmt, TMP_FILE_SUFFIX_FMT);
		make_unique_file_name(d_task->tmp_file_name_fmt, d_task->tmp_file_name_fmt, PATH_MAX); // filename.ext(n)._tmp_%d

		// save this task to db file
		snprintf(sql_buf, sizeof(sql_buf), SQL_INSERT_VALUES, file.d_url.filename, 
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

	snprintf(sql_buf, sizeof(sql_buf), SQL_QUERY_TABLE, file_name);
	ret = sqlite3_get_table(manager->db_key, sql_buf, &results, &row, &col, &err_msg);
	if (ret != SQLITE_OK)
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return -1;
	}
	if (row != 1 || col != 8)
		return -1;
	i = col + 1;

	// file_url
	if (parse_url(results[i++], &file.d_url) < 0)
		return -1;

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
	
	sqlite3_free_table(&result);

	dispatch_part_download(d_task, &file, per_part_len, last_part_len, parts);
}

static void download_task_init(d_task_t *d_task, d_callback_t finished, d_callback_t progress)
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

	if (!(ret = db_connect(&db_key)))
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

	task_desc *desc = (task_desc *)malloc(sizeof(task_desc));
	desc->arg = d_task;
	desc->fire_task_over = free_task;
	easy_thread_pool_add_task(manager->tp, recover_entry, desc);

	return 0;
}

static void get_ith_breakpoint(d_breakpoint_t *bp, char **results, int i)
{
	char tmp_file_name_fmt[PATH_MAX];
	int parts, j;
	// file_saved_path file_name
	strncpy(bp->file_name, results[i+1], PATH_MAX);
	strcat(bp->file_name, results[i]);
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
		return -1;
	}

	for (i = 0; i < row && i < max; i++)
	{
		j += col;
		get_ith_breakpoint(&bps[i], result, j);
	}
	sqlite3_free_table(result);
	return i;
}


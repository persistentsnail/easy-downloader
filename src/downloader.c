#include "threadpool.h"
#include "downloader_imp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include <sqlite3.h>

#include <unistd.h>

#include <sys/mman.h>

#define TMP_DIR                    ".easy_downloader"
#define TMP_FILE_SUFFIX_FMT        "._tmp_%d"
#define DB_FILE_NAME               "downloader_db"


#define MAX_PART_NUMBER    15
#define MIN_PART_SIZE      (1024 * 256)     // 256K
						
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

#define ERR_RET_VAL        ((void*)(-1))



static void *download_part_entry(void *arg)
{
	part_info_t *part = (part_info_t *)arg;
	int ret = 0;
	while (ret == 0 && part->end_pos - part->beg_pos + 1 > 0)
		ret = part->d_task->request_part_file(part);
	part->finished = (ret == 0 ? 1 : 0);

	if (ret == -2)
		exit(-1);     // can not continue;

	pthread_mutex_lock(part->d_task->part_mutex);
	part->d_task->parts_exit++;
	pthread_mutex_unlock(part->d_task->part_mutex);
	pthread_cond_signal(part->d_task->part_cond);
}

static int merge_files(const char *dst_file, int dst_len, char src_files[MAX_PART_NUMBER][PATH_MAX], int *src_lens, int nsrcs)
{
	int src_fd, dst_fd, i;
	void *src_buf, *dst_buf;
	int src_mapped_len, dst_mapped_len, dst_mem_beg_pos;
	int src_offset, dst_offset = 0;

	int file_buffer_limit = sysconf(_SC_PAGE_SIZE) * 128 * 1024; // about 512MB

	if ((dst_fd = open(dst_file, O_RDWR)) < 0)
		return ERR_MERGE_FILES;
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
				perror("mmap src failed:");
				return ERR_MERGE_FILES;
			}

			src_lens[i] -= src_mapped_len;
			src_offset  += src_mapped_len;

			// copy to the dest file buffer
			if (dst_mem_beg_pos + src_mapped_len > file_buffer_limit)
			{
				memcpy(dst_buf, src_buf, (dst_mapped_len - dst_mem_beg_pos));
				src_mapped_len -= (dst_mapped_len - dst_mem_beg_pos);
				src_buf        += (dst_mapped_len - dst_mem_beg_pos);
				munmap(dst_buf - dst_mem_beg_pos, dst_mapped_len);
				dst_mapped_len = dst_len > file_buffer_limit ? file_buffer_limit : dst_len;
				if ((dst_buf = mmap(NULL, dst_mapped_len, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, dst_offset)) == MAP_FAILED)
				{
					munmap(src_buf, src_mapped_len);
					close(src_fd);
					close(dst_fd);
					perror("mmap dest failed:");
					fprintf(stderr, "part id is %d, dst_mapped_len is %d, dst_offset is %d\n", i, dst_mapped_len, dst_offset); 
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
#ifndef DEBUG
		unlink(src_files[i]);
#endif
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

	pthread_mutex_lock(d_task->part_mutex);
	while (d_task->parts_exit < parts)
		pthread_cond_wait(d_task->part_cond, d_task->part_mutex);
	pthread_mutex_unlock(d_task->part_mutex);

	if (d_task->len_downloaded < file->length)
	{
		for (i = 0; i < parts; i++)
		{
			if (!parts_info[i].finished)
				d_task->request_part_file(&parts_info[i]);
		}
	}

	if (d_task->len_downloaded < file->length)
		fprintf(stderr, "download failed\n");
	else // save downloaded file
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
		// delete file record from db
		snprintf(sql_buf, sizeof(sql_buf), SQL_DEL_FILE, file->filename, d_task->file_saved_path);
		db_execute(d_task->dm->db_key, sql_buf, NULL);
	}
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

	if (d_task->request_file_info(d_task->url, &file, real_url) == 0)
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
		DEBUG_OUTPUT("parts is %d\n", parts);

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
	switch(file.d_url.proto)
	{
		case HTTP:
			d_task->request_part_file = http_request_part_file;
			break;
		case FTP:
			d_task->request_part_file = ftp_request_part_file;
			break;
	}

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

static void download_task_free(void *arg)
{
	task_desc *desc = (task_desc *)arg;
	d_task_t *d_task = (d_task_t *)desc->arg;
	pthread_mutex_destroy(d_task->len_mutex);
	pthread_mutex_destroy(d_task->part_mutex);
	pthread_cond_destroy(d_task->part_cond);
	free(d_task->len_mutex);
	free(d_task->part_cond);
	free(d_task->part_mutex);
	free(desc->arg);
	free(desc);

	if (d_task->finished_callback)
		d_task->finished_callback(NULL);
}

static void download_task_init(d_task_t *d_task, d_callback finished, d_callback progress)
{
	d_task->len_mutex             = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	d_task->part_mutex            = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	d_task->part_cond             = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_mutex_init(d_task->len_mutex, NULL);
	pthread_mutex_init(d_task->part_mutex, NULL);
	pthread_cond_init(d_task->part_cond, NULL);
	d_task->len_downloaded    = 0;
	d_task->parts_exit        = 0;
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

	if ((ret = db_connect(DB_FILE_NAME, &db_key, SQL_CREATE_TABLE)) != 0)
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

	switch(protocol(url))
	{
		case HTTP:
			d_task->request_file_info = http_request_file_info;
			d_task->request_part_file = http_request_part_file;
			break;
		case FTP:
			d_task->request_file_info = ftp_request_file_info;
			d_task->request_part_file = ftp_request_part_file;
			break;
	}

	task_desc *desc = (task_desc *)malloc(sizeof(task_desc));
	desc->arg = d_task;
	desc->fire_task_over = download_task_free;
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
	desc->fire_task_over = download_task_free;
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

void download_progress(d_task_t *d_task, int bytes_recv, int bytes_total)
{
	pthread_mutex_lock(d_task->len_mutex);
	d_task->len_downloaded += bytes_recv;
	pthread_mutex_unlock(d_task->len_mutex);

	if (d_task->progress_callback)
	{
		d_progress_t pt = { d_task->len_downloaded, bytes_total };
		d_task->progress_callback(&pt);
	}
}



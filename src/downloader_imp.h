#ifndef __DOWNLOADER_IMP_H__
#define __DOWNLOADER_IMP_H__

#include "downloader.h"
#include "utils.h"
#include <pthread.h>
#include "threadpool.h"

typedef struct _downloader_task d_task_t;

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
	int            finished;
}part_info_t;


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

	pthread_mutex_t    *len_mutex;
	int                len_downloaded;

	pthread_mutex_t    *part_mutex;
	pthread_cond_t     *part_cond;
	int                parts_exit;

	int (*request_file_info)(const char*, file_info_t *, char *);
	int (*request_part_file)(part_info_t *part);
}d_task_t;

void download_progress(d_task_t *d_task, int bytes_recv, int bytes_total);

int http_request_file_info(/*in*/const char *url, /*out*/file_info_t *file, /*out*/char *url_redirect);
int ftp_request_file_info(/*in*/const char *url, /*out*/file_info_t *file, /*out*/char *url_redirect);

int http_request_part_file(/*in*/part_info_t *part);
int ftp_request_part_file(/*in*/part_info_t *part);

#endif

#ifndef __DOWNLOADER_H__
#define __DOWNLOADER_H__

#define ERR_URL    0
#define 
typedef struct _downloader
{
}downloader;

typedef struct _download_progress
{
	int bytes_recv;
	int bytes_total;
}d_progress_t;

typedef void *(*d_callback)(void *);

typedef enum
{
	ERR_OK            = 0,
	ERR_FALSE         = -1,

	ERR_URL           = ERR_FALSE  - 1,
	ERR_CONNECT       = ERR_URL - 1,
	ERR_IO_WRITE      = ERR_CONNECT - 1,
	ERR_IO_READ       = ERR_IO_WRITE - 1,
	ERR_RES_NOT_FOUND = ERR_IO_READ - 1,
}d_err_t;

downloader *easy_dowloader_init();
void easy_downloader_add_task(downloader *inst, const char *url, const char *file_saved_path, 
		d_callback finished, d_callback progress);
void easy_downloader_destroy(downloader *inst);

#endif

#ifndef __DOWNLOADER_H__
#define __DOWNLOADER_H__

typedef struct _downloader
{
	int unused;
}downloader;

typedef struct _download_progress
{
	int bytes_recv;
	int bytes_total;
}d_progress_t;

typedef struct _download_breakpoint
{
	char file_name[PATH_MAX];
	int  file_length;
	int  len_downloaded;
}d_breakpoint_t;

typedef void *(*d_callback)(void *);

downloader *easy_dowloader_init();

void easy_downloader_add_task(downloader *inst, const char *url, const char *file_saved_path, 
		d_callback finished, d_callback progress);

void easy_downloader_recover_task(downloader *inst, const char *file_name, d_callback finished,
		d_callback progress);

int easy_downloader_get_breakpoints(downloader *inst, d_breakpoint_t *bps, int max);

void easy_downloader_destroy(downloader *inst);

#endif

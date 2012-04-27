#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "downloader.h"

#define USAGE_STR "Usage: edownloader [-d|r] URL [PATH]\n"                  \
                  "-d                Download file to PATH from URL\n"      \
				  "-r                Recover Last terminate downloads\n"    \

int check_url(const char *url)
{
	return 1;
}

int check_path(const char *path)
{
	return 1;
}

int parse_opt(const char *str)
{
	const char *ptr = str;
	ptr++;
	if (*ptr == 0 || *(ptr + 1) != 0)
		return -1;
	switch(*ptr)
	{
		case 'r':
			return 1;
		case 'd':
			return 2;
		default:
			return -1;
	}
}

pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_finished = 0;
void *download_finished(void *arg)
{
	pthread_mutex_lock(&g_mutex);
	g_finished = 1;
	pthread_mutex_unlock(&g_mutex);
	pthread_cond_signal(&g_cond);
}


int main(int argc, char *argv[])
{
	downloader *der;
	int i;
	char *p_url = NULL;
	int opt = -1; 
	int index = 1;
	char *file_name = NULL;
	char *saved_path = NULL;

	if (argc < 2)
		goto PARSE_ARGV_FAILED;
	if (argv[index][0] == '-' && (opt = parse_opt(argv[index])) >= 0)
	{
		index++;
		if (opt == 1)  // recover
		{
			if (index == argc - 1)
				file_name = argv[index];
			else if (index < argc - 1)
				goto PARSE_ARGV_FAILED;
		}
		else if (opt == 2)
		{
			if (index == argc - 1 && check_url(argv[index]))
				p_url = argv[index];
			else if (index == argc - 2 && check_url(argv[index]) && check_path(argv[++index]))
			{
				p_url = argv[index - 1];
				saved_path = argv[index];
			}
			else
				goto PARSE_ARGV_FAILED;
		}
	}
	else if(check_url(argv[index]))
	{
		opt = 2;
		p_url = argv[index];
		index++;
		if (index == argc - 1)
			saved_path = argv[index];
		else if (index < argc - 1)
			goto PARSE_ARGV_FAILED;
	}
	else
		goto PARSE_ARGV_FAILED;

	der = easy_downloader_init();
	if (!der)
		exit(EXIT_FAILURE);
	switch (opt)
	{
		case 1:
		{
			if (file_name)
				easy_downloader_recover_task(der, file_name, download_finished, NULL);
			else
			{
				d_breakpoint_t bps[5];
				int cnt = easy_downloader_get_breakpoints(der, bps, 5);
				int i;
				for (i = 0; i < cnt; i++)
				{
					printf("%d.\t%s\t%d/%d\t%f", i+1, bps[i].file_name,
					bps[i].len_downloaded, bps[i].file_length, 0.1f*bps[i].len_downloaded/bps[i].file_length);
					printf("\n");
				}
				return 0;
			}
		}
		break;
		case 2:
		{
			easy_downloader_add_task(der, p_url, saved_path, download_finished, NULL);
		}
		break;
		default:
		fprintf(stderr, "run time error\n");
		exit(EXIT_FAILURE);
	}
	pthread_mutex_lock(&g_mutex);
	while (!g_finished)
		pthread_cond_wait(&g_cond, &g_mutex);
	pthread_mutex_unlock(&g_mutex);
	easy_downloader_destroy(der);
	return 0;
PARSE_ARGV_FAILED:
    printf("error parameters.\n%s", USAGE_STR);
}

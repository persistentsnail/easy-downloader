#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
		p_url = argv[index];
		index++;
		if (index == argc - 1)
			saved_path = argv[index];
		else
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
				easy_downloader_recover_task(der, file_name, NULL, NULL);
			else
			{
				d_breakpoint_t bps[5];
				easy_downloader_get_breakpoints(der, bps, 5);
			}
		}
		break;
		case 2:
		{
			easy_downloader_add_task(der, p_url, saved_path, NULL, NULL);
		}
		break;
		default:
		fprintf(stderr, "run time error\n");
		exit(EXIT_FAILURE);
	}
	return 0;
PARSE_ARGV_FAILED:
    printf("%s", USAGE_STR);
}

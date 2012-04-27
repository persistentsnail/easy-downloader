#include "downloader_imp.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <fcntl.h>

#define MAX_BUFFER_LEN     1024
#define MAX_REDIRECT_TIMES 5

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


int http_request_file_info(const char *url, file_info_t *file, char *url_redirect)
{
	int connfd;
	d_url_t *d_url = &file->d_url;
	int ret = parse_url(url, d_url);
	static int redirect_times = 0;

	if (ret < 0)
	{
		fprintf(stderr, "wrong url request\n");
		return ret;
	}
	if ((connfd = connect_server(d_url)) < 0)
		return connfd;

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
		char response[MAX_BUFFER_LEN + 1];
		char status[4] = {0};
		int nread = read(connfd, response, MAX_BUFFER_LEN); 
		close(connfd);
		if (nread < 0)
			return ERR_IO_READ;
		response[nread] = '\0';
		//#if debug
		/*printf("response is %s", response);*/
		//#endif
		if (!(ptr = strstr(response, "HTTP/1.")))
			return ERR_FALSE;
		memcpy(status, ptr + strlen("HTTP/1.x "), 3);
		status[3] = '\0';
		if (strcmp(status, "200") != 0)
		{
			fprintf(stderr, "downloader recieved http response with failed status code %s\n", status);
			return ERR_REQUEST_FILE;
		}

		// redirect
		ptr = strstr(response, "Location:");
		if (ptr && redirect_times++ < MAX_REDIRECT_TIMES)
		{
			char url[MAX_URL_LEN];
			sscanf(ptr, "Location: %s", url);
			return http_request_file_info(url, file, url_redirect);
		}

		// file size
		ptr = strstr(response, "Content-Length:");
		if (ptr)
		{
			int length;
			ptr += strlen("Content-Length:");
			sscanf(ptr, "%d", &length);
			//#if debug
			printf("file length is %d\n", length);
			//#endif

			// check file length
			if (length < 0)
				return ERR_REQUEST_FILE;

			if (url_redirect)
				strcpy(url_redirect, url);
			file->length = length;
			return 0;
		}
		return ERR_RES_NOT_FOUND;
	}
}


int http_request_part_file(part_info_t *part)
{	
	int connfd = connect_server(&part->file->d_url);
	int ret = ERR_FALSE;

	// #if debug
	printf("download part id %d :  %d-%d \n", part->id, part->beg_pos, part->end_pos);
	// #endif

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
			return ERR_FALSE;
		}
		response[nread] = '\0';
		if (!(ptr = strstr(response, "HTTP/1.")))
		{
			close(connfd);
			return ERR_FALSE;
		}
			
		memcpy(status, ptr + strlen("HTTP/1.x "), 3);
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
				// char http_header[MAX_BUFFER_LEN+1];
				if (!start_read_body)
				{
					if (body = strstr(response, "\r\n\r\n"))
					{
						// *body = 0;
						// strcpy(http_header, response);
						start_read_body = 1;
						body += 4;
						nbody_read = nread - (body - response);

						// printf("head length is %d, total nread is %d, nbody_read is %d\n", body - response, nread, nbody_read);
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
					part->beg_pos += nbody_read;
					if ((nleft -= nbody_read) <= 0)
						break;
				}

				fd_set active_fds = rfds;
				int retval = select(connfd + 1, &active_fds, NULL, NULL, NULL);
				if (retval == -1 && errno != EINTR)
				{
					perror("select failed:");
					ret = -2;
					break;
				}

				nread = read(connfd, response, MAX_BUFFER_LEN);
				if (nread < 0 && errno != EWOULDBLOCK)
				{
					perror("read failed:");
					ret = -2;
					break;
				}
				else if (nread == 0)
				{
					fprintf(stderr, "only %d body read, left %d bytes to recieve, but srv close, is anything wrong about srv?\n", range_len - nleft, nleft);
					// printf("http header is %s\n", http_header);
					ret = 0;
					break;
				}

				nread = nread < 0? 0 : nread;
				response[nread] = '\0';
			}
			close(connfd);
			fclose(fp);
			return ret;
		}
		return ERR_FALSE;
	}
}

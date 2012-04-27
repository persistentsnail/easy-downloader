#include "downloader_imp.h"

#define MAX_BUF_SIZE 256
#define MAX_LINE_SIZE 1024

static int read_reply_line(int fd, char *reply, int max)
{
	int cnt = 0;
	char buf[MAX_BUF_SIZE + 1];
	while(1)
	{
		int n = read(fd, buf, MAX_BUF_SIZE);
		if ( n <= 0)
			return -1;
		if (cnt < max)
		{
			memcpy(reply + cnt, buf, n > max - cnt ? max - cnt : n);
			cnt += n;
		}
		buf[n] = '\0';
		if (strstr(buf, "\r\n"))
			break;
	}
	return cnt > max ? max : cnt;
}

static int read_reply_whole(int fd, char *reply_line, int max)
{
	char buf[4];
	int ret, code;


	char *reply = reply_line;
	if (reply == NULL || max < 4)
	{
		reply = buf;
		max = 4;
	}
	
	ret = read_reply_line(fd, reply, max);
	if (ret > 0)
	{
		char flag = reply[3];
		memcpy(buf, reply, 4);
		reply[ret] = '\0';
		buf[3] = '\0';
		code = atoi(reply);
		if (flag == '-')
		{
			while (1)
			{
				ret = read_reply_line(fd, buf, 4);
				if (ret <= 0)
					return -1;
				if (buf[3] == '-')
					break;
			}
		}
		return code;
	}
	else
	{
		strncpy(reply_line, "error reply format\n", max);
		reply_line[max] = '\0';
		return -1;
	}
}

static int get_reply_code(int fd)
{
	return read_reply_whole(fd, NULL, 0);
}

/*
static void assert_reply_code(int fd, int code)
{
	char reply_line[MAX_LINE_SIZE + 1] = {'\0'};
	if (read_reply_whole(fd, reply_line, MAX_LINE_SIZE) != code)
	{
		fprintf(stderr, "%s", reply_line);
	}
}
*/

#define ASSERT_REPLY_CODE(fd, code)                                               \
        if (read_reply_whole((fd), reply_line, MAX_LINE_SIZE) != (code))          \
		{                                                                         \
			fprintf(stderr, "%s", reply_line);                                    \
			return -1;                                                            \
		}                                                                         \

static int send_cmd(int fd, const char *cmd)
{
	if (write_n_chars(fd, cmd, strlen(cmd)) != strlen(cmd))
		return -1;
	return 0;
}

static int do_login(int fd)
{
	char reply_line[MAX_LINE_SIZE + 1] = {'\0'};
	send_cmd(fd, "USER anonymous\r\n");
	int code = read_reply_whole(fd, reply_line, MAX_LINE_SIZE);
	if (code == 230)
		return 0;
	else if (code == 331) // need password
	{
		send_cmd(fd, "PASS \r\n");
		if ((code = read_reply_whole(fd, reply_line, MAX_LINE_SIZE)) == 230)
			return 0;
	}

	if (code == 332) // need account
	{
		send_cmd(fd, "ACCT \r\n");
		if (read_reply_whole(fd, reply_line, MAX_LINE_SIZE) == 230)
			return 0;
	}
	fprintf(stderr, "login failed: %s\n", reply_line);
	return -1;
}

int ftp_request_part_file(/*in*/part_info_t *part)
{
	int length    = part->end_pos - part->beg_pos + 1;
	int offset = part->beg_pos;
	int ctl_fd, data_fd; 
	int code, nread;
	char cmd[256 + PATH_MAX];
	char reply_line[MAX_LINE_SIZE + 1];
	int a1, a2, a3, a4, p1, p2;
	int times;
	d_url_t data_url;


	while ((ctl_fd = connect_server(&part->file->d_url)) < 0)
	{
		if (times++ > 10)
		{
			fprintf(stderr, "part %d connet failed\n", part->id);
			return ctl_fd;
		}
		sleep(1);
	}

	DEBUG_OUTPUT("part %d connect successfully\n", part->id);
	ASSERT_REPLY_CODE(ctl_fd, 220);

	// >> USER & PASS
	if (do_login(ctl_fd) < 0)
	{
		fprintf(stderr, "part %d login failed\n", part->id);
		return -1;
	}

	// >> PASV
	send_cmd(ctl_fd, "PASV \r\n");
	code = 0;
	if ((nread = read_reply_line(ctl_fd, reply_line, MAX_LINE_SIZE)) > 4)
	{
		int i;
		for (i = 0; i < 3; i++)
			code = code * 10 + reply_line[i] - '0';
		while (code == 227)
		{
			if (isdigit(reply_line[i]))
				break;
			i++;
		}
		if (code == 227)
		{
			reply_line[nread] = '\0';
			if (sscanf(reply_line + i, "%d,%d,%d,%d,%d,%d", &a1, &a2, &a3, &a4, &p1, &p2) != 6)
				code = 0;
		}
	}
	if (code != 227)
	{
		fprintf(stderr, "cmd PASV failed\n");
		return -1;
	}

	snprintf(data_url.buffer, sizeof(data_url.buffer), "%d.%d.%d.%d", a1, a2, a3, a4);
	data_url.host = data_url.buffer;
	data_url.port = data_url.buffer + strlen(data_url.host) + 1;
	snprintf(data_url.port, 64, "%d", (p1 << 8) + p2);
	if ((data_fd = connect_server(&data_url)) < 0)
		return -1;

	// >> TYPE I
	send_cmd(ctl_fd, "TYPE I\r\n");
	ASSERT_REPLY_CODE(ctl_fd, 200);

	// >> REST offset
	snprintf(cmd, sizeof(cmd), "REST %d\r\n", offset);
	send_cmd(ctl_fd, cmd);
	ASSERT_REPLY_CODE(ctl_fd, 350);

	// >> RETR filename
	snprintf(cmd, sizeof(cmd), "RETR %s\r\n", part->file->d_url.path);
	send_cmd(ctl_fd, cmd);

	// transfer file
	{
		FILE *fp;
		char tmp_file_name[PATH_MAX];
		fd_set rfds;
		int max_fd;
		int ret;

		snprintf(tmp_file_name, PATH_MAX, part->d_task->tmp_file_name_fmt, part->id);
		if (!(fp = fopen(tmp_file_name, "a")))
		{
			fprintf(stderr, "part %s file can't open\n", tmp_file_name);
			close(ctl_fd);
			close(data_fd);
			return -1;
		}

		FD_ZERO(&rfds);
		FD_SET(data_fd, &rfds);
		FD_SET(ctl_fd, &rfds);
		max_fd = data_fd > ctl_fd ? data_fd : ctl_fd;

		ret = 0;
		while(1)
		{
			fd_set active_fds = rfds;
			int ret = select(max_fd + 1, &active_fds, NULL, NULL, NULL);
			if (ret == -1 && errno != EINTR)
			{
				perror("select failed:");
				ret = -2;
				break;
			}
			if (FD_ISSET(ctl_fd, &active_fds))
			{
				int code = get_reply_code(ctl_fd);
				if (code >= 300) // only 125, 150, 226, 250 allowed
				{
					fprintf(stderr, "cmd RETR failed with code %d\n", code);
					ret = -2;
					break;
				}
			}
			else if (FD_ISSET(data_fd, &active_fds))
			{
				char buf[256];
				nread = read(data_fd, buf, 256);
				if (nread <= 0)
				{
					perror("read file data failed");
					ret = -2;
					break;
				}
				nread = nread > length ? length : nread;
				fwrite(buf, 1, nread, fp);
				part->beg_pos += nread;
				download_progress(part->d_task, nread, part->file->length);
				if ((length -= nread) <= 0)
					break;
			}
		}
		fclose(fp);
		close(ctl_fd);
		close(data_fd);
		return ret;
	}
}

int ftp_request_file_info(/*in*/const char *url, /*out*/file_info_t *file, /*out*/char *url_redirect)
{
	int ctl_fd;
	int ret, code;
	int nread, nwritten;
	char cmd[PATH_MAX + 256];
	char reply_line[MAX_LINE_SIZE + 1];


	if (ret = parse_url(url, &file->d_url))
	{
		fprintf(stderr, "wrong url rquest\n");
		return ret;
	}
	if (url_redirect)
		strcpy(url_redirect, url);
	if ((ctl_fd = connect_server(&file->d_url)) < 0)
		return ctl_fd;

	ASSERT_REPLY_CODE(ctl_fd, 220);
	DEBUG_OUTPUT("%s", "connect server successfully\n");

	if (do_login(ctl_fd) < 0)
		return -1;

	send_cmd(ctl_fd, "TYPE I\r\n");
	ASSERT_REPLY_CODE(ctl_fd, 200);

	snprintf(cmd, sizeof(cmd), "SIZE %s\r\n", file->d_url.path);
	send_cmd(ctl_fd, cmd);

	if ((ret = read_reply_line(ctl_fd, reply_line, MAX_LINE_SIZE)) < 5)
		return -1;
	reply_line[ret] = '\0';
	sscanf(reply_line + 4, "%d\r\n", &file->length);
	DEBUG_OUTPUT("file size is %d\n", file->length);
	
	close(ctl_fd);
	return 0;
}

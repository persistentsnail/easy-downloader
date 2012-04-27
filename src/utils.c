#include "utils.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int db_connect(const char *db_file_name, sqlite3 **pkey, const char *sql_create_table)
{
	char *err_msg;
	int rc = sqlite3_open(db_file_name, pkey);
	if (rc)
	{
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(*pkey));
		sqlite3_close(*pkey);
		return ERR_DB_CONNECT;
	}
	rc = sqlite3_exec(*pkey, sql_create_table, NULL, 0, &err_msg);
	if (rc != SQLITE_OK)
	{
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		sqlite3_close(*pkey);
		return ERR_DB_CONNECT;
	}
	return 0;
}

int db_execute(sqlite3 *db_key, const char *sql_str, int (*callback)(void*, int, char**, char**))
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

void db_close(sqlite3 *db_key)
{
	sqlite3_close(db_key);
}

int parse_url(const char *url, d_url_t *d_url)
{
	const char *begin = url;
	const char *end   = strstr(url, "//");

	d_url->proto = protocol(url);

	// host
	if (end)
		begin = end + 2;
	if (!(end = strchr(begin, '/')))
		return ERR_URL;
	d_url->host = d_url->buffer;
	memcpy(d_url->host, begin, end - begin);

	d_url->path = d_url->host + (end - begin);
	*(d_url->path)++ = '\0';

	d_url->port = strstr(d_url->host, ":");
	if (d_url->port)
		*d_url->port++ = '\0';

	// path
	strcpy(d_url->path, end);

	if (d_url->port == NULL)
	{
		d_url->port = d_url->path + strlen(d_url->path) + 1;
		switch (d_url->proto)
		{
			case HTTP:
				strcpy(d_url->port, "80");
				break;
			case FTP:
			    strcpy(d_url->port, "21");
				break;
		}
	}

	if (!(d_url->filename = strrchr(d_url->path, '/')))
		return ERR_URL;
	d_url->filename++;
	return 0;
}

protocol_t protocol(const char *url)
{
	const char *ptr = strstr(url, "//");
	if (ptr)
	{
		if (strncasecmp(url, "http:", strlen("http:")) == 0)
			return HTTP;
		else if (strncasecmp(url, "ftp:", strlen("ftp:")) == 0)
			return FTP;
	}
	// HTTP is default
	return HTTP;
}

int write_n_chars(int fd, const char *buf, int n)
{
	int ntotal = n;
	while (n > 0)
	{
		int nwritten = write(fd, buf, n);
		if (nwritten < 0)
			break;
		n   -= nwritten;
		buf += nwritten;
	}
	return ntotal - n;
}

int read_n_chars(int fd, char *buf, int n)
{
	int ntotal = n;
	while (n > 0)
	{
		int nread = read(fd, buf, n);
		if (nread <= 0)
			break;
		n   -= nread;
		buf += nread;
	}
	return ntotal -n;
}

int connect_server(const d_url_t *d_url)
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
		fprintf(stderr, "could not connect %s, %s\n", d_url->host, d_url->port);
		perror("error");
		return ERR_CONNECT;
	}
	return sockfd;
}



#ifndef __UTILS_H__
#define __UTILS_H__

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sqlite3.h>

#define MAX_URL_LEN        256 + 32

// ERR MACRO
#define	ERR_OK              0
#define	ERR_FALSE          -1

#define ERR_URL            -2
#define ERR_CONNECT        -3
#define ERR_IO_WRITE       -4
#define ERR_IO_READ        -5
#define ERR_IO_CREATE      -12
#define ERR_RES_NOT_FOUND  -6

#define ERR_OPEN_TMP_FILE  -7
#define ERR_MERGE_FILES    -8
#define ERR_REQUEST_FILE   -9
#define ERR_DB_CONNECT     -10
#define ERR_DB_EXCUTE      -11

#ifdef DEBUG
#include <stdio.h>
#define DEBUG_OUTPUT(fmt, str) fprintf(stderr, fmt, str)
#else
#define DEBUG_OUTPUT(fmt, str)
#endif

typedef enum {HTTP, FTP, UNDEF} protocol_t;

typedef struct _download_url
{
	char     buffer[MAX_URL_LEN];

	protocol_t  proto;
	char     *host;
	char     *port;
	char     *path;
	char     *filename;
}d_url_t;

int db_connect(const char *db_file_name, sqlite3 **pkey, const char *sql_create_table);
void db_close(sqlite3 *db_key);
int db_execute(sqlite3 *db_key, const char *sql_str, int (*callback)(void*, int, char**, char**));

int parse_url(const char *url, d_url_t *d_url);
protocol_t protocol(const char *url);
int write_n_chars(int fd, const char *buf, int n);
int read_n_chars(int fd, char *buf, int n);
int connect_server(const d_url_t *d_url);
#endif

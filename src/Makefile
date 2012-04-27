	#gcc -g -o edownloader main.c downloader.c httpdownloader.c ftpdownloader.c utils.c threadpool/threadpool.c -I./threadpool/ -lsqlite3 -lpthread -lrt
CC      :=  gcc

ifeq ($(debug), 1)
CFLAGS = -g -DDEBUG -I./threadpool
else
CFLAGS = -O2 -I./threadpool
endif


LDFLAGS = -lpthread -lrt -lsqlite3 -L/usr/lib/local

TP_SRCS =  ./threadpool/threadpool.c
SRCS    =  main.c utils.c downloader.c httpdownloader.c ftpdownloader.c $(TP_SRCS)
OBJS    = ${SRCS:%.c=%.o}

all : depend target

depend :
	$(CC) $(CFLAGS) -MM $(SRCS) > .depend

-include .depend

#%.o : %.c
	#$(CC) $(CFLAGS) $< -o $@

target : $(OBJS)
	$(CC) -o edownloader $(OBJS) $(LDFLAGS)

clean :
	rm *.o edownloader


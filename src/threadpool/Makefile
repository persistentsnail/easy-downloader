ifeq ($(debug), 1)
CFLAGS = -g -DDEBUG
else
CFLAGS = -O2
endif

all:
	gcc  ${CFLAGS} -o test  test.c threadpool.c -lpthread -lrt

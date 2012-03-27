#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

typedef void *(*task_func)(void *);

typedef struct
{
	void *arg;
	void *ret;
} task_desc;

typedef struct easy_thread_pool
{
	int init_pool_size;
	int max_pool_size;
} easy_thread_pool;

easy_thread_pool *
easy_thread_pool_init(int init_pool_size, int max_pool_size);

void
easy_thread_pool_add_task(easy_thread_pool *easy_tp, task_func func, task_desc *task_info);

void
easy_thread_pool_free(easy_thread_pool *easy_tp);

#endif

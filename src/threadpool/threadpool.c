#include "threadpool.h"
#include <pthread.h>

#define MAX_THREADS_ALLOWED 1024

typedef struct _task_node
{
	task_func         func;
	task_desc         *desc;
	_task_node        *next;
}task_node;

typedef struct _task_list
{
	task_node         *head;
	task_node         *tail;
	pthread_mutex_t   *mutex;
}task_list;


typedef struct _thread_info
{
	//void *(*thread_entry)(void*);
	pthread_cond_t  *active_cond;
	pthread_mutex_t *active_mutex;
	int quit; 
	task_node       *task;
}thread_info;

typedef struct _thread_array
{
	thread_info       *threads;
	int               size;
	int               *unused;
	int               unused_size;

	pthread_mutex_t   *mutex;
	pthread_cond_t    *cond;     // 似乎不需要
}thread_array;

typedef struct _idle_thread_id_array
{
	int               *idxs;
	int               size;
	pthread_mutex_t   *mutex;
	pthread_cond_t    *cond;
}idle_thread_array;

typedef struct _easy_thread_pool_manager
{
	easy_thread_pool  *easy_tp;

	thread_array      all_threads;
	task_list         all_tasks;

	idle_thread_array idle_threads;
}easy_tp_man;

typedef struct _thread_task_arg
{
	easy_tp_man           *man;
	thread_info           *ti;
	int                    id;
}thread_task_arg;

static void *thread_task_entry(void *arg)
{
	// attention! here no lock
	easy_tp_man *manager = (thread_task_arg *)(arg)->man;
	thread_info *ti      = (thread_task_arg *)(arg)->ti;
	int id               = (thread_task_arg *)(arg)->id;
	
	//
	while (;;)
	{
		pthread_mutex_lock(ti->active_mutex);
		while (ti->task == NULL || !ti->quit)
			pthread_cond_wait(ti->active_cond, ti->active_mutex);
		pthread_mutex_unlock(ti->active_mutex);
		if (!ti->quit)
		{
			// ASSERT(ti->task);
			ti->task.desc->ret = ti->task.func(ti->task.desc->argc);
			free(ti->task);
			// become idle
			pthread_mutex_lock(manager->idle_threads.mutex);
			manager->idle_threads.idxs[manager->idle_threads.size] = id;
			manager->idle_threads.size++;
			pthread_mutex_unlock();
			pthread_cond_signal(manager->idle_threads.cond);
		}
		else
			break;
	}
	free(arg);
	return 0;
}

static void *thread_manager_entry(void *arg)
{
	int idle_id = -1;
	easy_tp_man *manager = (easy_tp_man *)arg;
	task_node *task;

	while (;;)
	{
		// wait until a task coming
		idle_id = -1;
		pthread_mutex_lock(manager->all_tasks.mutex);
		while (manager->all_tasks.head == NULL)
			pthread_cond_wait(manager->all_tasks.cond, manager->all_tasks.mutex);
		task = manager->all_tasks.head;
		manager->all_tasks.head = manager->all_tasks.head->next;
		pthread_mutex_unlock(manager->all_tasks.mutex);

		// any idle thread exist? if indeed, let it run the task
		pthread_mutex_lock(manager->idle_threads.mutex);
		if (manager->idle_threads.size > 0)
		{
			manager->idle_threads.size--;
			idle_id = manager->idle_threads.idxs[manager->idle_threads.size];
		}
		pthread_mutex_unlock(manager->idle_threads.mutex);

		if (idle_id == -1)
		{
			// no idle thread, try create a new thread and put it to pool
			pthread_t thread_id;
			thread_info *ti = NULL;
			thread_task_arg *arg = (thread_task_arg *)malloc(sizeof(thread_task_arg));
			pthread_mutex_lock(manager->all_threads.mutex);
			if (manager->all_threads.size < manager->easy_tp->max_pool_size)
			{
				manager->all_threads.size++;
				manager->all_threads.unused_size--;
				ti = &manager->all_threads.threads[manager->all_threads.unused_size];
				arg->id = manager->all_threads.unused_size;
				pthread_mutex_unlock(manager->all_threads.mutex);

				ti->active_cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
				pthread_cond_init(ti->active_cond, NULL);
				ti->active_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
				pthread_mutex_init(ti->active_mutex, NULL);
				ti->quit = 0;
				ti->task = NULL;
				arg->ti  = ti;
				arg->man = manager;
				pthread_create(&thread_id, NULL, thread_task_entry, (void *)arg);
				idle_id = arg_id;
			}
			else   // if can not create a new thread, wait until someone thread becomes idle
			{
				pthread_mutex_unlock(manager->all_threads.mutex);
				pthread_mutex_lock(manager->idle_threads.mutex);
				while (manager->idle_threads.size <= 0)
					pthread_cond_wait(manager->idle_threads.cond, manager->idle_threads.mutex);
				manager->idle_threads.size--;
				idle_id = manager->idle_threads.idxs[manager->idle_threads.size];	
				pthread_mutex_unlock(manager->idle_thread.mutex);
			}
		}

		// attention! no lock, ensure the thread pointed by idle_id exist
		{
			// notify the idle thread to run the task
			thread_info *ti;
			ti = &manager->all_threads.threads[idle_id];
			pthread_mutex_lock(ti->mutex);
			ti->task = task;
			pthread_mutex_unlock(ti->mutex);
			pthread_cond_signal(ti->cond);
		}
	}
}

easy_thread_pool *easy_thread_pool_init(int init_pool_size, int max_pool_size)
{
	int i;
	pthread_t thread_id;
	easy_tp_man *manager = (easy_tp_man *)malloc(sizeof(easy_tp_man));
	
	manager->easy_tp = (easy_thread_pool *)malloc(sizeof(easy_thread_pool));
	easy_tp->init_pool_size = init_pool_size;
	easy_tp->max_pool_size  = max_pool_size;

	manager->all_threads.size = init_pool_size;
	manager->all_threads.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(manager->all_threads.mutex, NULL);
	manager->all_threads.cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_cond_init(manager->all_threads.cond, NULL);
	manager->all_threads.threads = (thread_info *)malloc(sizeof(thread_info) * max_pool_size);
	manager->all_threads.unused = (int *)malloc(sizeof(int) * max_pool_size);
	for (i = 0; i < manager->all_threads.size; i++)
	{
		thread_task_arg *arg = (thread_task_arg *)malloc(sizeof(thread_task_arg));
		manager->all_threads.threads[i].active_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(manager->all_threads.thread[i].active_mutex, NULL);
		manager->all_threads.threads[i].active_cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
		pthread_cond_init(manager->all_threads.thread[i].active_cond, NULL);
		manager->all_threads.threads[i].quit = 0;
		manager->all_threads.threads[i].task = NULL;
		arg->ti  = &manager->all_threads.threads[i];
		arg->man = manager;
		arg->id  = i;
		pthread_create(&thread_id, NULL, thread_task_entry, (void *)arg);
	}
	for (i = manager->all_threads.size; i < max_pool_size; i++)
		manager->all_threads.unused[i - manager->all_threads.size] = i;
	manager->all_threads.unused_size = max_pool_size - manager->all_threads.size;

	manager->all_tasks.head = manager->all_tasks.tail = NULL;
	manager->all_tasks.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(manager->all_tasks.mutex, NULL);
	manager->all_tasks.cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_cond_init(manager->all_tasks.cond, NULL);

	manager->idle_threads.size = init_pool_size;
	manager->idle_threads.idxs = (int *)malloc(sizeof(int) * max_pool_size);
	for (i = 0; i < manager->idle_threads.size; i++)
		manager->idle_threads.idxs[i] = i;
	manager->idle_threads.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(manager->idle_threads.mutex, NULL);
	manager->idle_threads.cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_cond_init(manager->idle_threads.cond, NULL);

	pthread_create(&thread_id, NULL, thread_manager_entry, manager); 
	return manager->easy_tp;
}



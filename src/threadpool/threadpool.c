#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>

#define MAX_THREADS_ALLOWED 1024

#ifdef DEBUG
#include <stdio.h>
#define DEBUG_OUTPUT(fmt, str) fprintf(stderr, fmt, str)
#else
#define DEBUG_OUTPUT(fmt, str)
#endif

typedef struct _task_node
{
	task_func                func;
	task_desc                *desc;
	struct _task_node        *next;
}task_node;

typedef struct _task_list
{
	task_node         *head;
	task_node         *tail;
	pthread_mutex_t   *mutex;
	pthread_cond_t    *cond;
}task_list;


typedef struct _thread_info
{
	//void *(*thread_entry)(void*);
	pthread_t        tid;
	pthread_cond_t  *active_cond;
	pthread_mutex_t *active_mutex;
	task_node       *task;
}thread_info;

typedef struct _thread_array
{
	thread_info       *threads;
	int               size;
	int               *unused;
	int               unused_size;

	pthread_mutex_t   *mutex;
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
	easy_thread_pool  easy_tp;
	pthread_t         tid;

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

static void *exit_task_entry(void *arg)
{
	DEBUG_OUTPUT("thread %d exit\n", *(int *)arg); 
}

static void *thread_task_entry(void *arg)
{
	// attention! here no lock
	easy_tp_man *manager = ((thread_task_arg *)(arg))->man;
	thread_info *ti      = ((thread_task_arg *)(arg))->ti;
	int id               = ((thread_task_arg *)(arg))->id;
	
	//
	for (;;)
	{
		task_node *task;
		pthread_mutex_lock(ti->active_mutex);
		while (ti->task == NULL)
			pthread_cond_wait(ti->active_cond, ti->active_mutex);
		task = ti->task;
		ti->task = NULL;
#ifdef DEBUG
		if (task->func != exit_task_entry)
			DEBUG_OUTPUT("task %d is processing\n",*(int*)task->desc->arg);
#endif
		pthread_mutex_unlock(ti->active_mutex);
		if (task->func != exit_task_entry)
		{
			// ASSERT(ti->task);
			task->desc->ret = task->func(task->desc->arg);
			free(task);
			// become idle
			pthread_mutex_lock(manager->idle_threads.mutex);
			manager->idle_threads.idxs[manager->idle_threads.size] = id;
			manager->idle_threads.size++;
			pthread_mutex_unlock(manager->idle_threads.mutex);
			pthread_cond_signal(manager->idle_threads.cond);
		}
		else
		{
			exit_task_entry(&id);
			break;
		}
	}
	free(arg);
	return 0;
}

static void *thread_manager_entry(void *arg)
{
	int idle_id = -1;
	easy_tp_man *manager = (easy_tp_man *)arg;
	task_node *task;

	for (;;)
	{
		// wait until a task coming
		idle_id = -1;
		pthread_mutex_lock(manager->all_tasks.mutex);
		while (manager->all_tasks.head == NULL)
			pthread_cond_wait(manager->all_tasks.cond, manager->all_tasks.mutex);
		task = manager->all_tasks.head;
		if (task->func != exit_task_entry)
			manager->all_tasks.head = manager->all_tasks.head->next;
		pthread_mutex_unlock(manager->all_tasks.mutex);

		// it is a exit task?
		if (task->func == exit_task_entry)
		{
			int j = 0;
			int i = 0;
			// terminate recycle thread firstly
			// ...
			//
			// then, wait all tasks finished, that's to say, wait all work thread become idle
			pthread_mutex_lock(manager->idle_threads.mutex);
			while (manager->idle_threads.size != manager->all_threads.size)
				pthread_cond_wait(manager->idle_threads.cond, manager->idle_threads.mutex);
			pthread_mutex_unlock(manager->idle_threads.mutex);

			// then, terminate all idle threads, recycle thread has exited, so
			// no other thread would own all work threads array, so lock is not need
			for (i = 0; i < manager->easy_tp.max_pool_size && manager->all_threads.size; i++)
			{
				if (manager->all_threads.threads[i].tid != -1)
				{
					manager->all_threads.size--;
					task_node *per_task = (task_node *)malloc(sizeof(task_node));
					per_task->func = exit_task_entry;
					per_task->next = NULL;
					per_task->desc = NULL;
					pthread_mutex_lock(manager->all_threads.threads[i].active_mutex);
#ifdef DEBUG
					if (manager->all_threads.threads[i].task)
						DEBUG_OUTPUT("thread %d task is not null",i);
#endif
					manager->all_threads.threads[i].task = per_task;
					pthread_mutex_unlock(manager->all_threads.threads[i].active_mutex);
					pthread_cond_signal(manager->all_threads.threads[i].active_cond);
					manager->all_threads.unused[j++] = i; 
				}
			}
			manager->all_threads.unused_size = j;
			for (i = 0; i < j; i++)
			{
				pthread_join(manager->all_threads.threads[manager->all_threads.unused[i]].tid, NULL);
			}
			break;
		}

		DEBUG_OUTPUT("task %d is ready to dispatch\n", *(int *)task->desc->arg);
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
			thread_info *ti = NULL;
			thread_task_arg *arg = (thread_task_arg *)malloc(sizeof(thread_task_arg));
			pthread_mutex_lock(manager->all_threads.mutex);
			if (manager->all_threads.size < manager->easy_tp.max_pool_size)
			{
				int unused_idx = 0;
				manager->all_threads.size++;
				unused_idx = manager->all_threads.unused[--manager->all_threads.unused_size];
				ti = &manager->all_threads.threads[unused_idx];
				pthread_mutex_unlock(manager->all_threads.mutex);

				ti->active_cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
				pthread_cond_init(ti->active_cond, NULL);
				ti->active_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
				pthread_mutex_init(ti->active_mutex, NULL);
				ti->task = NULL;
				arg->ti  = ti;
				arg->man = manager;
				arg->id  = unused_idx;

			#ifdef DEBUG
			{
				if (*(int *)task->desc->arg == 19999)
					DEBUG_OUTPUT("task 19999 will be processed by thread %d\n", arg->id);
			}
			#endif

				pthread_create(&ti->tid, NULL, thread_task_entry, (void *)arg);
				idle_id = arg->id;
			}
			else   // if can not create a new thread, wait until someone thread becomes idle
			{
				pthread_mutex_unlock(manager->all_threads.mutex);
				pthread_mutex_lock(manager->idle_threads.mutex);
				while (manager->idle_threads.size <= 0)
					pthread_cond_wait(manager->idle_threads.cond, manager->idle_threads.mutex);
				manager->idle_threads.size--;
				idle_id = manager->idle_threads.idxs[manager->idle_threads.size];	
			#ifdef DEBUG
			{
				if (*(int *)task->desc->arg == 19999)
					DEBUG_OUTPUT("task 19999 will be processed by thread %d\n", idle_id);
			}
			#endif
				pthread_mutex_unlock(manager->idle_threads.mutex);
			}
		}

		// attention! no lock, ensure the thread pointed by idle_id exist
		{
			// notify the idle thread to run the task
			thread_info *ti;
			ti = &manager->all_threads.threads[idle_id];

			#ifdef DEBUG
			{
				if (*(int *)task->desc->arg == 19999)
					DEBUG_OUTPUT("task 19999 will be processed by thread %d\n", idle_id);
			}
			#endif
			pthread_mutex_lock(ti->active_mutex);
			ti->task = task;
			pthread_mutex_unlock(ti->active_mutex);
			pthread_cond_signal(ti->active_cond);
		}
	}
}

easy_thread_pool *easy_thread_pool_init(int init_pool_size, int max_pool_size)
{
	int i;
	easy_tp_man *manager = (easy_tp_man *)malloc(sizeof(easy_tp_man));
	
	manager->easy_tp.init_pool_size = init_pool_size;
	manager->easy_tp.max_pool_size  = max_pool_size;
    
	// init all threads
	manager->all_threads.size = init_pool_size;
	manager->all_threads.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(manager->all_threads.mutex, NULL);
	manager->all_threads.threads = (thread_info *)malloc(sizeof(thread_info) * max_pool_size);
	manager->all_threads.unused = (int *)malloc(sizeof(int) * max_pool_size);
	for (i = 0; i < manager->all_threads.size; i++)
	{
		thread_task_arg *arg = (thread_task_arg *)malloc(sizeof(thread_task_arg));
		manager->all_threads.threads[i].active_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(manager->all_threads.threads[i].active_mutex, NULL);
		manager->all_threads.threads[i].active_cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
		pthread_cond_init(manager->all_threads.threads[i].active_cond, NULL);
		manager->all_threads.threads[i].task = NULL;
		arg->ti  = &manager->all_threads.threads[i];
		arg->man = manager;
		arg->id  = i;
		pthread_create(&manager->all_threads.threads[i].tid, NULL, thread_task_entry, (void *)arg);
	}
	for (i = manager->all_threads.size; i < max_pool_size; i++)
	{
		manager->all_threads.unused[i - manager->all_threads.size] = i;
		manager->all_threads.threads[i].tid = -1;
	}
	manager->all_threads.unused_size = max_pool_size - manager->all_threads.size;
    
	// init all tasks
	manager->all_tasks.head = manager->all_tasks.tail = NULL;
	manager->all_tasks.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(manager->all_tasks.mutex, NULL);
	manager->all_tasks.cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_cond_init(manager->all_tasks.cond, NULL);


    // init idle threads
	manager->idle_threads.size = init_pool_size;
	manager->idle_threads.idxs = (int *)malloc(sizeof(int) * max_pool_size);
	for (i = 0; i < manager->idle_threads.size; i++)
		manager->idle_threads.idxs[i] = i;
	manager->idle_threads.mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(manager->idle_threads.mutex, NULL);
	manager->idle_threads.cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
	pthread_cond_init(manager->idle_threads.cond, NULL);

	if (pthread_create(&manager->tid, NULL, thread_manager_entry, manager) != 0)
		perror("manger thread create failed :");
	return (easy_thread_pool *)manager;
}

void easy_thread_pool_add_task(easy_thread_pool *easy_tp, task_func func, task_desc *task_info)
{
	int hasFree = 0;
	easy_tp_man *manager = (easy_tp_man *)easy_tp;
	task_node *task = (task_node *)malloc(sizeof(task_node));
	task->func = func;
	task->desc = task_info;
	task->next = NULL;
	pthread_mutex_lock(manager->all_tasks.mutex);
	if (manager->all_tasks.head == NULL)
		manager->all_tasks.head = manager->all_tasks.tail = task;
	else if (manager->all_tasks.tail->func != exit_task_entry)
	{
		manager->all_tasks.tail->next = task;
		manager->all_tasks.tail = task;
	}
	else
		hasFree = 1;
	pthread_mutex_unlock(manager->all_tasks.mutex);
	if (!hasFree)
		pthread_cond_signal(manager->all_tasks.cond);
	else
		free(task);
}

void easy_thread_pool_free(easy_thread_pool *easy_tp)
{
	easy_thread_pool_add_task(easy_tp, exit_task_entry, NULL);
	easy_tp_man *manager = (easy_tp_man *)easy_tp;
	pthread_join(manager->tid, NULL);

    // free idle threads
	pthread_mutex_destroy(manager->idle_threads.mutex);
	free(manager->idle_threads.mutex);
	pthread_cond_destroy(manager->idle_threads.cond);
	free(manager->idle_threads.cond);
	free(manager->idle_threads.idxs);


    // free all tasks
	pthread_mutex_destroy(manager->all_tasks.mutex);
	free(manager->all_tasks.mutex);
	pthread_cond_destroy(manager->all_tasks.cond);
	free(manager->all_tasks.cond);
	while (manager->all_tasks.head)
	{
		task_node *cur = manager->all_tasks.head;
		manager->all_tasks.head = cur->next;
		free(cur);
	}

    // free all threads
	{
		int i;
		for (i = 0; i < manager->all_threads.unused_size; i++)
		{
			int id = manager->all_threads.unused[i];
			pthread_mutex_destroy(manager->all_threads.threads[id].active_mutex);
			free(manager->all_threads.threads[id].active_mutex);
			pthread_cond_destroy(manager->all_threads.threads[id].active_cond);
			free(manager->all_threads.threads[id].active_cond);
		}
		free(manager->all_threads.unused);
		free(manager->all_threads.threads);
		pthread_mutex_destroy(manager->all_threads.mutex);
		free(manager->all_threads.mutex);
	}

	free(manager);
}


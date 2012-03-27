#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>


#define TASK_NUM 20000
char *finish_flags;
void *task_entry_1(void *arg)
{
//	printf("I am task %d, task run\n", *(int *)arg);
	// do something
	{
		int i = 0;
		int a = 0;
		for (i = 0; i < 100000; i++)
		{
			a += i;
			a /= (i + 1);
			a *= i;
		}
		printf("%d",a);
	}
	finish_flags[*(int *)arg] = 1;
//	printf("I am task %d, task finished\n", *(int *)arg);
	return 0;
}

int main(int argc, char *argv[])
{
	int i;
	struct timeval start, end;
	int all_finished = 1;

	finish_flags = (char *)malloc(sizeof(char) * TASK_NUM);
	memset(finish_flags, 0, sizeof(char) * TASK_NUM);

	task_desc *descs = (task_desc *)malloc(sizeof(task_desc) * TASK_NUM);

	gettimeofday(&start, NULL);
	if (argc > 1)
	{	//single-thread
		//
		int a = 0;
		for (i = 0; i < 20000 * 100000; i++)
		{
			a += i;
			a /= (i + 1);
			a *= i;
		}
		printf("%d",a);
	}
	else
	{
		//multi-thread
		//
		easy_thread_pool *tp = easy_thread_pool_init(100, 300);

		for (i = 0; i < TASK_NUM; i++)
		{
			descs[i].arg = malloc(sizeof(int));
			descs[i].ret = malloc(sizeof(int));
			*(int *)descs[i].arg = i;

			easy_thread_pool_add_task(tp, task_entry_1, &descs[i]);
		}

		easy_thread_pool_free(tp);
	}
	gettimeofday(&end, NULL);

	printf("time used : %f seconds\n",
	((end.tv_sec - start.tv_sec) * 1000000 + end.tv_usec - start.tv_usec) / 1000000.0);

	for (i = 0; i < TASK_NUM; i++)
	{
		free(descs[i].arg);
		free(descs[i].ret);
	}
	free(descs);

	for (i = 0; i < TASK_NUM; i++)
	{
		if (!finish_flags[i])
		{
			printf("task %d is not finished\n", i);
			all_finished = 0;
		}
	}
	if (all_finished)
		printf("all task finished successfully\n");
	pthread_exit(NULL);
	return 0;
}

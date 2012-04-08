#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>


#define TASK_NUM 200
char *finish_flags;
void *task_entry_1(void *arg)
{
//	printf("I am task %d, task run\n", *(int *)arg);
	// do something
	char a = 0;
	{
		int i = 0;
		for (i = 0; i < 10000000; i++)
		{
			// 确保计算不被编译器优化
			a += i;
			a /= (i + 1);
			a *= i;
		}
	}
	finish_flags[*(int *)arg] = a? a : 1;
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
		char a = 0;
		for (i = 0; i < 20000 * 100000; i++)
		{
			a += i;
			a /= (i + 1);
			a *= i;
		}
		printf("%d", a);
	}
	else
	{
		//multi-thread
		//
		easy_thread_pool *tp = easy_thread_pool_init(2, 320);

		for (i = 0; i < TASK_NUM; i++)
		{
			descs[i].arg = malloc(sizeof(int));
			*(int *)descs[i].arg = i;
			descs[i].fire_task_over = NULL;

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
	}
	free(descs);

	if (argc == 1)
	{
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
	}
	return 0;
}

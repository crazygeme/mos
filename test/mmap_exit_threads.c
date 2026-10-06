#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define THREADS 8
#define ROUNDS 32
#define SIZE (4UL * 1024 * 1024)
static pthread_barrier_t barrier;

static void *worker(void *arg)
{
	(void)arg;
	pthread_barrier_wait(&barrier);
	for (;;) {
		void *area = mmap(NULL, SIZE, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (area == MAP_FAILED)
			_exit(2);
		if (mprotect(area, 4096, PROT_READ | PROT_WRITE))
			_exit(2);
		*(volatile uintptr_t *)area = 1;
		if (munmap(area, SIZE))
			_exit(2);
	}
	return NULL;
}

int main(void)
{
	setbuf(stdout, NULL);
	for (unsigned round = 0; round < ROUNDS; round++) {
		pid_t child = fork();
		if (child < 0)
			return 3;
		if (!child) {
			pthread_t threads[THREADS];
			if (pthread_barrier_init(&barrier, NULL, THREADS + 1))
				_exit(3);
			for (unsigned i = 0; i < THREADS; i++)
				if (pthread_create(&threads[i], NULL, worker, NULL))
					_exit(3);
			pthread_barrier_wait(&barrier);
			usleep(1000 + round * 100);
			_exit(0);
		}
		int status;
		if (waitpid(child, &status, 0) != child || status != 0) {
			printf("Mapping exit checks: FAIL (round %u).\n", round);
			return 1;
		}
	}
	printf("Mapping exit checks: PASS (%d rounds, %d threads).\n", ROUNDS, THREADS);
	return 0;
}

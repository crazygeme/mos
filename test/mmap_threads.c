#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define N 8
#define ROUNDS 512
#define SIZE (4UL * 1024 * 1024)
static pthread_barrier_t barrier;
static void *areas[N];
static void *worker(void *arg) {
 uintptr_t id=(uintptr_t)arg;
 for(int round=0;round<ROUNDS;round++) {
  pthread_barrier_wait(&barrier);
  areas[id]=mmap(NULL,SIZE,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  pthread_barrier_wait(&barrier);
  if(!id) {
   for(int i=0;i<N;i++) for(int j=i+1;j<N;j++)
    if(areas[i]!=MAP_FAILED && areas[j]!=MAP_FAILED &&
       (uintptr_t)areas[i] < (uintptr_t)areas[j] + SIZE &&
       (uintptr_t)areas[j] < (uintptr_t)areas[i] + SIZE) {
     printf("MMAP_COLLISION round=%d threads=%d,%d address=%p\n",round,i,j,areas[i]);fflush(stdout);_exit(1);
    }
  }
  pthread_barrier_wait(&barrier);
  if(areas[id]==MAP_FAILED) {perror("mmap");_exit(2);}
  if(mprotect(areas[id],4096,PROT_READ|PROT_WRITE)) {perror("mprotect");_exit(2);}
  *(volatile uintptr_t*)areas[id]=id+1;
  pthread_barrier_wait(&barrier);
  if(*(volatile uintptr_t*)areas[id]!=id+1) {puts("MMAP_DATA_COLLISION");_exit(1);}
  pthread_barrier_wait(&barrier);
  if(munmap(areas[id],SIZE)) {perror("munmap");_exit(2);}
 }
 return NULL;
}
int main(void) {
 pthread_t threads[N];setbuf(stdout,NULL);pthread_barrier_init(&barrier,NULL,N);
 for(uintptr_t i=0;i<N;i++) if(pthread_create(&threads[i],NULL,worker,(void*)i)) return 3;
 for(int i=0;i<N;i++)pthread_join(threads[i],NULL);
 printf("Mapping concurrency checks: PASS (%d threads, %d rounds).\n",N,ROUNDS);return 0;
}

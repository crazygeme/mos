#include <pthread.h>
#include <sys/random.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define THREADS 8
#define COUNT 10000
static unsigned char ids[THREADS*COUNT][16];
static pthread_barrier_t barrier;
static void*run(void*arg){long k=(long)arg;for(int i=0;i<COUNT;i++){if(i%64==0)pthread_barrier_wait(&barrier);if(getrandom(ids[k*COUNT+i],16,0)!=16)abort();}return NULL;}
static int cmp(const void*a,const void*b){return memcmp(a,b,16);}
int main(){pthread_t threads[THREADS];pthread_barrier_init(&barrier,NULL,THREADS);for(long i=0;i<THREADS;i++)pthread_create(&threads[i],NULL,run,(void*)i);for(int i=0;i<THREADS;i++)pthread_join(threads[i],NULL);qsort(ids,THREADS*COUNT,16,cmp);int duplicates=0;for(int i=1;i<THREADS*COUNT;i++)if(!memcmp(ids[i-1],ids[i],16))duplicates++;printf("Identifiers: %d; duplicates: %d\n",THREADS*COUNT,duplicates);return duplicates?1:0;}

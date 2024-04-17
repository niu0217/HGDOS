#include <stddef.h>

typedef unsigned int key_t;

struct struct_shmem
{
    unsigned int size;  //共享内存大小
    unsigned int key;   //标识这个共享内存
    unsigned long page; //共享内存的开始地址
};

int shmget(key_t key, size_t size);
void *shmat(int shmid);

#define SHM_NUM 16
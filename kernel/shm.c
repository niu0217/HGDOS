#include <shm.h>
#include <linux/mm.h>
#include <unistd.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>

struct struct_shmem shm_list[SHM_NUM] = {{0, 0, 0}};

// 获得一个空闲的物理页面
int sys_shmget(key_t key, size_t size)
{
    int i;
    unsigned long page;

    if (size > PAGE_SIZE)
    {
        errno = EINVAL;
        printk("shmget:The size connot be greater than the PAGE_SIZE!\r\n");
        return -1;
    }

    if (key == 0)
    {
        printk("shmget:key connot be 0!\r\n");
        return -1;
    }

    // 判断是否已经创建
    for (i = 0; i < SHM_NUM; i++)
    {
        if (shm_list[i].key == key)
            return i;
    }

    page = get_free_page(); // 申请内存页
    if (!page)
    {
        errno = ENOMEM;
        printk("shmget:connot get free page!\r\n");
        return -1;
    }

    for (i = 0; i < SHM_NUM; i++)
    {
        if (shm_list[i].key == 0)
        {
            shm_list[i].size = size;
            shm_list[i].key = key;
            shm_list[i].page = page;
            break;
        }
    }
    return i;
}

// 将这个页面和进程的虚拟地址以及逻辑地址关联起来，让进程对某个逻辑地址的读写就是在读写该内存页
void *sys_shmat(int shmid)
{
    unsigned long tmp; // 虚拟地址
    unsigned long logicalAddr;
    if (shmid < 0 || shmid >= SHM_NUM || shm_list[shmid].page == 0 || shm_list[shmid].key <= 0)
    {
        errno = EINVAL;
        printk("shmat:The shmid id invalid!\r\n");
        return NULL;
    }
    tmp = get_base(current->ldt[1]) + current->brk; // 计算虚拟地址
    put_page(shm_list[shmid].page, tmp);
    logicalAddr = current->brk; // 记录逻辑地址
    current->brk += PAGE_SIZE;  // 更新brk指针
    return (void *)logicalAddr;
}
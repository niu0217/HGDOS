/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <signal.h>

volatile void do_exit(int error_code);

// 获取当前任务信号屏蔽位图（屏蔽码或阻塞码）
int sys_sgetmask()
{
	return current->blocked;
}

// 设置新的信号屏蔽图。SIGKILL不能被屏蔽。返回值是原信号屏蔽位图
int sys_ssetmask(int newmask)
{
	int old=current->blocked;

	current->blocked = newmask & ~(1<<(SIGKILL-1));
	return old;
}

// 复制sigaction数据到fs数据段to处。即从内核空间复制到用户（任务）数据段中
static inline void save_old(char * from,char * to)
{
	int i;

	verify_area(to, sizeof(struct sigaction)); //验证to处的内存空间是否足够大
	for (i=0 ; i< sizeof(struct sigaction) ; i++) {
		put_fs_byte(*from,to);
		from++;
		to++;
	}
}

// 把sigaction数据从fs数据段from位置复制到to处。即从用户数据空间复制到内核数据段中
static inline void get_new(char * from,char * to)
{
	int i;

	for (i=0 ; i< sizeof(struct sigaction) ; i++)
		*(to++) = get_fs_byte(from++);
}

// signal信号调用。为指定的信号安装新的信号句柄（信号处理程序）
int sys_signal(int signum, long handler, long restorer)
{
	struct sigaction tmp;

	// 信号值在有效范围内（1-32）并且不能是信号SIGKILL（和SIGSTOP）
	// 因为这两个信号不能被进程捕获
	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	// 根据提供的参数组建sigaction结构内容。sa_handler是指定的信号处理句柄（函数）
	// sa_mask是执行信号处理句柄时的信号屏蔽码；sa_flags是执行时的一些标志组合，
	// 这里设定该信号处理句柄只使用一次后就恢复到默认值，并允许信号在自己的处理句柄中收到
	tmp.sa_handler = (void (*)(int)) handler;
	tmp.sa_mask = 0;
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
	tmp.sa_restorer = (void (*)(void)) restorer;	// 保存恢复处理函数指针
	// 取该信号原来的处理句柄，并设置该信号的sigaction结构。最后返回原信号句柄
	handler = (long) current->sigaction[signum-1].sa_handler;
	current->sigaction[signum-1] = tmp;
	return handler;
}

// sigaction()系统调用。改变进程在收到一个信号时的操作。signum是除了SIGKILL以外的任何信号
// 如果action不为空，则新操作被安装；如果oldaction指针不为空，则原操作将被保留到oldaction
int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction tmp;

	// 信号值要在（1-32）范围内，并且SIGKILL不能被改变
	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	// 在信号的sigaction结构中设置新的操作（动作）。如果oldaction指针不为空的话，则将
	// 原操作指针保存到oldaction所指的位置
	tmp = current->sigaction[signum-1];
	get_new((char *) action,
		(char *) (signum-1+current->sigaction));
	if (oldaction)
		save_old((char *) &tmp,(char *) oldaction);
	// 如果允许信号在自己的信号句柄中收到，则令屏蔽码为0，否则设置屏蔽本信号
	if (current->sigaction[signum-1].sa_flags & SA_NOMASK)
		current->sigaction[signum-1].sa_mask = 0;
	else
		current->sigaction[signum-1].sa_mask |= (1<<(signum-1));
	return 0;
}

// 系统调用的中断处理程序中真正的信号预处理程序
// 主要作用：将信号处理句柄插入到用户程序堆栈中，并在本系统调用结束返回后立即执行信号句柄程序，然后继续执行用户的程序
// CPU执行中断指令压入的用户栈地址ss和esp、标志寄存器eflags和返回地址cs和eip
// 在刚进入system_call时压入栈的寄存器ds、es、fs和edx、ecx、ebx
void do_signal(long signr,long eax, long ebx, long ecx, long edx,
	long fs, long es, long ds,
	long eip, long cs, long eflags,
	unsigned long * esp, long ss)
{
	unsigned long sa_handler;
	long old_eip=eip;
	struct sigaction * sa = current->sigaction + signr - 1;
	int longs;
	unsigned long * tmp_esp;

	// 如果信号句柄为SIG_IGN（1，默认忽略句柄）则不对信号进行处理而直接返回。
	// 如果信号句柄为SIG_DEL（0，默认处理）则如果信号是SIGCHLD也直接返回，否则终止进程的执行
	// SIG_IGN定义为1 SIG_DEL定义为0
	// 126行do_exit()的参数是返回码和程序提供的退出状态信息。可作为wait()或waitpid()函数
	// 的状态信息。wait()或waitpid()函数利用这些宏就可以取得子进程的退出状态码或子进程终止的原因（信号）
	sa_handler = (unsigned long) sa->sa_handler;
	if (sa_handler==1)
		return;
	if (!sa_handler) {
		if (signr==SIGCHLD)
			return;
		else
			do_exit(1<<(signr-1));
	}
	// 以下准备对信号句柄的调用设置。如果该信号句柄只需使用一次，则将该句柄置空。
	// 注意：该信号句柄已经保存在sa_handler指针中
	// 在系统调用进入内核时，用户程序返回地址（eip、cs）被保存在内核态栈中。下面这段代码
	// 修改内核态堆栈上用户调用系统调用时的代码指针eip为指向信号处理句柄，同时也将sa_restorer、
	// signr、进程屏蔽码（如果SA_NOMASK没置位）、eax、ecx、edx作为参数以及原调用系统调用的程序
	// 返回指针及标志寄存器值压入用户堆栈。因此在本次系统调用中断返回用户程序时会首先执行用户的
	// 信号句柄程序，然后在继续执行用户程序
	if (sa->sa_flags & SA_ONESHOT)
		sa->sa_handler = NULL;
	// 将内核态堆栈上用户调用系统调用下一条代码指令指针eip指向该信号处理句柄。由于C函数是传值函数，
	// 因此给eip赋值时需要使用*(&eip)这样的形式。另外如果允许信号自己的处理句柄收到信号自己，则也需要
	// 将进程的阻塞码压入堆栈。
	// 特别注意：*(&eip) = sa_handler;对普通的C函数参数进行修改是不起作用的。因为当函数返回时堆栈
	// 上的参数将被调用者丢弃。这里之所以可以使用这种方式，是因为该函数是从汇编程序中被调用的，并且在
	// 函数返回后汇编程序并没有把调用do_signal()时的所有参数都丢弃。eip等仍然在堆栈中。
	// sigaction结构的sa_mask字段给出了在当前信号句柄（信号描述符）程序执行期间应该被屏蔽的信号集。
	// 同时，引起本信号句柄执行的信号也会被屏蔽。不过若sa_flags中使用了SA_NOMASK标志，那么引起本信号
	// 句柄执行的信号将不会被屏蔽掉。如果允许信号自己的处理句柄收到信号自己，则也需要将进程的信号阻塞码
	// 压入堆栈
	*(&eip) = sa_handler;
	longs = (sa->sa_flags & SA_NOMASK)?7:8;
	// 将原调用程序的用户堆栈指针向下扩展7（或8）个长字（用来存放调用信号句柄的参数等）
	// 并检查内存使用情况（例如如果内存超界则分配新页等）
	*(&esp) -= longs;
	verify_area(esp,longs*4);
	// 在用户堆栈中从下到上存放sa_restorer、信号signr、屏蔽码blocked（如果SA_NOMASK置位）
	// eax、ecx、edx、eflags和用户程序原代码指针
	tmp_esp=esp;
	put_fs_long((long) sa->sa_restorer,tmp_esp++);
	put_fs_long(signr,tmp_esp++);
	if (!(sa->sa_flags & SA_NOMASK))
		put_fs_long(current->blocked,tmp_esp++);
	put_fs_long(eax,tmp_esp++);
	put_fs_long(ecx,tmp_esp++);
	put_fs_long(edx,tmp_esp++);
	put_fs_long(eflags,tmp_esp++);
	put_fs_long(old_eip,tmp_esp++);
	current->blocked |= sa->sa_mask;	//进程阻塞码（屏蔽码）添上sa_mask中的码位
}

#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"

// 引入用户初始化代码
#include "../../user/initcode.h"

// 外部汇编与函数声明
extern char trampoline[];
extern void swtch(context_t *old, context_t *new);
extern void trap_user_return();

// --- 全局控制 ---

// 调度器调试开关：置 1 开启调度日志，置 0 关闭（解决刷屏问题）
#define SCHED_TRACE 0

// --- 静态资源管理 ---

// 进程池：所有进程控制块的仓库
static proc_t proc_pool[N_PROC];
// 指向首个用户进程（通常是 init）
static proc_t *init_process;

// PID 分配锁与计数器
static spinlock_t pid_lock;
static int next_pid = 1;

// 进程生命周期锁：用于保护 wait/exit 操作中的进程树关系
static spinlock_t lifecycle_lock;

// --- 内部函数 ---

// 分配一个新的 PID
static int allocate_pid()
{
    int pid;
    spinlock_acquire(&pid_lock);
    pid = next_pid++;
    spinlock_release(&pid_lock);
    return pid;
}

// 进程初次运行的入口函数 (内核态 -> 用户态)
static void proc_entry_point()
{
    // 释放调度器切换过来时持有的进程锁
    spinlock_release(&myproc()->lk);
    // 返回用户空间
    trap_user_return();
}

// 进程模块初始化
void proc_init()
{
    spinlock_init(&pid_lock, "pid_allocator");
    spinlock_init(&lifecycle_lock, "proc_lifecycle");
    
    // 初始化进程池
    for (int i = 0; i < N_PROC; i++) {
        spinlock_init(&proc_pool[i].lk, "proc_lock");
        proc_pool[i].state = UNUSED;
        // 预先计算好每个进程的内核栈基址
        proc_pool[i].kstack = KSTACK(i);
    }
}

// 初始化进程页表：映射 trampoline 和 trapframe
pgtbl_t proc_pgtbl_init(uint64 tf_va)
{
    pgtbl_t tbl = (pgtbl_t)pmem_alloc(true);
    if (!tbl) return NULL;
    memset(tbl, 0, PGSIZE);

    // 映射跳板页 (trampoline) - 执行权限
    vm_mappages(tbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    // 映射 trapframe - 读写权限
    vm_mappages(tbl, TRAPFRAME, tf_va, PGSIZE, PTE_R | PTE_W);

    return tbl;
}

// 申请一个空闲进程块
// 返回时持有进程锁，失败返回 NULL
proc_t *proc_alloc()
{
    proc_t *p = NULL;

    // 遍历寻找 UNUSED 状态的进程槽位
    for (int i = 0; i < N_PROC; i++) {
        spinlock_acquire(&proc_pool[i].lk);
        if (proc_pool[i].state == UNUSED) {
            p = &proc_pool[i];
            break;
        }
        spinlock_release(&proc_pool[i].lk);
    }

    if (!p) return NULL;

    // 初始化元数据
    p->pid = allocate_pid();
    p->state = UNUSED; // 暂时保持 UNUSED，直到完全初始化

    // 分配 trapframe 物理页
    if ((p->tf = (trapframe_t *)pmem_alloc(true)) == NULL) {
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);

    // 初始化用户页表
    if ((p->pgtbl = proc_pgtbl_init((uint64)p->tf)) == NULL) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
        spinlock_release(&p->lk);
        return NULL;
    }

    // 设置内核上下文，为第一次 swtch 做准备
    memset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.ra = (uint64)proc_entry_point; // swtch 返回后跳转这里
    p->ctx.sp = p->kstack + PGSIZE;       // 设置内核栈顶

    // 清理其他字段
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->mmap = NULL;
    memset(p->name, 0, sizeof(p->name));

    return p;
}

// 释放进程资源 (调用者需持有 p->lk)
void proc_free(proc_t *p)
{
    if (p->tf) pmem_free((uint64)p->tf, true);
    p->tf = NULL;

    if (p->pgtbl) {
        // 1. 解除用户空间映射 (代码、数据)
        vm_unmappages(p->pgtbl, USER_BASE, PGSIZE, true);
        
        // 2. 解除堆映射
        if (p->heap_top > USER_BASE + PGSIZE)
             vm_unmappages(p->pgtbl, USER_BASE + PGSIZE, p->heap_top - (USER_BASE + PGSIZE), true);
        
        // 3. 解除栈映射
        if (p->ustack_npage > 0)
            vm_unmappages(p->pgtbl, TRAPFRAME - p->ustack_npage * PGSIZE, p->ustack_npage * PGSIZE, true);
        
        // 4. 解除 mmap 映射
        mmap_region_t *m = p->mmap;
        while (m) {
            vm_unmappages(p->pgtbl, m->begin, m->npages * PGSIZE, true);
            mmap_region_t *next = m->next;
            mmap_region_free(m);
            m = next;
        }
        p->mmap = NULL;
        
        // 5. 解除系统页映射 (不释放物理内存)
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
        
        // 6. 释放页表本身
        pmem_free((uint64)p->pgtbl, true);
    }
    p->pgtbl = NULL;
    p->pid = 0;
    p->parent = NULL;
    p->name[0] = 0;
    p->state = UNUSED;
    
    spinlock_release(&p->lk);
}

// 构建第一个用户进程 (proczero)
void proc_make_first()
{
    proc_t *p = proc_alloc();
    init_process = p;

    // 拷贝 initcode 到用户空间
    void *code_mem = pmem_alloc(false);
    memmove(code_mem, target_user_initcode, target_user_initcode_len);
    vm_mappages(p->pgtbl, USER_BASE, (uint64)code_mem, PGSIZE, PTE_R|PTE_W|PTE_X|PTE_U);

    // 分配并映射用户栈 (1页)
    void *stack_mem = pmem_alloc(false);
    vm_mappages(p->pgtbl, TRAPFRAME - PGSIZE, (uint64)stack_mem, PGSIZE, PTE_R|PTE_W|PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USER_BASE + PGSIZE;

    // 配置 Trapframe 以便返回用户态
    p->tf->user_to_kern_epc = USER_BASE;      // PC 指向代码段
    p->tf->sp = TRAPFRAME;                    // SP 指向用户栈顶 (TRAPFRAME下方的虚拟地址)
    p->tf->user_to_kern_satp = r_satp();
    p->tf->user_to_kern_sp = p->kstack + PGSIZE; // 保存内核栈顶
    extern void trap_user_handler();
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_hartid = r_tp();

    // 设置名称
    char *s = "proczero";
    for(int i=0; s[i]; i++) p->name[i] = s[i];

    p->state = RUNNABLE;
    spinlock_release(&p->lk);
}

// 创建子进程 (Fork)
int proc_fork()
{
    proc_t *curr = myproc();
    proc_t *child = proc_alloc(); // 返回时持有 child->lk
    if (!child) return -1;

    // 1. 复制地址空间 (页表 + 物理页)
    uvm_copy_pgtbl(curr->pgtbl, child->pgtbl, curr->heap_top, curr->ustack_npage, curr->mmap);
    child->heap_top = curr->heap_top;
    child->ustack_npage = curr->ustack_npage;

    // 2. 复制 mmap 管理结构
    mmap_region_t *src = curr->mmap;
    mmap_region_t **dst = &child->mmap;
    while (src) {
        mmap_region_t *new_node = mmap_region_alloc();
        new_node->begin = src->begin;
        new_node->npages = src->npages;
        new_node->next = NULL;
        *dst = new_node;
        dst = &new_node->next;
        src = src->next;
    }

    // 3. 复制 Trapframe
    *(child->tf) = *(curr->tf);
    child->tf->a0 = 0; // 子进程返回值为 0
    
    // [重要] 修正子进程的内核栈指针，否则会踩踏父进程栈
    child->tf->user_to_kern_sp = child->kstack + PGSIZE;

    // 4. 复制其他属性
    for(int i=0; i<16; i++) child->name[i] = curr->name[i];
    child->parent = curr;
    
    int pid = child->pid;
    child->state = RUNNABLE;
    spinlock_release(&child->lk);
    
    return pid; // 父进程返回子进程 PID
}

// 调度器辅助：切换到调度器上下文
void proc_sched()
{
    swtch(&myproc()->ctx, &mycpu()->ctx);
}

// 进程主动让出 CPU (Yield)
void proc_yield()
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 唤醒机制
// 唤醒所有在 chan 上等待的进程
void proc_wakeup(void *chan)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_pool[i];
        if (p != myproc()) {
            spinlock_acquire(&p->lk);
            if (p->state == SLEEPING && p->sleep_space == chan) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}

// 睡眠机制
// 释放保护锁 lk，进程进入睡眠，唤醒后重新获取 lk
void proc_sleep(void *chan, spinlock_t *lk)
{
    proc_t *p = myproc();
    
    // 必须持有进程锁才能修改状态和切换
    // 为了避免死锁，先获取进程锁，再释放传入的外部锁
    spinlock_acquire(&p->lk);
    spinlock_release(lk);

    p->sleep_space = chan;
    p->state = SLEEPING;

    proc_sched(); // 切换 CPU

    // 醒来后清理
    p->sleep_space = NULL;
    
    spinlock_release(&p->lk);
    // 恢复外部锁
    spinlock_acquire(lk);
}

// 调度器主循环
void proc_scheduler()
{
    cpu_t *c = mycpu();
    c->proc = NULL;

    for (;;) {
        // 开启中断，避免调度器空转时无法响应中断
        intr_on();

        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &proc_pool[i];
            
            spinlock_acquire(&p->lk);
            
            if (p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;
                
                // [DEBUG] 仅在开启追踪时打印，避免 Test-1 刷屏
                #if SCHED_TRACE
                printf("proc %d is running...\n", p->pid);
                #endif
                
                swtch(&c->ctx, &p->ctx);
                
                // 进程切换回来，清理 CPU 引用
                c->proc = NULL;
            }
            
            spinlock_release(&p->lk);
        }
    }
}

// 进程退出
void proc_exit(int code)
{
    proc_t *curr = myproc();
    if (curr == init_process) panic("init process exiting");

    // 获取生命周期锁，处理父子关系
    spinlock_acquire(&lifecycle_lock);

    // 1. 将所有子进程过继给 init_process
    for (int i = 0; i < N_PROC; i++) {
        if (proc_pool[i].parent == curr) {
            proc_pool[i].parent = init_process;
            // 如果该子进程已经是僵尸，唤醒新父亲 (init_process)
            spinlock_acquire(&proc_pool[i].lk);
            if (proc_pool[i].state == ZOMBIE) {
                 // init_process 应该在 lifecycle_lock 或自身指针上等待
                 // 这里简化：唤醒在 lifecycle_lock 上等待的进程（包括 init）
                 proc_wakeup(init_process); 
            }
            spinlock_release(&proc_pool[i].lk);
        }
    }

    spinlock_acquire(&curr->lk);
    curr->exit_code = code;
    curr->state = ZOMBIE;
    
    // 2. 唤醒父进程
    // 父进程 wait 时是在父进程自身地址上等待的吗？
    // 为了配合 proc_wait 的修改，这里唤醒 parent 即可
    proc_wakeup(curr->parent); 

    spinlock_release(&curr->lk);
    // 调度前释放生命周期锁
    spinlock_release(&lifecycle_lock);

    // 带着进程锁进入调度器（调度器会释放它）
    spinlock_acquire(&curr->lk);
    proc_sched();
    panic("zombie process revived");
}

// 等待子进程退出
// 返回子进程 PID，并拷贝退出码
int proc_wait(uint64 addr)
{
    spinlock_acquire(&lifecycle_lock);

    for (;;) {
        int have_kids = 0;
        proc_t *curr = myproc();

        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &proc_pool[i];
            if (p->parent != curr) continue;
            
            have_kids = 1;
            spinlock_acquire(&p->lk);
            if (p->state == ZOMBIE) {
                // 找到僵尸子进程，回收
                int pid = p->pid;
                int code = p->exit_code;
                
                // 打印要求的唤醒日志 (Test-4)
                printf("proc %d is wakeup!\n", curr->pid);

                proc_free(p); // free 会释放 p->lk
                spinlock_release(&lifecycle_lock);
                
                if (addr != 0) {
                    uvm_copyout(curr->pgtbl, addr, (uint64)&code, sizeof(int));
                }
                return pid;
            }
            spinlock_release(&p->lk);
        }

        if (!have_kids) {
            spinlock_release(&lifecycle_lock);
            return -1;
        }

        // 有子进程但都在运行，睡眠等待
        // 睡眠通道使用当前进程指针，避免全局冲突
        proc_sleep(curr, &lifecycle_lock);
    }
}
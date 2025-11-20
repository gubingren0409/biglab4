#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"

#include "../../user/initcode.h"

extern char trampoline[];
extern void swtch(context_t *old, context_t *new);
extern void trap_user_return();

// --- 全局数据 ---

// 进程池
static proc_t process_pool[N_PROC];
// 初始进程指针
static proc_t *init_proc;

// PID 生成锁
static spinlock_t pid_gen_lock;
static int next_pid = 1;

// 进程树同步锁（用于 wait/exit/reparent）
static spinlock_t tree_lock;

// --- 内部辅助函数 ---

static int new_pid()
{
    int pid;
    spinlock_acquire(&pid_gen_lock);
    pid = next_pid++;
    spinlock_release(&pid_gen_lock);
    return pid;
}

// 新进程的内核入口
static void fork_ret()
{
    spinlock_release(&myproc()->lk);
    trap_user_return();
}

// 初始化进程管理
void proc_init()
{
    spinlock_init(&pid_gen_lock, "pid_gen");
    spinlock_init(&tree_lock, "proc_tree");
    
    for (int i = 0; i < N_PROC; i++) {
        spinlock_init(&process_pool[i].lk, "proc_entry");
        process_pool[i].state = UNUSED;
        // 预计算内核栈地址
        process_pool[i].kstack = KSTACK(i);
    }
}

// 初始化进程页表 (映射 trampoline 和 trapframe)
pgtbl_t proc_pgtbl_init(uint64 tf_va)
{
    pgtbl_t tbl = (pgtbl_t)pmem_alloc(true);
    if (!tbl) return NULL;
    memset(tbl, 0, PGSIZE);

    // 映射 trampoline (RX)
    vm_mappages(tbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    // 映射 trapframe (RW)
    vm_mappages(tbl, TRAPFRAME, tf_va, PGSIZE, PTE_R | PTE_W);

    return tbl;
}

// 分配进程块
proc_t *proc_alloc()
{
    proc_t *p = NULL;

    // 寻找 UNUSED 槽位
    for (int i = 0; i < N_PROC; i++) {
        spinlock_acquire(&process_pool[i].lk);
        if (process_pool[i].state == UNUSED) {
            p = &process_pool[i];
            break;
        }
        spinlock_release(&process_pool[i].lk);
    }

    if (!p) return NULL;

    p->pid = new_pid();
    p->state = UNUSED;

    // 分配 trapframe
    if ((p->tf = (trapframe_t *)pmem_alloc(true)) == NULL) {
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);

    // 初始化页表
    if ((p->pgtbl = proc_pgtbl_init((uint64)p->tf)) == NULL) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
        spinlock_release(&p->lk);
        return NULL;
    }

    // 设置上下文，准备调度
    memset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.ra = (uint64)fork_ret;
    p->ctx.sp = p->kstack + PGSIZE;

    // 清理字段
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->mmap = NULL;
    memset(p->name, 0, sizeof(p->name));

    p->state = RUNNABLE;
    return p;
}

// 释放进程资源
void proc_free(proc_t *p)
{
    if (p->tf) pmem_free((uint64)p->tf, true);
    p->tf = NULL;

    if (p->pgtbl) {
        vm_unmappages(p->pgtbl, USER_BASE, PGSIZE, true); // 代码
        // 释放堆
        if (p->heap_top > USER_BASE + PGSIZE)
             vm_unmappages(p->pgtbl, USER_BASE + PGSIZE, p->heap_top - (USER_BASE + PGSIZE), true);
        // 释放栈
        if (p->ustack_npage > 0)
            vm_unmappages(p->pgtbl, TRAPFRAME - p->ustack_npage * PGSIZE, p->ustack_npage * PGSIZE, true);
        // 释放 mmap
        mmap_region_t *m = p->mmap;
        while (m) {
            vm_unmappages(p->pgtbl, m->begin, m->npages * PGSIZE, true);
            mmap_region_t *next = m->next;
            mmap_region_free(m);
            m = next;
        }
        p->mmap = NULL;
        
        // 解除系统页映射
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
        
        pmem_free((uint64)p->pgtbl, true);
    }
    p->pgtbl = NULL;
    p->pid = 0;
    p->parent = NULL;
    p->name[0] = 0;
    p->state = UNUSED;
    
    spinlock_release(&p->lk);
}

// 创建首个进程
void proc_make_first()
{
    proc_t *p = proc_alloc();
    init_proc = p;

    // 加载 initcode
    void *mem = pmem_alloc(false);
    memmove(mem, target_user_initcode, target_user_initcode_len);
    vm_mappages(p->pgtbl, USER_BASE, (uint64)mem, PGSIZE, PTE_R|PTE_W|PTE_X|PTE_U);

    // 加载栈
    void *stack = pmem_alloc(false);
    vm_mappages(p->pgtbl, TRAPFRAME - PGSIZE, (uint64)stack, PGSIZE, PTE_R|PTE_W|PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USER_BASE + PGSIZE;

    // 设置 Trapframe
    p->tf->user_to_kern_epc = USER_BASE;
    p->tf->sp = TRAPFRAME;
    p->tf->user_to_kern_satp = r_satp();
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    extern void trap_user_handler();
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;

    // 命名
    char *s = "init";
    for(int i=0; s[i]; i++) p->name[i] = s[i];

    spinlock_release(&p->lk);
}

// 复制进程
int proc_fork()
{
    proc_t *curr = myproc();
    proc_t *child = proc_alloc();
    if (!child) return -1;

    // 复制页表
    uvm_copy_pgtbl(curr->pgtbl, child->pgtbl, curr->heap_top, curr->ustack_npage, curr->mmap);
    child->heap_top = curr->heap_top;
    child->ustack_npage = curr->ustack_npage;

    // 复制 mmap 结构
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

    // 复制 trapframe
    *(child->tf) = *(curr->tf);
    child->tf->a0 = 0; // 子进程返回 0
    
    // [CRITICAL] 修复栈指针：子进程必须使用自己的内核栈
    child->tf->user_to_kern_sp = child->kstack + PGSIZE;

    // 复制父进程名称
    for(int i=0; i<16; i++) child->name[i] = curr->name[i];
    child->parent = curr;
    
    int pid = child->pid;
    spinlock_release(&child->lk);
    return pid;
}

// 唤醒机制
void proc_wakeup(void *chan)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &process_pool[i];
        if (p != myproc()) {
            spinlock_acquire(&p->lk);
            if (p->state == SLEEPING && p->sleep_space == chan) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}

// 休眠机制
void proc_sleep(void *chan, spinlock_t *lk)
{
    proc_t *p = myproc();
    
    // 必须先拿进程锁，再放外部锁，保证原子性
    spinlock_acquire(&p->lk);
    spinlock_release(lk);

    p->sleep_space = chan;
    p->state = SLEEPING;

    proc_sched(); // 切换

    p->sleep_space = NULL;
    
    spinlock_release(&p->lk);
    spinlock_acquire(lk);
}

// 调度器
void proc_scheduler()
{
    cpu_t *c = mycpu();
    c->proc = NULL;

    for (;;) {
        intr_on(); // 必须开中断

        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &process_pool[i];
            spinlock_acquire(&p->lk);
            
            if (p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;
                
                // 切换上下文
                swtch(&c->ctx, &p->ctx);
                
                c->proc = NULL;
            }
            spinlock_release(&p->lk);
        }
    }
}

// 切换回调度器
void proc_sched()
{
    swtch(&myproc()->ctx, &mycpu()->ctx);
}

// 主动让出
void proc_yield()
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 退出
void proc_exit(int code)
{
    proc_t *curr = myproc();
    if (curr == init_proc) panic("init exiting");

    // 获取全局树锁，开始处理父子关系
    spinlock_acquire(&tree_lock);

    // 过继子进程
    for (int i = 0; i < N_PROC; i++) {
        if (process_pool[i].parent == curr) {
            process_pool[i].parent = init_proc;
            // 如果孩子已经是僵尸，唤醒新父亲
            spinlock_acquire(&process_pool[i].lk);
            if (process_pool[i].state == ZOMBIE) {
                 // 唤醒 init_proc (逻辑简化：只要唤醒 sleep_space==&tree_lock 的即可)
                 proc_wakeup(&tree_lock); 
            }
            spinlock_release(&process_pool[i].lk);
        }
    }

    spinlock_acquire(&curr->lk);
    curr->exit_code = code;
    curr->state = ZOMBIE;
    
    // 唤醒父进程
    proc_wakeup(&tree_lock); // 父进程应该睡在 tree_lock 上

    spinlock_release(&curr->lk);
    // 持有 tree_lock 进入调度会导致死锁吗？
    // 调度器不使用 tree_lock。父进程 wait 会获取 tree_lock。
    // 如果这里持有 tree_lock 切换，父进程醒来获取 tree_lock 就会死锁。
    // 所以必须在切换前释放 tree_lock。
    spinlock_release(&tree_lock);

    spinlock_acquire(&curr->lk);
    proc_sched();
    panic("zombie exit");
}

// 等待子进程
int proc_wait(uint64 addr)
{
    spinlock_acquire(&tree_lock);

    for (;;) {
        int have_kids = 0;
        proc_t *curr = myproc();

        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &process_pool[i];
            if (p->parent != curr) continue;
            
            have_kids = 1;
            spinlock_acquire(&p->lk);
            if (p->state == ZOMBIE) {
                int pid = p->pid;
                int code = p->exit_code;
                proc_free(p); // 内部释放 p->lk
                spinlock_release(&tree_lock);
                
                if (addr) uvm_copyout(curr->pgtbl, addr, (uint64)&code, sizeof(int));
                return pid;
            }
            spinlock_release(&p->lk);
        }

        if (!have_kids) {
            spinlock_release(&tree_lock);
            return -1;
        }

        // 休眠在 tree_lock 上
        proc_sleep(&tree_lock, &tree_lock);
    }
}
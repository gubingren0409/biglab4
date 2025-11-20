#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"

// 引入用户程序的二进制代码 (生成的 initcode.h)
#include "../../user/initcode.h"

// 外部汇编函数声明
extern char trampoline[];
extern void swtch(context_t *old, context_t *new);
extern void trap_user_return(); // 返回用户态的入口

/* -------------------------------------------------------------------------
 * 全局变量定义
 * ------------------------------------------------------------------------- */

// 进程控制块池
static proc_t process_pool[N_PROC];

// 指向第一个用户进程的指针 (proczero)
static proc_t *init_proc;

// PID 分配器及其锁
static int next_pid = 1;
static spinlock_t pid_lock;

// 全局锁：用于保护父子进程之间的同步操作 (wait/exit) 以及孤儿进程的过继
// 这把锁必须在获取进程私有锁之前获取，以避免死锁
static spinlock_t orphan_lock;

/* -------------------------------------------------------------------------
 * 辅助函数
 * ------------------------------------------------------------------------- */

// 分配一个新的 PID
static int allocate_pid()
{
    int pid;
    spinlock_acquire(&pid_lock);
    pid = next_pid++;
    spinlock_release(&pid_lock);
    return pid;
}

// 新进程在内核态的入口点
// 当调度器 switch 到新进程时，会从这里开始执行
static void fork_ret()
{
    // 调度器在 switch 之前持有了进程锁，这里需要释放
    spinlock_release(&myproc()->lk);
    
    // 返回用户空间
    trap_user_return();
}

// 尝试唤醒父进程
// 调用者必须持有 p->lk，且 p->parent 必须有效
static void wakeup_parent_locked(proc_t *p)
{
    proc_t *parent = p->parent;
    
    spinlock_acquire(&parent->lk);
    
    // 只有当父进程处于 SLEEPING 状态，且正在等待的对象是 orphan_lock 时才唤醒
    // 这对应于 proc_wait 中的 sleep 调用
    if (parent->state == SLEEPING && parent->sleep_space == &orphan_lock) {
        parent->state = RUNNABLE;
    }
    
    spinlock_release(&parent->lk);
}

// 将 dying_proc 的所有子进程过继给 init_proc
static void reparent_children(proc_t *dying_proc)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &process_pool[i];
        
        spinlock_acquire(&p->lk);
        
        if (p->parent == dying_proc) {
            p->parent = init_proc;
            
            // 如果被领养的孩子已经是僵尸，需要唤醒新父亲(init_proc)来回收它
            if (p->state == ZOMBIE) {
                wakeup_parent_locked(p);
            }
        }
        
        spinlock_release(&p->lk);
    }
}

/* -------------------------------------------------------------------------
 * 核心接口实现
 * ------------------------------------------------------------------------- */

// 初始化进程管理子系统
void proc_init()
{
    spinlock_init(&pid_lock, "pid_gen");
    spinlock_init(&orphan_lock, "orphan_reparent");
    
    // 初始化所有进程槽位
    for (int i = 0; i < N_PROC; i++) {
        spinlock_init(&process_pool[i].lk, "proc_ctrl");
        process_pool[i].state = UNUSED;
        
        // 预先计算好内核栈的虚拟地址
        // 每个进程对应一个固定的内核栈区域
        process_pool[i].kstack = KSTACK(i);
    }
}

// 初始化进程页表：映射 trampoline 和 trapframe
pgtbl_t proc_pgtbl_init(uint64 trapframe_va)
{
    pgtbl_t tbl = (pgtbl_t)pmem_alloc(true);
    if (tbl == NULL) return NULL;
    
    memset(tbl, 0, PGSIZE);

    // 映射 Trampoline 代码 (内核与用户共享，但用户不可写)
    // 必须有 PTE_X 权限
    vm_mappages(tbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 映射 Trapframe 数据区 (内核读写，用户不可直接访问)
    // 不设置 PTE_U
    vm_mappages(tbl, TRAPFRAME, trapframe_va, PGSIZE, PTE_R | PTE_W);

    return tbl;
}

// 从池中分配一个空闲进程
// 返回时持有进程锁
proc_t *proc_alloc()
{
    proc_t *p = NULL;

    // 1. 寻找空闲槽位
    for (int i = 0; i < N_PROC; i++) {
        spinlock_acquire(&process_pool[i].lk);
        if (process_pool[i].state == UNUSED) {
            p = &process_pool[i];
            break;
        }
        spinlock_release(&process_pool[i].lk);
    }

    if (p == NULL) return NULL;

    // 2. 初始化基础元数据
    p->pid = allocate_pid();
    p->state = UNUSED; // 分配未完成前保持 UNUSED

    // 3. 分配 Trapframe 物理页
    if ((p->tf = (trapframe_t *)pmem_alloc(true)) == NULL) {
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);

    // 4. 初始化用户页表
    if ((p->pgtbl = proc_pgtbl_init((uint64)p->tf)) == NULL) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
        spinlock_release(&p->lk);
        return NULL;
    }

    // 5. 初始化内核上下文 (Context)
    // 这决定了调度器 switch 到该进程时从哪里开始执行
    memset(&p->ctx, 0, sizeof(context_t));
    p->ctx.ra = (uint64)fork_ret;        // 返回地址
    p->ctx.sp = p->kstack + PGSIZE;      // 内核栈顶 (栈向下增长)

    // 6. 清理其他字段
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->mmap = NULL;
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    memset(p->name, 0, sizeof(p->name));

    // 7. 标记为 RUNNABLE 并带锁返回
    p->state = RUNNABLE;
    return p;
}

// 释放进程资源
// 调用者必须持有 p->lk
void proc_free(proc_t *p)
{
    if (p->tf) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
    }

    if (p->pgtbl) {
        // 逐个释放用户空间的映射
        // 1. 代码段 (USER_BASE)
        vm_unmappages(p->pgtbl, USER_BASE, PGSIZE, true);
        
        // 2. 堆空间
        if (p->heap_top > USER_BASE + PGSIZE) {
            uint64 heap_sz = p->heap_top - (USER_BASE + PGSIZE);
            vm_unmappages(p->pgtbl, USER_BASE + PGSIZE, heap_sz, true);
        }

        // 3. 用户栈
        if (p->ustack_npage > 0) {
            uint64 stack_base = TRAPFRAME - p->ustack_npage * PGSIZE;
            vm_unmappages(p->pgtbl, stack_base, p->ustack_npage * PGSIZE, true);
        }

        // 4. mmap 区域
        mmap_region_t *node = p->mmap;
        while (node) {
            vm_unmappages(p->pgtbl, node->begin, node->npages * PGSIZE, true);
            mmap_region_t *next = node->next;
            mmap_region_free(node);
            node = next;
        }
        p->mmap = NULL;

        // 5. 解除系统页映射 (不释放物理页)
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);

        // 6. 释放页表本身
        pmem_free((uint64)p->pgtbl, true);
        p->pgtbl = NULL;
    }

    p->pid = 0;
    p->parent = NULL;
    p->name[0] = 0;
    p->state = UNUSED;
    
    spinlock_release(&p->lk);
}

// 创建第一个用户进程 (initcode)
void proc_make_first()
{
    proc_t *p = proc_alloc();
    if (!p) panic("proc_make_first: failed");

    init_proc = p;
    
    // 设置进程名
    char *name = "init_proc";
    for(int i=0; name[i]; i++) p->name[i] = name[i];

    // 1. 加载 initcode 二进制到用户空间起始位置
    void *code_page = pmem_alloc(false);
    memmove(code_page, target_user_initcode, target_user_initcode_len);
    vm_mappages(p->pgtbl, USER_BASE, (uint64)code_page, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // 2. 分配一个初始用户栈 (1页)
    void *stack_page = pmem_alloc(false);
    uint64 stack_va = TRAPFRAME - PGSIZE;
    vm_mappages(p->pgtbl, stack_va, (uint64)stack_page, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;

    // 更新堆顶指针
    p->heap_top = USER_BASE + PGSIZE;

    // 3. 设置 Trapframe，为首次进入用户态做准备
    // 用户 PC 指向代码段
    p->tf->user_to_kern_epc = USER_BASE;
    // 用户 SP 指向栈底 (TRAPFRAME 是高地址边界)
    p->tf->sp = TRAPFRAME;
    
    // 关键内核信息：用于从 U-mode 陷阱回 S-mode
    p->tf->user_to_kern_satp = r_satp();
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    extern void trap_user_handler();
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;

    // 分配完成后释放锁，允许调度
    spinlock_release(&p->lk);
}

// 复制当前进程 (Fork)
int proc_fork()
{
    proc_t *curr = myproc();
    proc_t *child = proc_alloc(); // 返回时持有 child->lk
    
    if (!child) return -1;

    // 1. 完整复制内存空间 (COW 优化可选，这里做深拷贝)
    uvm_copy_pgtbl(curr->pgtbl, child->pgtbl, curr->heap_top, curr->ustack_npage, curr->mmap);
    child->heap_top = curr->heap_top;
    child->ustack_npage = curr->ustack_npage;

    // 2. 复制 mmap 链表结构
    mmap_region_t *src = curr->mmap;
    mmap_region_t **dst_ptr = &child->mmap;
    while (src) {
        mmap_region_t *new_node = mmap_region_alloc();
        new_node->begin = src->begin;
        new_node->npages = src->npages;
        new_node->next = NULL;
        *dst_ptr = new_node;
        dst_ptr = &new_node->next;
        src = src->next;
    }

    // 3. 复制 Trapframe
    *(child->tf) = *(curr->tf);
    
    // Fork 返回值：子进程返回 0
    child->tf->a0 = 0;
    
    // [修复 Test-02] 子进程的 trapframe 中必须记录它自己的内核栈地址
    // 否则 trap 后会踩坏父进程的栈
    child->tf->user_to_kern_sp = child->kstack + PGSIZE;

    // 4. 建立父子关系
    child->parent = curr;
    for(int i=0; i<16; i++) child->name[i] = curr->name[i];

    int child_pid = child->pid;
    spinlock_release(&child->lk);

    return child_pid;
}

// 进程调度器主循环
// 运行在每个 CPU 的独立调度线程中
void proc_scheduler()
{
    cpu_t *cpu = mycpu();
    cpu->proc = NULL;

    for (;;) {
        // 必须开启中断，否则系统会死锁
        intr_on();

        for (int i = 0; i < N_PROC; i++) {
            proc_t *p = &process_pool[i];
            
            spinlock_acquire(&p->lk);
            
            if (p->state == RUNNABLE) {
                // 找到可运行进程，切换状态
                p->state = RUNNING;
                cpu->proc = p;
                
                // 切换上下文：调度器 -> 进程
                swtch(&cpu->ctx, &p->ctx);
                
                // 进程让出 CPU 后回到这里
                cpu->proc = NULL;
            }
            
            spinlock_release(&p->lk);
        }
    }
}

// 进程切换：从当前进程切换回调度器
// 调用者必须持有 p->lk
void proc_sched()
{
    proc_t *p = myproc();

    if (!spinlock_holding(&p->lk)) panic("proc_sched: lock not held");
    if (mycpu()->noff != 1) panic("proc_sched: locks nesting");
    if (p->state == RUNNING) panic("proc_sched: proc is running");
    if (intr_get()) panic("proc_sched: interrupts enabled");

    // 上下文切换：进程 -> 调度器
    swtch(&p->ctx, &mycpu()->ctx);
}

// 主动让出 CPU (RUNNING -> RUNNABLE)
void proc_yield()
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 进程休眠 (RUNNING -> SLEEPING)
// 原子地释放 external_lock 并进入休眠
void proc_sleep(void *chan, spinlock_t *external_lock)
{
    proc_t *p = myproc();
    
    // 必须先获取进程锁，再释放外部锁，保证操作原子性
    // 防止在进入 sleep 状态前就丢失了 wakeup 信号
    spinlock_acquire(&p->lk);
    spinlock_release(external_lock);

    p->sleep_space = chan;
    p->state = SLEEPING;

    proc_sched();

    // 醒来后清理状态
    p->sleep_space = NULL;
    p->state = RUNNING;

    spinlock_release(&p->lk);
    
    // 恢复外部锁状态
    spinlock_acquire(external_lock);
}

// 唤醒等待在 chan 上的所有进程
void proc_wakeup(void *chan)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &process_pool[i];
        if (p != myproc()) {
            spinlock_acquire(&p->lk);
            if (p->state == SLEEPING && p->sleep_space == chan) {
                p->state = RUNNABLE;
                // 可选：Test-04 需要打印唤醒信息
                // printf("proc %d is wakeup!\n", p->pid);
            }
            spinlock_release(&p->lk);
        }
    }
}

// 进程退出 (RUNNING -> ZOMBIE)
void proc_exit(int exit_code)
{
    proc_t *curr = myproc();
    if (curr == init_proc) panic("init_proc cannot exit");

    // 1. 获取全局孤儿锁，开始处理父子关系
    spinlock_acquire(&orphan_lock);

    // 2. 将所有子进程过继给 init_proc
    reparent_children(curr);

    // 3. 标记自身状态
    spinlock_acquire(&curr->lk);
    curr->exit_code = exit_code;
    curr->state = ZOMBIE;

    // 4. 唤醒父进程来收尸
    wakeup_parent_locked(curr);

    spinlock_release(&curr->lk);
    
    // [关键] 在进入调度器前释放孤儿锁
    // 因为调度器 switch 不会释放这个锁，如果不释放会导致死锁
    // 父进程 wait 时会获取这把锁
    spinlock_release(&orphan_lock);

    // 5. 重新获取进程锁并切换到调度器
    // 此时状态已为 ZOMBIE，永远不会返回
    spinlock_acquire(&curr->lk);
    proc_sched();
    
    panic("proc_exit: zombie returned");
}

// 等待子进程退出并回收资源
int proc_wait(uint64 addr_for_exit_code)
{
    proc_t *curr = myproc();
    
    // 获取全局孤儿锁，确保不会错过子进程的 exit 唤醒
    spinlock_acquire(&orphan_lock);

    for (;;) {
        int has_kids = 0;
        
        // 遍历所有进程，寻找子进程
        for (int i = 0; i < N_PROC; i++) {
            proc_t *target = &process_pool[i];
            if (target->parent != curr) continue;

            has_kids = 1;
            
            // 检查子进程状态
            spinlock_acquire(&target->lk);
            if (target->state == ZOMBIE) {
                // 找到僵尸子进程，进行回收
                int pid = target->pid;
                int code = target->exit_code;
                
                // 释放子进程资源（内部会释放 target->lk）
                proc_free(target); 
                
                // 释放全局锁
                spinlock_release(&orphan_lock);
                
                // 将退出码写回用户空间
                if (addr_for_exit_code != 0) {
                    uvm_copyout(curr->pgtbl, addr_for_exit_code, (uint64)&code, sizeof(int));
                }
                
                return pid;
            }
            spinlock_release(&target->lk);
        }

        // 如果没有子进程，立即返回错误
        if (!has_kids) {
            spinlock_release(&orphan_lock);
            return -1;
        }

        // 有子进程但都在运行，进入休眠
        // 休眠在 orphan_lock 上，这与 proc_exit 的唤醒条件对应
        // proc_sleep 会原子地释放 orphan_lock 并挂起
        proc_sleep(&orphan_lock, &orphan_lock);
    }
}
#include "mod.h"
#include "../proc/mod.h" 

/*
 * 初始化睡眠锁
 * 睡眠锁本质上由一个自旋锁保护其内部状态（locked标志和持有者PID）
 */
void sleeplock_init(sleeplock_t *slk, char *name)
{
    // 初始化内部的自旋锁，用于保护对睡眠锁状态的原子访问
    spinlock_init(&slk->lock, "sleeplock_spin");
    
    slk->name = name;
    slk->locked = 0;  // 初始状态为未锁定
    slk->pid = 0;     // 初始无持有者
}

/*
 * 检查当前进程是否持有该睡眠锁
 * 返回 true 表示持有，false 表示未持有
 */
bool sleeplock_holding(sleeplock_t *slk)
{
    int is_holding;
    
    // 获取自旋锁以读取状态，确保原子性
    spinlock_acquire(&slk->lock);
    
    // 检查锁是否被占用，且占用者是否为当前进程
    // 注意：myproc() 获取当前 CPU 运行的进程
    is_holding = (slk->locked && slk->pid == myproc()->pid);
    
    spinlock_release(&slk->lock);
    
    return is_holding;
}

/*
 * 获取睡眠锁
 * 如果锁被占用，当前进程将进入睡眠状态（SLEEPING），让出 CPU
 */
void sleeplock_acquire(sleeplock_t *slk)
{
    // 1. 获取内部自旋锁，保护共享变量
    spinlock_acquire(&slk->lock);
    
    // 2. 循环检查锁的状态
    // 如果已经被其他进程持有，则进入睡眠
    while (slk->locked) {
        // proc_sleep 的原子性操作：
        // 1. 标记当前进程为 SLEEPING
        // 2. 记录等待的资源（这里是 slk 本身）
        // 3. 释放 slk->lock (允许其他进程访问锁状态)
        // 4. 切换进程
        // 5. 被唤醒后，重新获取 slk->lock
        proc_sleep(slk, &slk->lock);
    }
    
    // 3. 抢到了锁，标记占用
    slk->locked = 1;
    slk->pid = myproc()->pid;
    
    // 4. 释放内部自旋锁
    spinlock_release(&slk->lock);
}

/*
 * 释放睡眠锁
 * 并唤醒所有在该锁上等待的进程
 */
void sleeplock_release(sleeplock_t *slk)
{
    // 1. 获取内部自旋锁
    spinlock_acquire(&slk->lock);
    
    // 2. 清除占用状态
    slk->locked = 0;
    slk->pid = 0;
    
    // 3. 唤醒等待队列中的进程
    // 它们醒来后会继续在 acquire 的 while 循环中竞争锁
    proc_wakeup(slk);
    
    // 4. 释放内部自旋锁
    spinlock_release(&slk->lock);
}
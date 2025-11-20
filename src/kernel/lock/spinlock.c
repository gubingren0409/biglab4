#include "mod.h"

/*
    开关中断的基本逻辑:
    1. 多个地方可能开关中断, 因此它不是“开/关”的二元状态，而是“关 关 关 开 开 开”的stack
    2. 在第一次执行关中断时, 记录中断的初始状态为X
    3. 每次关中断, stack中的元素加1
    4. 每次开中断, stack中的元素减1
    5. 如果stack中元素清空, 将中断状态设为初始的X
*/

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();
    intr_off();
    cpu_t *cpu = mycpu();
    if (cpu->noff == 0)
        cpu->origin = old;
    cpu->noff++;
}

// 带层数叠加的开中断
void pop_off(void)
{
    cpu_t *cpu = mycpu();
    assert(intr_get() == 0, "push_off: 1\n"); // 确保此时中断是关闭的
    assert(cpu->noff >= 1, "push_off: 2\n");  // 确保push和pop的对应
    cpu->noff--;
    if (cpu->noff == 0 && cpu->origin == 1) // 只有所有push操作都被抵消且原来状态是开着时
        intr_on();
}


// 自旋锁初始化
void spinlock_init(spinlock_t *lk, char *name)
{
    lk->locked = 0;
    lk->name = name;
    lk->cpuid = -1;
}

// 是否持有自旋锁
// 修复点：这里必须返回一个布尔值
bool spinlock_holding(spinlock_t *lk)
{
    // 只有当锁被锁住(locked=1)且持有者ID等于当前CPU ID时，才算持有
    return (lk->locked && lk->cpuid == mycpuid());
}

// 获取自旋锁
void spinlock_acquire(spinlock_t *lk)
{
    push_off(); // 关中断，防止在持有锁时发生中断导致死锁

    if (spinlock_holding(lk))
        panic("spinlock_acquire: recursive lock"); // 禁止重入

    // 原子操作：尝试将 locked 设置为 1
    // 如果原来就是 1，则循环等待 (spin)
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;

    // 内存屏障，保证临界区代码不被乱序到锁获取之前
    __sync_synchronize();

    // 记录当前持有锁的 CPU
    lk->cpuid = mycpuid();
}

// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    if (!spinlock_holding(lk))
        panic("spinlock_release: not holding");

    lk->cpuid = -1; // 清除持有者信息

    // 内存屏障，保证临界区代码不被乱序到锁释放之后
    __sync_synchronize();

    // 原子释放锁
    __sync_lock_release(&lk->locked);

    pop_off(); // 恢复中断状态
}
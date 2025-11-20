#include "mod.h"
#include "../arch/mod.h"
#include "../lib/mod.h"

/* -------------------------------------------------------------------------
 * Machine Mode (M-mode) 部分
 * 负责底层硬件时钟中断的设置
 * ------------------------------------------------------------------------- */

// 汇编入口，位于 trap.S
extern void timer_vector();

// 每个 CPU 核心在 M-mode 中断处理时需要的临时存储区
// 保存: [0-2] 临时寄存器, [3] mtimecmp 地址, [4] interval 间隔
static uint64 timer_scratch_pad[NCPU][5];

void timer_init()
{
    // 1. 获取当前核心 ID
    int cpuid = r_tp();

    // 2. 设定下一次中断时间
    // MTIMECMP = MTIME + INTERVAL
    uint64 *mtime_reg = (uint64*)CLINT_MTIME;
    uint64 *mtimecmp_reg = (uint64*)CLINT_MTIMECMP(cpuid);
    *mtimecmp_reg = *mtime_reg + INTERVAL;

    // 3. 准备 scratch area 供汇编程序使用
    uint64 *scratch = timer_scratch_pad[cpuid];
    scratch[3] = (uint64)mtimecmp_reg;
    scratch[4] = INTERVAL;

    // 4. 将 scratch 地址写入 mscratch 寄存器
    w_mscratch((uint64)scratch);

    // 5. 设置 M-mode 异常向量表地址
    w_mtvec((uint64)timer_vector);

    // 6. 开启 M-mode 全局中断和时钟中断
    w_mstatus(r_mstatus() | MSTATUS_MIE);
    w_mie(r_mie() | MIE_MTIE);
}

/* -------------------------------------------------------------------------
 * Supervisor Mode (S-mode) 部分
 * 负责系统节拍维护和基于时间的睡眠
 * ------------------------------------------------------------------------- */

// 全局时间管理器
static struct {
    uint64 current_ticks;  // 系统启动以来的总节拍数
    spinlock_t lock;       // 保护 ticks 的互斥锁
} time_keeper;

// 初始化系统时钟（S-mode）
void timer_create()
{
    time_keeper.current_ticks = 0;
    spinlock_init(&time_keeper.lock, "global_time_keeper");
}

// 时钟中断处理函数调用此函数更新时间
void timer_update()
{
    spinlock_acquire(&time_keeper.lock);
    
    time_keeper.current_ticks++;
    
    spinlock_release(&time_keeper.lock);

    // 广播唤醒：唤醒所有在该时间管理器上睡眠的进程
    // 它们被唤醒后会检查当前时间是否满足唤醒条件
    proc_wakeup(&time_keeper);
}

// 获取当前系统时间
uint64 timer_get_ticks()
{
    uint64 snapshot;
    spinlock_acquire(&time_keeper.lock);
    snapshot = time_keeper.current_ticks;
    spinlock_release(&time_keeper.lock);
    return snapshot;
}

// 让当前进程休眠 n_tick 个节拍
void timer_wait(uint64 n_tick)
{
    spinlock_acquire(&time_keeper.lock);
    
    // 计算唤醒的目标时间
    uint64 target_tick = time_keeper.current_ticks + n_tick;

    // 循环等待直到时间到达
    while (time_keeper.current_ticks < target_tick) {
        // [Lab Requirement] 打印睡眠日志，用于 Test-04 验证
        printf("proc %d is sleeping!\n", myproc()->pid);

        // 原子操作：释放锁 -> 进入睡眠 -> 被唤醒 -> 重新获取锁
        // 等待的资源标识就是 time_keeper 结构体的地址
        proc_sleep(&time_keeper, &time_keeper.lock);
    }

    // [Lab Requirement] 打印唤醒日志
    printf("proc %d is wakeup!\n", myproc()->pid);

    spinlock_release(&time_keeper.lock);
}
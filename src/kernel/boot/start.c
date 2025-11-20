#include "../arch/mod.h"
#include "../lib/mod.h"  // 需要包含库函数以访问 timer_init 声明
#include "../trap/mod.h"

// 为每个 CPU 核心预留启动栈空间 (4KB per CPU)
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

// 声明跳转目标
extern void main();

void start()
{
    // 1. 启动初期禁用分页，直接使用物理地址访问
    w_satp(0);

    // 2. 将当前 CPU 的硬件 ID (mhartid) 保存到 tp 寄存器
    // 这样后续在 S-mode 可以通过 r_tp() 获取当前核心 ID
    uint64 id = r_mhartid();
    w_tp(id);

    // 3. 配置 Trap 委托机制 (M-mode -> S-mode)
    // 将所有异常 (Exception) 委托给 S-mode 处理
    // 0xffff 覆盖了我们关心的所有常见异常
    w_medeleg(0xffff);

    // 将 S-mode 相关的中断 (软件、时钟、外部) 委托给 S-mode
    // Bit 1: SSIP, Bit 5: STIP, Bit 9: SEIP
    uint64 s_interrupts = (1 << 1) | (1 << 5) | (1 << 9);
    w_mideleg(s_interrupts);

    // 4. 允许 S-mode 读取性能计数器 (Time, Cycle, InstRet)
    w_mcounteren(0x7);

    // 5. 初始化 M-mode 时钟中断 (CLINT)
    // 这个函数定义在 kernel/trap/timer.c 中
    timer_init();

    // 6. 准备从 M-mode 切换到 S-mode
    // 修改 mstatus 寄存器：
    // MPP (Previous Mode) 设置为 S-mode (1)
    // 这样执行 mret 后 CPU 会进入 S-mode
    uint64 status_val = r_mstatus();
    status_val &= ~MSTATUS_MPP_MASK; // 清除 MPP 位
    status_val |= MSTATUS_MPP_S;     // 设置为 Supervisor 模式
    w_mstatus(status_val);

    // 7. 设置 mret 的返回地址 (mepc)
    // 当执行 mret 时，PC 将跳转到 main 函数入口
    w_mepc((uint64)main);

    // 8. 执行状态切换，正式进入 OS 内核主逻辑
    asm volatile("mret");
}
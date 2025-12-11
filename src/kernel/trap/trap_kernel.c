#include "mod.h"
// 需要访问寄存器读写内联函数与中断开关工具
#include "../arch/mod.h"
// 需要printf/panic/assert/uart_intr等库函数声明
#include "../lib/mod.h"

// 中断描述信息
char *interrupt_desc[16] = {
    "User software interrupt",
    "Supervisor software interrupt",
    "Reserved-2",
    "Machine software interrupt",
    "User timer interrupt",
    "Supervisor timer interrupt",
    "Reserved-6",
    "Machine timer interrupt",
    "User external interrupt",
    "Supervisor external interrupt",
    "Reserved-10",
    "Machine external interrupt",
    // ... 其余保留
};

// 异常描述信息
char *exception_desc[16] = {
    "Instruction addr misaligned",
    "Instruction access fault",
    "Illegal instruction",
    "Breakpoint",
    "Load addr misaligned",
    "Load access fault",
    "Store/AMO addr misaligned",
    "Store/AMO access fault",
    "Ecall from User",
    "Ecall from Supervisor",
    "Reserved-10",
    "Ecall from Machine",
    "Instruction page fault",
    "Load page fault",
    "Reserved-14",
    "Store/AMO page fault",
};

// 汇编入口声明
extern void kernel_vector();

void trap_kernel_init()
{
    plic_init();
    timer_create();
}

void trap_kernel_inithart()
{
    plic_inithart();
    // 设置内核态陷阱入口
    w_stvec((uint64)kernel_vector);
    
    // 开启 S 态的软件中断、时钟中断和外部中断
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
    intr_on();
}

// 内核态陷阱处理主函数
void trap_kernel_handler()
{
    // 读取关键 CSR 寄存器
    uint64 sepc_val = r_sepc();
    uint64 sstatus_val = r_sstatus();
    uint64 scause_val = r_scause();
    uint64 stval_val = r_stval();

    // 检查：确保陷阱来自 S 态，且中断已被硬件自动关闭
    assert((sstatus_val & SSTATUS_SPP) != 0, "KernTrap: not from S-mode");
    assert(intr_get() == 0, "KernTrap: interrupts not disabled");

    int irq_type = scause_val & 0xf;
    int is_async = (scause_val & 0x8000000000000000ul) != 0;

    if (is_async) {
        // --- 中断处理 ---
        switch (irq_type) {
        case 1: // S 态软件中断（实际上由 M 态时钟中断转发而来）
            timer_interrupt_handler();
            break;
        case 9: // S 态外部中断（外设）
            external_interrupt_handler();
            break;
        default:
            printf("Kernel panic: unexpected interrupt %d\n", irq_type);
            printf("sepc=%p stval=%p\n", sepc_val, stval_val);
            panic("trap_kernel_handler");
        }
    } else {
        // --- 异常处理 ---
        printf("Kernel panic: unexpected exception %d (%s)\n", irq_type, exception_desc[irq_type]);
        printf("sepc=%p stval=%p\n", sepc_val, stval_val);
        panic("trap_kernel_handler");
    }

    // 抢占式调度点：
    // 如果是时钟中断，且当前有进程正在运行（而非调度器或空闲线程），则让出 CPU
    if (is_async && irq_type == 1) {
        if (myproc() != NULL && myproc()->state == RUNNING) {
            proc_yield();
        }
    }

    // 恢复上下文将由汇编代码 kernel_vector 完成
    w_sepc(sepc_val);
    w_sstatus(sstatus_val);
}

// 处理外部设备中断（如 UART）
void external_interrupt_handler()
{
    int irq_num = plic_claim();

    if (irq_num == UART_IRQ) {
        // 处理串口输入
        uart_intr();
    } else if (irq_num == VIRTIO_IRQ) {
        // [NEW] 处理磁盘中断
        virtio_disk_intr();
    }else if (irq_num != 0) {
        printf("Ignored unexpected PLIC irq: %d\n", irq_num);
    }

    // 通知 PLIC 中断处理完成
    if (irq_num != 0) {
        plic_complete(irq_num);
    }
}

// 处理时钟中断
void timer_interrupt_handler()
{
    // 只有 CPU 0 负责更新全局系统滴答数
    if (mycpuid() == 0) {
        timer_update();
    }

    // 清除 SIP 寄存器中的软件中断挂起位
    // 告知硬件该中断已被处理
    w_sip(r_sip() & ~2);
}
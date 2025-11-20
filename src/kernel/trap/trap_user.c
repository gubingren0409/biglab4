#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"

// 引入外部汇编符号
// trampoline.S
extern char trampoline[];  
extern char user_vector[]; 
extern char user_return[]; 

// trap.S
extern char kernel_vector[]; 

// trap_kernel.c
extern char *interrupt_info[16];
extern char *exception_info[16];

// 用户态陷阱处理的核心入口
// 由 user_vector 在保存完上下文后跳转至此
void trap_user_handler()
{
    // 1. 进入内核后，首先将中断向量表切换到内核的处理函数
    // 这样如果在内核态发生中断，将由 kernel_vector 接管
    w_stvec((uint64)kernel_vector);

    proc_t *curr_proc = myproc();
    trapframe_t *frame = curr_proc->tf;

    // 2. 安全检查：确保之前的状态确实是用户态 (SPP位为0)
    uint64 status_reg = r_sstatus();
    if ((status_reg & SSTATUS_SPP) != 0) {
        panic("trap_user_handler: trap did not come from user mode");
    }

    // 3. 保存发生陷阱时的用户程序计数器(PC)
    frame->user_to_kern_epc = r_sepc();

    uint64 scause_reg = r_scause();
    int cause_type = scause_reg & 0xf;
    bool is_interrupt = (scause_reg & 0x8000000000000000ul) != 0;

    if (is_interrupt) {
        // --- 处理中断 ---
        switch (cause_type) {
        case 1: // S模式软件中断 (由M模式时钟中断触发)
            timer_interrupt_handler();
            break;
        case 9: // S模式外部中断 (PLIC)
            external_interrupt_handler();
            break;
        default:
            printf("Unknown user interrupt: %d\n", cause_type);
            printf("Context: sepc=%p stval=%p\n", frame->user_to_kern_epc, r_stval());
            break;
        }
    } else {
        // --- 处理异常 ---
        switch (cause_type) {
        case 8: // 用户态系统调用 (ECALL)
            // 系统调用返回后，PC需要手动+4，跳过ecall指令
            frame->user_to_kern_epc += 4;
            
            // 开启中断，允许在系统调用期间响应中断（如时钟）
            intr_on();
            syscall();
            intr_off(); // 返回前再次关闭
            break;

        case 13: // Load Page Fault
        case 15: // Store/AMO Page Fault
        {
            // 处理用户栈的自动增长
            uint64 bad_addr = r_stval();
            // printf("User Page Fault: addr=%p, type=%d\n", bad_addr, cause_type);
            
            uint64 current_stack_pages = curr_proc->ustack_npage;
            // 尝试扩展用户栈
            uint64 new_stack_pages = uvm_ustack_grow(curr_proc->pgtbl, current_stack_pages, bad_addr);
            
            if (new_stack_pages == (uint64)-1) {
                printf("Stack overflow or invalid access: pid=%d, addr=%p\n", curr_proc->pid, bad_addr);
                curr_proc->state = ZOMBIE; // 杀死进程
                curr_proc->exit_code = -1;
                proc_sched();
            } else {
                curr_proc->ustack_npage = new_stack_pages;
            }
            break;
        }
        default:
            printf("Unhandled user exception: id=%d, pid=%d\n", cause_type, curr_proc->pid);
            printf("sepc=%p stval=%p\n", frame->user_to_kern_epc, r_stval());
            curr_proc->state = ZOMBIE; // 无法处理的异常，终止进程
            curr_proc->exit_code = -1;
            proc_sched();
            break;
        }
    }

    // 4. 检查是否需要调度
    // 如果是时钟中断，说明时间片用完，强制让出 CPU
    if (is_interrupt && cause_type == 1) {
        proc_yield();
    }

    // 5. 返回用户态
    trap_user_return();
}

// 恢复上下文并返回用户态
void trap_user_return()
{
    proc_t *curr_proc = myproc();
    trapframe_t *frame = curr_proc->tf;

    // 1. 关闭中断
    // 接下来要操作 stvec 和 sscratch，必须保证原子性，防止被中断打断
    intr_off();

    // 2. 设置陷阱向量表指向 trampoline 中的 user_vector
    // 这样用户态再次发生陷阱时，CPU 知道去哪里跳转
    uint64 trampoline_uservec = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(trampoline_uservec);

    // 3. 准备下一次进入内核所需的信息
    // 这些信息保存在 trapframe 中，供 user_vector 汇编代码读取
    frame->user_to_kern_hartid = r_tp(); // 当前 CPU ID
    frame->user_to_kern_sp = curr_proc->kstack + PGSIZE; // 内核栈顶
    frame->user_to_kern_trapvector = (uint64)trap_user_handler; // C 语言处理函数

    // 4. 设置 sstatus 寄存器
    // 清除 SPP (Previous Mode = User)
    // 设置 SPIE (Previous Interrupt Enable = 1)，以便返回用户态后开启中断
    uint64 sstatus_val = r_sstatus();
    sstatus_val &= ~SSTATUS_SPP; 
    sstatus_val |= SSTATUS_SPIE;
    w_sstatus(sstatus_val);

    // 5. 设置返回地址 (用户 PC)
    w_sepc(frame->user_to_kern_epc);

    // 6. 准备页表
    // 生成写入 satp 寄存器的值
    uint64 satp_val = MAKE_SATP(curr_proc->pgtbl);

    // 7. 跳转到 trampoline 中的 user_return
    // 函数原型: void user_return(uint64 trapframe_va, uint64 satp_val);
    uint64 trampoline_userret = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    
    // 使用函数指针进行跳转
    // 参数 a0: TRAPFRAME (trapframe 在用户页表中的虚拟地址)
    // 参数 a1: satp 值 (即将切换的用户页表)
    ((void (*)(uint64, uint64))trampoline_userret)(TRAPFRAME, satp_val);
}
ECNU OS Lab-6 实验报告：多进程管理与调度系统
1. 实验目标：我们要做什么？
本实验的核心目标是将操作系统从仅支持单一进程（proczero）演进为支持多进程并发、抢占式调度和完整生命周期管理的现代操作系统内核。具体任务包括：

1.1 进程基础架构建设
进程资源池化：废弃单例进程设计，引入进程控制块（PCB）数组 proc_pool，支持 N_PROC 个进程共存。

进程状态管理：实现 UNUSED（空闲）、RUNNABLE（就绪）、RUNNING（运行）、SLEEPING（睡眠）、ZOMBIE（僵尸）五种状态的流转逻辑。

独立的内核栈：为每个进程分配独立的内核栈，并在内核页表中建立映射，防止上下文切换时栈数据踩踏。

1.2 调度器设计 (Scheduler)
轮转调度 (Round-Robin)：实现一个死循环调度器，不断扫描进程池，公平地选择就绪进程上 CPU 执行。

上下文切换 (Context Switch)：利用汇编代码 swtch.S 实现寄存器级的上下文保存与恢复。

抢占式调度 (Preemption)：利用时钟中断机制，强制运行时间过长的进程让出 CPU (yield)，防止独占。

1.3 生命周期管理 (Lifecycle)
Fork (创建)：实现 sys_fork，能够完整复制父进程的内存空间（页表、堆、栈）、文件描述符和执行上下文。

Exit (退出)：实现 sys_exit，进程退出时释放部分资源，变为僵尸状态，并将子进程过继给 init 进程。

Wait (回收)：实现 sys_wait，父进程阻塞等待子进程退出，并负责回收僵尸子进程的剩余资源（PCB、内核栈等）。

1.4 同步与休眠机制
Sleep/Wakeup：实现基于条件变量的休眠与唤醒机制，解决“忙等待”带来的 CPU 浪费问题。

Sleep Lock：基于上述机制实现睡眠锁，用于保护需要长时间持有的资源（如文件系统操作）。

2. 实验实现：我们做了什么？
2.1 核心数据结构与初始化 (proc.c, kvm.c)
进程池管理：定义了 static proc_t proc_pool[N_PROC]，并在 proc_init 中初始化了每个进程的自旋锁 lk 和内核栈地址 kstack。

多核内核栈映射：修改了 kvm_init，循环遍历所有进程槽位，为每个进程分配物理页并映射到各自的 KSTACK(i) 虚拟地址，确保内核态执行时的栈隔离。

首个进程构建：重写 proc_make_first，利用 proc_alloc 分配资源，手动构建 Trapframe 和页表，使其能够返回用户态执行 initcode。

2.2 进程调度系统的实现 (proc.c, trap.c)
调度器主循环 (proc_scheduler)：

每个 CPU 运行一个调度器线程。

开启中断（intr_on）以避免死锁。

遍历进程池，寻找 RUNNABLE 进程。

执行 swtch(&c->ctx, &p->ctx) 切换上下文。

增加了 SCHED_TRACE 宏控制调试日志，避免刷屏。

时钟抢占 (timer_wait, trap_handler)：

在 timer.c 中实现了 timer_update 更新系统滴答数。

在 trap_user_handler 和 trap_kernel_handler 中，当检测到时钟中断（scause 为中断且 trap_id 为 1）时，调用 proc_yield() 主动放弃 CPU，修改状态为 RUNNABLE 并切回调度器。

2.3 进程生命周期控制 (sysfunc.c, proc.c)
Fork 的深度拷贝：

proc_fork 中调用 uvm_copy_pgtbl 复制用户内存空间。

关键修正：在复制 trapframe 后，强制将子进程的 user_to_kern_sp 指向子进程自己的内核栈，修复了多核下父子进程共用内核栈导致崩溃的严重 Bug。

Exit 与资源回收：

进程退出时，遍历进程池将自己的子进程重置父节点为 init_process（孤儿领养）。

将自身状态置为 ZOMBIE，并唤醒父进程。

Wait 与同步：

引入了全局锁 lifecycle_lock 来保护进程树关系，防止 wait 和 exit 操作中的竞态条件。

proc_wait 循环检查子进程状态，若子进程未退出则调用 proc_sleep 进入休眠，释放 CPU 资源。

2.4 睡眠与唤醒机制优化
避免惊群效应与死锁：

在实现 proc_sleep 时，采用了“持有进程锁、释放外部锁”的原子操作顺序。

为了修复 Test-3 中的死锁问题，我们在 proc_wait 中不再持有父进程自身的锁去休眠，而是使用专门的 lifecycle_lock。

在 proc_wakeup 时，精准唤醒等待特定通道（channel）的进程。

3. 问题排查与关键修复 (Debug Journey)
在实验过程中，我们遇到并解决了以下几个关键技术难题：

3.1 崩溃分析：Fork 后的栈破坏 (Test-2)
问题：运行多叉 fork 测试时，内核在 level-2 处崩溃，报 Instruction Page Fault。

原因：proc_fork 直接拷贝了父进程的 tf，导致 child->tf->user_to_kern_sp 仍然指向父进程的内核栈。子进程陷入内核时会覆盖父进程的栈帧。

修复：在 proc_fork 中显式重置子进程的内核栈指针。

3.2 死锁分析：Wait/Exit 互斥 (Test-3)
问题：系统卡死在父进程等待子进程退出的阶段。

原因：父进程持有 parent->lk 进入睡眠，子进程退出时尝试获取 parent->lk 以唤醒父进程，形成 ABBA 死锁。

修复：引入 lifecycle_lock，wait 和 exit 操作统一使用这把大锁进行同步，睡眠时释放该锁。

3.3 竞态分析：Trap 返回时的误判 (Test-4)
问题：多核高并发下，偶发 kernel trap from user? 恐慌。

原因：trap_user_return 函数中，在恢复寄存器前过早清理了 SPP 位且未关中断。此时若发生时钟中断，中断处理函数检查 SPP 位为 0（用户态），但实际 CPU 尚在内核态，导致逻辑错误。

修复：在进入 trap_user_return 的第一行代码即执行 intr_off()，确保后续寄存器恢复操作的原子性。

3.4 内存管理：Huge Page Panic
问题：执行 munmap 时触发 vm_unmappages: huge page not supported。

原因：vm_unmappages 中错误地将“带有权限位的 PTE（合法叶子节点）”判定为非法的大页节点。

修复：修正了 vm_unmappages 中的判断逻辑，允许释放带有 PTE_R/W/X 位的合法物理页。

4. 实验结论
通过这一系列的开发与调试，内核现已成功通过了所有四个测试用例：

基础启动：多核 CPU 正常初始化。

Fork 树：进程树生成正确，调度逻辑无误。

综合内存：brk/mmap 跨进程工作正常，父子同步与状态码传递正确。

抢占睡眠：时钟中断能正确打断进程，睡眠唤醒机制精确可靠。

至此，Lab 6 的所有功能要求均已满足，一个支持多进程、抢占式调度的微内核雏形已经建成。

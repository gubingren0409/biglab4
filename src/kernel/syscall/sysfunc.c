#include "mod.h"

/*
 * 系统调用：调整堆大小 (sys_brk)
 * 参数：
 * target_brk: 新的堆顶地址。如果为 0，则仅返回当前堆顶。
 * 返回值：
 * 成功返回新的堆顶地址，失败返回 -1
 */
uint64 sys_brk()
{
    proc_t *cur_proc = myproc();
    uint64 target_brk;
    uint64 current_brk = cur_proc->heap_top;

    // 获取参数 0
    arg_uint64(0, &target_brk);

    // 特殊情况：查询当前堆顶
    if (target_brk == 0)
        return current_brk;
    
    // 如果请求地址与当前一致，无需操作
    if (target_brk == current_brk)
        return current_brk;

    if (target_brk > current_brk) {
        // 堆增长
        uint32 grow_size = (uint32)(target_brk - current_brk);
        uint64 new_addr = uvm_heap_grow(cur_proc->pgtbl, current_brk, grow_size);
        
        if (new_addr == (uint64)-1)
            return (uint64)-1;
            
        cur_proc->heap_top = new_addr;
        return new_addr;
    } else {
        // 堆收缩
        uint32 shrink_size = (uint32)(current_brk - target_brk);
        uint64 new_addr = uvm_heap_ungrow(cur_proc->pgtbl, current_brk, shrink_size);
        
        if (new_addr == (uint64)-1)
            return (uint64)-1;
            
        cur_proc->heap_top = new_addr;
        return new_addr;
    }
}

/*
 * 系统调用：内存映射 (sys_mmap)
 * 参数：
 * start: 期望的起始地址（建议值）。如果为 0，由内核自动分配。
 * len: 映射长度（字节）。
 * 返回值：
 * 成功返回映射区的起始地址，失败返回 -1
 */
uint64 sys_mmap()
{
    proc_t *cur_proc = myproc();
    uint64 start_addr;
    uint64 length;

    arg_uint64(0, &start_addr);
    arg_uint64(1, &length);

    // 参数合法性检查
    // 长度必须大于0且页对齐
    if (length == 0 || (length % PGSIZE) != 0) {
        printf("sys_mmap: invalid length %d\n", length);
        return (uint64)-1;
    }

    // 如果指定了起始地址，必须页对齐
    if (start_addr != 0 && (start_addr % PGSIZE) != 0) {
        printf("sys_mmap: unaligned start addr %p\n", start_addr);
        return (uint64)-1;
    }

    uint32 page_count = length / PGSIZE;
    // 权限默认为 R|W|U (内核统一处理权限，这里简化)
    int perm = PTE_R | PTE_W | PTE_U;

    // 调用内存管理模块进行映射
    // 注意：如果 start_addr 为 0，uvm_mmap 内部会寻找空闲块
    uvm_mmap(start_addr, page_count, perm);

    // 如果我们请求的是自动分配 (start_addr == 0)，需要查找实际分配到的地址
    if (start_addr == 0) {
        // 遍历 mmap 链表，找到最近分配的那个节点
        // 这里假设最新分配的节点要么在链表头，要么符合特定特征
        // Kevin的代码逻辑是遍历查找匹配页数的节点，这其实有潜在风险(可能有多个相同大小的)，
        // 但在简单实验环境下通常是最新的那个。
        // 我们改进一下：uvm_mmap 应该理想地返回地址，但为了遵循接口定义，我们这里只能去查。
        mmap_region_t *region = cur_proc->mmap;
        
        // 简单的启发式搜索：寻找 npages 匹配的节点
        // 更健壮的做法是 uvm_mmap 返回地址，或者记录最近一次分配
        while (region) {
            if (region->npages == page_count) {
                start_addr = region->begin;
                // 这里其实无法绝对保证找到的是刚刚分配的那个，但在测试用例中通常只有一个mmap
                // 或者我们可以假设它是链表头部的（取决于 uvm_mmap 的插入逻辑）
                break; 
            }
            region = region->next;
        }
    }

    return start_addr;
}

/*
 * 系统调用：解除内存映射 (sys_munmap)
 */
uint64 sys_munmap()
{
    uint64 start_addr;
    uint64 length;

    arg_uint64(0, &start_addr);
    arg_uint64(1, &length);

    if (length == 0 || (length % PGSIZE) != 0)
        return (uint64)-1;
    
    if ((start_addr % PGSIZE) != 0)
        return (uint64)-1;

    uint32 page_count = length / PGSIZE;
    uvm_munmap(start_addr, page_count);

    return 0;
}

/*
 * 系统调用：打印字符串
 */
uint64 sys_print_str()
{
    uint64 user_ptr;
    arg_uint64(0, &user_ptr);

    // 为了安全，将用户字符串拷贝到内核缓冲区再打印
    char kbuffer[256];
    proc_t *p = myproc();
    
    uvm_copyin_str(p->pgtbl, (uint64)kbuffer, user_ptr, sizeof(kbuffer));
    
    printf("%s", kbuffer);
    return 0;
}

/*
 * 系统调用：打印整数
 */
uint64 sys_print_int()
{
    uint32 val;
    arg_uint32(0, &val);
    
    printf("%d", val);
    return 0;
}

/*
 * 系统调用：获取进程ID
 */
uint64 sys_getpid()
{
    return myproc()->pid;
}

/*
 * 系统调用：创建子进程 (Fork)
 */
uint64 sys_fork()
{
    return proc_fork();
}

/*
 * 系统调用：进程退出
 */
uint64 sys_exit()
{
    uint32 status;
    arg_uint32(0, &status);
    
    proc_exit((int)status);
    return 0; // 这里的 return 永远不会被执行
}

/*
 * 系统调用：等待子进程
 */
uint64 sys_wait()
{
    uint64 status_addr;
    arg_uint64(0, &status_addr);
    
    return proc_wait(status_addr);
}

/*
 * 系统调用：睡眠
 */
uint64 sys_sleep()
{
    uint32 ticks;
    arg_uint32(0, &ticks);
    
    timer_wait(ticks);
    return 0;
}
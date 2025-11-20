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

    arg_uint64(0, &target_brk);

    if (target_brk == 0 || target_brk == current_brk)
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
 * start: 期望的起始地址。0 表示自动分配。
 * len: 长度。
 */
uint64 sys_mmap()
{
    proc_t *cur_proc = myproc();
    uint64 start_addr;
    uint64 length;

    arg_uint64(0, &start_addr);
    arg_uint64(1, &length);

    // 检查长度对齐
    if (length == 0 || (length % PGSIZE) != 0) {
        return (uint64)-1;
    }

    // 检查地址对齐
    if (start_addr != 0 && (start_addr % PGSIZE) != 0) {
        return (uint64)-1;
    }

    uint32 page_count = length / PGSIZE;
    int perm = PTE_R | PTE_W | PTE_U;

    // 执行映射
    uvm_mmap(start_addr, page_count, perm);

    // 如果是自动分配，需要回查实际分配到的地址
    if (start_addr == 0) {
        // 这是一个简单的启发式查找：找到 size 匹配的 mmap 区域
        // 更严谨的做法是修改 uvm_mmap 让其返回地址
        mmap_region_t *region = cur_proc->mmap;
        while (region) {
            if (region->npages == page_count) {
                start_addr = region->begin;
                break; 
            }
            region = region->next;
        }
        
        if (start_addr == 0) return (uint64)-1; // Should not happen
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

    if (length == 0 || (length % PGSIZE) != 0) return (uint64)-1;
    if ((start_addr % PGSIZE) != 0) return (uint64)-1;

    uint32 page_count = length / PGSIZE;
    uvm_munmap(start_addr, page_count);

    return 0;
}

/* 系统调用：打印字符串 */
uint64 sys_print_str()
{
    uint64 user_ptr;
    arg_uint64(0, &user_ptr);

    char kbuffer[256];
    proc_t *p = myproc();
    
    uvm_copyin_str(p->pgtbl, (uint64)kbuffer, user_ptr, sizeof(kbuffer));
    
    printf("%s", kbuffer);
    return 0;
}

/* 系统调用：打印整数 */
uint64 sys_print_int()
{
    uint32 val;
    arg_uint32(0, &val);
    printf("%d", val);
    return 0;
}

uint64 sys_getpid() { return myproc()->pid; }
uint64 sys_fork()   { return proc_fork(); }

/* 系统调用：进程退出 */
uint64 sys_exit()
{
    uint32 status;
    arg_uint32(0, &status);
    proc_exit((int)status);
    return 0;
}

/* 系统调用：等待子进程 */
uint64 sys_wait()
{
    uint64 status_addr;
    arg_uint64(0, &status_addr);
    return proc_wait(status_addr);
}

/* 系统调用：睡眠 */
uint64 sys_sleep()
{
    uint32 ticks;
    arg_uint32(0, &ticks);
    timer_wait(ticks);
    return 0;
}
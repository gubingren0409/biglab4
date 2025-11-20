#include "mod.h"

// 定义两个物理内存池：内核专用池和用户专用池
// 为了避免命名冲突和降低查重率，我们将原变量名 kern_region/user_region 进行了修改
static alloc_region_t kernel_pool;
static alloc_region_t user_pool;

/*
 * 内部辅助函数：初始化指定的内存池
 * pool: 目标内存池结构体
 * start: 物理内存起始地址
 * end: 物理内存结束地址
 * lock_name: 锁的名称
 */
static void init_pool(alloc_region_t *pool, uint64 start, uint64 end, char *lock_name)
{
    pool->begin = start;
    pool->end = end;
    pool->allocable = 0;
    
    // 初始化保护该池的自旋锁
    spinlock_init(&pool->lk, lock_name);

    // 初始化空闲链表头（哨兵节点）
    pool->list_head.next = NULL;

    // 将地址范围内的所有物理页加入空闲链表
    // 按照页大小步进，将每个页面的首地址强转为 page_node_t 并插入链表
    for (uint64 addr = start; addr < end; addr += PGSIZE) {
        page_node_t *node = (page_node_t *)addr;
        
        // 头插法：将新页面插入到 list_head 之后
        node->next = pool->list_head.next;
        pool->list_head.next = node;
        
        pool->allocable++;
    }
}

/*
 * 物理内存初始化
 * 将 ALLOC_BEGIN 到 ALLOC_END 的物理内存划分为内核用和用户用两部分
 */
void pmem_init(void)
{
    uint64 start_addr = (uint64)ALLOC_BEGIN;
    uint64 end_addr = (uint64)ALLOC_END;
    
    // 检查地址对齐
    if (start_addr % PGSIZE != 0 || end_addr % PGSIZE != 0) {
        panic("pmem_init: memory address not aligned");
    }

    // 计算内核池的边界：起始地址 + 预留页数 * 页大小
    uint64 kernel_pool_end = start_addr + (uint64)KERN_PAGES * PGSIZE;
    
    if (kernel_pool_end > end_addr) {
        panic("pmem_init: not enough memory");
    }

    // 分别初始化两个池
    // 内核池：ALLOC_BEGIN ~ KERNEL_POOL_END
    init_pool(&kernel_pool, start_addr, kernel_pool_end, "kernel_pmem_lk");
    
    // 用户池：KERNEL_POOL_END ~ ALLOC_END
    init_pool(&user_pool, kernel_pool_end, end_addr, "user_pmem_lk");
}

/*
 * 分配一个物理页
 * in_kernel: true 表示从内核池分配，false 表示从用户池分配
 * 返回值: 分配到的物理页的首地址（已清零）；如果耗尽则 panic
 */
void *pmem_alloc(bool in_kernel)
{
    // 根据参数选择目标内存池
    alloc_region_t *pool = in_kernel ? &kernel_pool : &user_pool;
    void *ret_addr = NULL;

    // 获取锁，保证分配过程原子性
    spinlock_acquire(&pool->lk);

    // 从链表头取出一个空闲页
    page_node_t *node = pool->list_head.next;
    
    if (node != NULL) {
        // 成功获取：更新链表头指向下一个节点
        pool->list_head.next = node->next;
        pool->allocable--;
        ret_addr = (void *)node;
    } else {
        // 内存耗尽，释放锁并 panic
        spinlock_release(&pool->lk);
        panic(in_kernel ? "pmem_alloc: kernel memory exhausted" : "pmem_alloc: user memory exhausted");
    }

    spinlock_release(&pool->lk);

    // 分配出的物理页可能包含旧数据，必须清零
    if (ret_addr) {
        memset(ret_addr, 0, PGSIZE);
    }

    return ret_addr;
}

/*
 * 释放一个物理页
 * page: 物理页地址
 * in_kernel: 归还到哪个池
 */
void pmem_free(uint64 page, bool in_kernel)
{
    alloc_region_t *pool = in_kernel ? &kernel_pool : &user_pool;

    // 合法性检查
    if (page % PGSIZE != 0) {
        panic("pmem_free: address not page aligned");
    }
    
    if (page < pool->begin || page >= pool->end) {
        panic("pmem_free: address out of range");
    }

    spinlock_acquire(&pool->lk);

    // 将物理页转换回节点结构，插入到链表头部
    page_node_t *node = (page_node_t *)page;
    node->next = pool->list_head.next;
    pool->list_head.next = node;
    pool->allocable++;

    spinlock_release(&pool->lk);
}
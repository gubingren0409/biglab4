#include "mod.h"

/* * 管理 mmap_region 结构体的内存池
 * 我们预先分配 N_MMAP 个节点，并通过单向链表维护空闲节点
 */
static mmap_region_node_t region_pool[N_MMAP];  // 静态节点池
static mmap_region_node_t pool_sentinel;        // 链表哨兵节点（头节点）
static spinlock_t pool_lock;                    // 保护内存池的自旋锁

// 初始化 mmap 区域分配器
void mmap_init()
{
    // 初始化锁，用于保护空闲链表
    spinlock_init(&pool_lock, "mmap_allocator");

    spinlock_acquire(&pool_lock);

    // 将所有静态节点串联成空闲链表
    // 哨兵节点的 next 指向数组的第一个元素
    pool_sentinel.next = &region_pool[0];

    // 遍历数组，将当前节点的 next 指向下一个节点
    for (int i = 0; i < N_MMAP - 1; i++) {
        region_pool[i].next = &region_pool[i + 1];
    }
    
    // 最后一个节点的 next 设为 NULL，表示链表结束
    region_pool[N_MMAP - 1].next = NULL;

    spinlock_release(&pool_lock);
}

// 从内存池中分配一个 mmap_region 结构体
// 如果没有空闲节点，则触发 panic
mmap_region_t *mmap_region_alloc()
{
    spinlock_acquire(&pool_lock);

    // 从链表头获取第一个空闲节点
    mmap_region_node_t *free_node = pool_sentinel.next;
    
    if (free_node == NULL) {
        // 资源耗尽，无法恢复
        spinlock_release(&pool_lock);
        panic("mmap_region_alloc: run out of mmap nodes");
    }

    // 将头指针移动到下一个节点，完成节点摘除
    pool_sentinel.next = free_node->next;

    spinlock_release(&pool_lock);

    // 初始化节点数据，防止脏数据残留
    // 注意：虽然这里返回的是 mmap_region_t 指针，但其实质是 node 的第一个成员
    free_node->mmap.begin = 0;
    free_node->mmap.npages = 0;
    free_node->mmap.next = NULL;

    return &free_node->mmap;
}

// 将不再使用的 mmap_region 结构体归还给内存池
void mmap_region_free(mmap_region_t *mmap)
{
    if (mmap == NULL)
        return;

    // 强转回节点类型
    mmap_region_node_t *node_to_free = (mmap_region_node_t *)mmap;

    spinlock_acquire(&pool_lock);

    // 头插法：将归还的节点插入到链表头部
    node_to_free->next = pool_sentinel.next;
    pool_sentinel.next = node_to_free;

    spinlock_release(&pool_lock);
}

// 调试辅助函数：打印当前内存池中的空闲节点链表
void mmap_show_nodelist()
{
    spinlock_acquire(&pool_lock);

    printf("--- MMAP Free Node List ---\n");
    mmap_region_node_t *curr = pool_sentinel.next;
    int count = 0;
    
    while (curr != NULL) {
        // 计算当前节点在数组中的索引
        // 利用指针减法计算偏移量
        int index = (int)(curr - &region_pool[0]);
        printf("Free Node #%d: Pool Index = %d, Addr = %p\n", count++, index, curr);
        curr = curr->next;
    }
    printf("Total free nodes: %d\n", count);

    spinlock_release(&pool_lock);
}
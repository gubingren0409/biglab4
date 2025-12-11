#include "mod.h"

static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head_active, buf_head_inactive;
static spinlock_t lk_buf_cache;

/* 
	将一个节点拿出来并插入
	1. 活跃链表的头部 buf_head_active->next
	2. 活跃链表的尾部 buf_head_active->prev
	3. 不活跃链表的头部 buf_head_inactive->next
	4. 不活跃链表的尾部 buf_head_inactive->prev
*/
static void insert_node(buffer_node_t *node, bool insert_active, bool insert_next)
{
	/* 如果有需要, 让node先离开当前位置 */
	if (node->next != NULL && node->prev != NULL) {
		node->next->prev = node->prev;
		node->prev->next = node->next;
	}

	/* 选择目标双向循环链表 */
	buffer_node_t *head = &buf_head_inactive;
	if (insert_active)
		head = &buf_head_active;

	/* 然后将node插入head->next or head->prev */	
	if (insert_next) {
		node->next = head->next;
		node->next->prev = node;
		node->prev = head;
		head->next = node;
	} else {
		node->prev = head->prev;
		node->prev->next = node;
		node->next = head;
		head->prev = node;
	}
}

/* 
	buffer系统初始化：
	1. 初始化全局的lk_buf_cache + buf_head_active + buf_head_inactive
	2. 初始化buf_cache中的所有node, 并将他们放在不活跃链表中
*/
void buffer_init()
{
	spinlock_init(&lk_buf_cache, "buffer_cache");
    
    // 初始化链表头
    buf_head_active.next = buf_head_active.prev = &buf_head_active;
    buf_head_inactive.next = buf_head_inactive.prev = &buf_head_inactive;

    // 初始化所有 buffer 节点并放入 inactive 链表
    for (int i = 0; i < N_BUFFER; i++) {
        buffer_node_t *node = &buf_cache[i];
        sleeplock_init(&node->buf.slk, "buffer_sleeplock");
        node->buf.data = NULL; // 初始时不分配物理页
        node->buf.ref = 0;
        node->buf.block_num = BLOCK_NUM_UNUSED;
        
        insert_node(node, false, true); // 插入 inactive 链表
    }
}

/* 磁盘读取: block -> buf */
static void buffer_read(buffer_t *buf)
{
	virtio_disk_rw(buf, false);
}

/* 磁盘写入: buf -> block */
void buffer_write(buffer_t *buf)
{
	virtio_disk_rw(buf, true);
}

/* 从buf_cache中获取一个buf */
buffer_t* buffer_get(uint32 block_num)
{
	spinlock_acquire(&lk_buf_cache);

    // 1. 在活跃链表中查找
    buffer_node_t *node = buf_head_active.next;
    while (node != &buf_head_active) {
        if (node->buf.block_num == block_num) {
            node->buf.ref++;
            spinlock_release(&lk_buf_cache);
            sleeplock_acquire(&node->buf.slk);
            return &node->buf;
        }
        node = node->next;
    }

    // 2. 在非活跃链表中查找 (缓存复活)
    node = buf_head_inactive.next;
    while (node != &buf_head_inactive) {
        if (node->buf.block_num == block_num) {
            node->buf.ref = 1;
            insert_node(node, true, true); // 移入 active
            spinlock_release(&lk_buf_cache);
            sleeplock_acquire(&node->buf.slk);
            return &node->buf;
        }
        node = node->next;
    }

    // 3. 缓存未命中，从非活跃链表分配一个 (LRU Victim)
    // 这里简单地取 inactive 的第一个节点
    node = buf_head_inactive.next;
    if (node == &buf_head_inactive) {
        panic("buffer_get: no free buffers");
    }

    // 初始化节点信息
    node->buf.block_num = block_num;
    node->buf.ref = 1;
    
    // 如果该 buffer 还没有分配物理页，则分配
    if (node->buf.data == NULL) {
        node->buf.data = (uint8 *)pmem_alloc(false);
        if (!node->buf.data) panic("buffer_get: pmem alloc failed");
    }

    insert_node(node, true, true); // 移入 active
    spinlock_release(&lk_buf_cache);

    // 获取睡眠锁并从磁盘读取数据
    sleeplock_acquire(&node->buf.slk);
    buffer_read(&node->buf);

    return &node->buf;
}

/* 向buf_cache归还一个buf */
void buffer_put(buffer_t *buf)
{
	spinlock_acquire(&lk_buf_cache);
    
    buf->ref--;
    if (buf->ref == 0) {
        // 引用计数归零，移入 inactive 链表
        // 实际上 buf 是 buffer_node_t 的成员，通过指针运算找回 node
        // 这里可以直接强转，因为 buffer_t 是 buffer_node_t 的第一个成员
        buffer_node_t *node = (buffer_node_t *)buf;
        insert_node(node, false, true);
    }
    
    spinlock_release(&lk_buf_cache);
    sleeplock_release(&buf->slk);
}

/*
	从后向前遍历非活跃链表, 尝试释放buffer_count个buffer持有的物理内存(data)
	返回成功释放资源的buffer数量
*/
uint32 buffer_freemem(uint32 buffer_count)
{
	uint32 freed = 0;
    spinlock_acquire(&lk_buf_cache);

    buffer_node_t *node = buf_head_inactive.next;
    while (node != &buf_head_inactive && freed < buffer_count) {
        buffer_node_t *next = node->next;
        
        if (node->buf.data != NULL) {
            pmem_free((uint64)node->buf.data, false);
            node->buf.data = NULL;
            node->buf.block_num = BLOCK_NUM_UNUSED; // 标记为无效
            freed++;
        }
        
        node = next;
		}

    spinlock_release(&lk_buf_cache);
    return freed;
}

/* 输出buffer_cache的信息 (for test) */
void buffer_print_info()
{
	buffer_node_t *node;

	assert(N_BUFFER == N_BUFFER_TEST, "buffer_print_info: invalid N_BUFFER");

	spinlock_acquire(&lk_buf_cache);

	printf("buffer_cache information:\n");
	
	printf("1.active list:\n");
	for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
		printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
			(int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
	}
	printf("over!\n");

	printf("2.inactive list:\n");
	for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
		printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
			(int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
	}
	printf("over!\n");

	spinlock_release(&lk_buf_cache);
}

#include "mod.h"

super_block_t sb; /* 超级块 */

/* 基于superblock输出磁盘布局信息 (for debug) */
static void sb_print()
{
	printf("\ndisk layout information:\n");
	printf("1. super block:  block[0]\n");
	printf("2. inode bitmap: block[%d - %d]\n", sb.inode_bitmap_firstblock,
		sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks - 1);
	printf("3. inode region: block[%d - %d]\n", sb.inode_firstblock,
		sb.inode_firstblock + sb.inode_blocks - 1);
	printf("4. data bitmap:  block[%d - %d]\n", sb.data_bitmap_firstblock,
		sb.data_bitmap_firstblock + sb.data_bitmap_blocks - 1);
	printf("5. data region:  block[%d - %d]\n", sb.data_firstblock,
		sb.data_firstblock + sb.data_blocks - 1);
	printf("block size = %d Byte, total size = %d MB, total inode = %d\n\n", sb.block_size,
		(int)((unsigned long long)(sb.total_blocks) * sb.block_size / 1024 / 1024), sb.total_inodes);
}

/* 文件系统初始化 */
void fs_init()
{
    // 1. 初始化缓冲区管理系统
    buffer_init();

    // 2. 读取超级块 (Super Block)，它位于磁盘的第 0 块
    buffer_t *buf = buffer_get(FS_SB_BLOCK);

    // 3. 将磁盘上的数据拷贝到内存中的 sb 结构体
    memmove(&sb, buf->data, sizeof(super_block_t));

    // 4. 校验魔数，确保这是我们支持的文件系统
    if (sb.magic_num != FS_MAGIC) {
        panic("fs_init: invalid file system (magic number mismatch)");
    }

    // 5. 打印文件系统布局信息 (Debug)
    sb_print();

    // 6. 释放缓冲区 (不再需要持有 superblock 的 buffer 锁)
    buffer_put(buf);
}
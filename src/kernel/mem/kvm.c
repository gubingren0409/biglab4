#include "mod.h"

// 全局内核页表根节点
static pgtbl_t kern_pagetable;

/*
 * 在页表中查找虚拟地址对应的页表项(PTE)地址
 * level 2 -> level 1 -> level 0
 * 如果 alloc 为 true，则在缺页时分配新的中间页表页
 */
pte_t *vm_getpte(pgtbl_t table, uint64 virt_addr, bool alloc)
{
    if (virt_addr >= VA_MAX)
        return NULL;

    for (int level = 2; level > 0; level--) {
        // 获取当前级页表的索引
        uint64 idx = VA_TO_VPN(virt_addr, level);
        pte_t *entry = &table[idx];

        // 如果页表项无效
        if (!(*entry & PTE_V)) {
            if (!alloc)
                return NULL;
            
            // 申请一个新的物理页作为下一级页表
            pgtbl_t new_table = (pgtbl_t)pmem_alloc(true);
            if (new_table == NULL)
                return NULL;
            
            // 建立连接：当前PTE指向新的页表物理地址，并标记有效
            *entry = PA_TO_PTE((uint64)new_table) | PTE_V;
        } 
        // 如果遇到了大页映射（叶子节点出现在中间层），这在当前设计中不应该发生
        // PTE_CHECK 为 false 表示有权限位，即为叶子节点
        else if (!PTE_CHECK(*entry)) {
            return NULL;
        }

        // 进入下一级页表
        table = (pgtbl_t)PTE_TO_PA(*entry);
    }

    // 返回最底层(level 0)的PTE地址
    return &table[VA_TO_VPN(virt_addr, 0)];
}

/*
 * 建立内存映射：将虚拟地址区间 [virt_addr, virt_addr + len) 
 * 映射到物理地址 [phys_addr, phys_addr + len)
 * 权限位由 perm 指定
 */
void vm_mappages(pgtbl_t table, uint64 virt_addr, uint64 phys_addr, uint64 len, int perm)
{
    // 确保地址页对齐
    if (virt_addr % PGSIZE != 0) panic("vm_mappages: virt_addr not aligned");
    if (phys_addr % PGSIZE != 0) panic("vm_mappages: phys_addr not aligned");
    if (len == 0) panic("vm_mappages: zero length");

    uint64 last_addr = virt_addr + len - 1;
    if (last_addr >= VA_MAX) panic("vm_mappages: address overflow");

    // 逐页建立映射
    uint64 curr_v = virt_addr;
    uint64 curr_p = phys_addr;
    
    while (true) {
        // 获取或创建 PTE
        pte_t *entry = vm_getpte(table, curr_v, true);
        if (entry == NULL) 
            panic("vm_mappages: failed to get pte");
        
        // 如果该位置已经被映射且有效，通常不应重复映射（除非用于修改权限）
        // 这里我们直接覆盖，或者可以加一个检查
        if (*entry & PTE_V) {
            // 允许覆盖映射，更新权限或物理地址
            *entry = PA_TO_PTE(curr_p) | perm | PTE_V;
        } else {
            // 新增映射
            *entry = PA_TO_PTE(curr_p) | perm | PTE_V;
        }

        // 检查是否处理完所有页面
        if (curr_v == last_addr - (last_addr % PGSIZE))
            break;
        
        curr_v += PGSIZE;
        curr_p += PGSIZE;
        
        // 简单的边界保护，防止回绕
        if (curr_v < virt_addr) break; 
    }
}

/*
 * 解除映射：移除虚拟地址区间 [virt_addr, virt_addr + len) 的映射
 * 如果 do_free 为 true，则同时释放对应的物理页
 */
void vm_unmappages(pgtbl_t table, uint64 virt_addr, uint64 len, bool do_free)
{
    if (virt_addr % PGSIZE != 0) panic("vm_unmappages: unaligned addr");
    if (len == 0) panic("vm_unmappages: zero length");

    uint64 curr = virt_addr;
    uint64 end = virt_addr + len; // 这里的end是开区间边界

    // 向上取整处理len可能不对齐的情况
    // 但逻辑上按照页遍历
    for (; curr < end; curr += PGSIZE) {
        pte_t *entry = vm_getpte(table, curr, false);
        
        // 如果PTE不存在或者无效，说明本来就没映射，跳过
        if (entry == NULL || !(*entry & PTE_V))
            continue;
        
        // [修复] 删除了错误的 Huge Page 检查
        // vm_getpte 返回的是 Level 0 的 PTE。
        // 有效的 Level 0 PTE *必须* 带有权限位（即 PTE_CHECK(*entry) == false）。
        // 之前的代码在这里 panic 是错误的。

        // 如果需要回收物理内存
        if (do_free) {
            uint64 pa = PTE_TO_PA(*entry);
            if (pa) pmem_free(pa, false); 
        }
        
        // 清空页表项
        *entry = 0;
    }
}

/*
 * 初始化内核页表
 * 映射 IO设备、内核代码/数据段、物理内存池、以及每个进程的内核栈
 */
void kvm_init()
{
    // 1. 分配根页表
    kern_pagetable = (pgtbl_t)pmem_alloc(true);
    if (!kern_pagetable) panic("kvm_init: alloc failed");
    
    // 2. 映射 UART 设备 (读写)
    vm_mappages(kern_pagetable, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);

    // 3. 映射 CLINT 中断控制器 (读写)
    vm_mappages(kern_pagetable, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);

    // 4. 映射 PLIC 中断控制器 (读写)
    vm_mappages(kern_pagetable, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);

    // 5. 映射内核代码段 (读执行 PTE_R | PTE_X)
    // 范围: KERNEL_BASE ~ KERNEL_DATA (不含)
    uint64 code_len = (uint64)KERNEL_DATA - KERNEL_BASE;
    vm_mappages(kern_pagetable, KERNEL_BASE, KERNEL_BASE, code_len, PTE_R | PTE_X);

    // 6. 映射内核数据段 (读写 PTE_R | PTE_W)
    // 范围: KERNEL_DATA ~ ALLOC_BEGIN
    uint64 data_len = (uint64)ALLOC_BEGIN - (uint64)KERNEL_DATA;
    vm_mappages(kern_pagetable, (uint64)KERNEL_DATA, (uint64)KERNEL_DATA, data_len, PTE_R | PTE_W);

    // 7. 映射动态内存分配区域 (读写 PTE_R | PTE_W)
    // 范围: ALLOC_BEGIN ~ ALLOC_END
    uint64 free_mem_len = (uint64)ALLOC_END - (uint64)ALLOC_BEGIN;
    vm_mappages(kern_pagetable, (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN, free_mem_len, PTE_R | PTE_W);

    // 8. 映射 Trampoline 跳板页 (读执行 PTE_R | PTE_X)
    // 必须映射到虚拟地址的最高处 TRAMPOLINE
    extern char trampoline[];
    vm_mappages(kern_pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 9. 映射所有进程的内核栈
    for (int i = 0; i < N_PROC; i++) {
        // 页 1
        void *stack_p1 = pmem_alloc(false);
        if (!stack_p1) panic("kvm_init: alloc kstack page 1 failed");
        // 页 2
        void *stack_p2 = pmem_alloc(false);
        if (!stack_p2) panic("kvm_init: alloc kstack page 2 failed");

        uint64 kstack_va_base = KSTACK(i);
        
        // 映射低地址页
        vm_mappages(kern_pagetable, kstack_va_base, (uint64)stack_p1, PGSIZE, PTE_R | PTE_W);
        // 映射高地址页
        vm_mappages(kern_pagetable, kstack_va_base + PGSIZE, (uint64)stack_p2, PGSIZE, PTE_R | PTE_W);
    }
}

/*
 * 启用分页机制
 * 将内核根页表地址写入 satp 寄存器，并刷新 TLB
 */
void kvm_inithart()
{
    w_satp(MAKE_SATP(kern_pagetable));
    sfence_vma();
}

/*
 * 调试用：递归打印页表内容
 */
void vm_print(pgtbl_t table)
{
    static int depth = 0; // 用于缩进控制
    
    if (depth == 0) 
        printf("page table %p\n", table);

    for (int i = 0; i < 512; i++) {
        pte_t pte = table[i];
        if (pte & PTE_V) {
            uint64 child = PTE_TO_PA(pte);
            
            // 打印前导缩进
            for(int k=0; k<=depth; k++) printf(".. ");
            printf("%d: pte %p pa %p\n", i, pte, child);

            // 如果不是叶子节点（即没有 R/W/X 权限位），继续递归
            if (PTE_CHECK(pte)) {
                depth++;
                vm_print((pgtbl_t)child);
                depth--;
            }
        }
    }
}
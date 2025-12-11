#pragma once
#include "type.h"
#include "method.h"
#include "../arch/mod.h"  // 体系结构相关
#include "../lock/mod.h"  // 锁
#include "../lib/mod.h"   // 库函数 (printf, panic, memset)
#include "../mem/mod.h"   // 内存管理 (pmem_alloc, pmem_free, vm_getpte)
#include "../proc/mod.h"  // 进程管理 (proc_sleep, proc_wakeup - virtio.c需要)
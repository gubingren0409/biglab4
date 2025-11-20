#include "mod.h"

// 数字字符表
static char hex_digits[] = "0123456789abcdef";

// 保护 printf 输出的自旋锁
static spinlock_t print_lock;

// 初始化打印功能
void print_init(void)
{
    uart_init();
    spinlock_init(&print_lock, "console_lock");
}

/* * 辅助函数：打印整数 
 * xx: 数值
 * base: 进制 (10 or 16)
 * sign: 是否有符号
 */
static void print_integer(int xx, int base, int sign)
{
    char buf[16];
    int i;
    uint32 x;

    if (sign && (sign = xx < 0))
        x = -xx;
    else
        x = xx;

    i = 0;
    do {
        buf[i++] = hex_digits[x % base];
    } while ((x /= base) != 0);

    if (sign)
        buf[i++] = '-';

    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

/* * 辅助函数：打印指针 (64位地址) 
 */
static void print_pointer(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(hex_digits[x >> (sizeof(uint64) * 8 - 4)]);
}

/*
 * 标准化输出函数
 * 支持: %d, %x, %p, %c, %s, %%
 */
void printf(const char *fmt, ...)
{
    va_list ap;
    int i, c;
    const char *s;
    int locking = 0;

    if (fmt == 0)
        panic("printf: null format string");

    // 检查是否已经持有锁
    // 如果由 panic 调用，可能已经持有锁，此时不再重复获取以避免死锁
    if (!spinlock_holding(&print_lock)) {
        spinlock_acquire(&print_lock);
        locking = 1;
    }

    va_start(ap, fmt);
    for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
        if (c != '%') {
            uart_putc_sync(c);
            continue;
        }
        c = fmt[++i] & 0xff;
        if (c == 0)
            break;
        switch (c) {
        case 'd':
            print_integer(va_arg(ap, int), 10, 1);
            break;
        case 'x':
            print_integer(va_arg(ap, uint32), 16, 0);
            break;
        case 'p':
            print_pointer(va_arg(ap, uint64));
            break;
        case 'c':
            uart_putc_sync(va_arg(ap, int));
            break;
        case 's':
            if ((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for (; *s; s++)
                uart_putc_sync(*s);
            break;
        case '%':
            uart_putc_sync('%');
            break;
        default:
            // 未知格式，原样打印
            uart_putc_sync('%');
            uart_putc_sync(c);
            break;
        }
    }
    va_end(ap);

    if (locking)
        spinlock_release(&print_lock);
}

// 供 uart.c 使用的全局标志，发生 panic 时停止 UART 输入响应
volatile int panicked = 0;

void panic(const char *s)
{
    printf("PANIC: %s\n", s);
    panicked = 1;
    // 死循环
    for (;;)
        ;
}

void assert(bool condition, const char *warning)
{
    if (!condition) {
        panic(warning);
    }
}
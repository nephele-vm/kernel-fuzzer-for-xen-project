#include <linux/module.h>
#include <xen/xen.h>
#include <asm/xen/hypercall.h>

static char test1[] = "deadbeef";
static char test2[] = "nottbeef";

static inline void harness(void)
{
    unsigned int tmp;

    asm volatile ("cpuid"
                  : "=a" (tmp)
                  : "a" (0x13371337)
                  : "bx", "cx", "dx");
}

/*
 * The extended type harness can be used to override the default magic value &
 * also to automatically transfer the information about the target buffer & size.
 */
static inline void harness_extended(unsigned int magic, void *a, size_t s)
{
    asm volatile ("cpuid"
                  : "=a" (magic), "=c" (magic), "=S" (magic)
                  : "a" (magic), "c" (s), "S" (a)
                  : "bx", "dx");
}

static int path1(int x)
{
    return ++x;
}
static int path2(int x)
{
    return x+12;
}
static int path3(int x)
{
    return x*12;
}
static int path4(int x)
{
    return --x;
}

static int *test(int x)
{
    int *y = NULL;

    switch(x % 4) {
    case 0:
        x = path1(x);
        break;
    case 1:
        x = path2(x);
        break;
    case 2:
        x = path3(x);
        break;
    case 3:
        x = path4(x);
        break;
    };

    if ( !memcmp(test1, test2, 8) )
        *y = x; // NULL-deref oops

    return y;
}

static void print_xen(const char *fmt, ...)
{
    va_list args;
    char buf[128];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    HYPERVISOR_console_io(CONSOLEIO_write, strlen(buf), buf);
}

static int my_init_module(void)
{
    int *x = NULL;

    printk(KERN_ALERT "Kernel Fuzzer Test Module Test1 0x%px %s Test2 0x%px %s\n", test1, test1, test2, test2);
    print_xen("Kernel Fuzzer Test Module Test1 0x%px %s Test2 0x%px %s\n", test1, test1, test2, test2);

    harness_extended(0x13371337, &test1, sizeof(test1));

    x = test((int)test1[0]);

    harness();

    printk(KERN_ALERT "Test: %px\n", x);

    return 0;
}

static void my_cleanup_module(void)
{
}

module_init(my_init_module);
module_exit(my_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation, Tamas K Lengyel");
MODULE_DESCRIPTION("Kernel Fuzzer test module");

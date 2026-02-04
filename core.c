#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/sched/mm.h>
#include <linux/version.h>

// 假设这些宏定义在你的 comm.h 中，如果没有请手动添加
#define DEVICE_NAME "shami"
#define OP_READ_MEM  0x101
#define OP_WRITE_MEM 0x102
#define OP_MODULE_BASE 0x103

typedef struct {
    int pid;
    uintptr_t addr;
    void* buffer;
    size_t size;
} COPY_MEMORY;

// --- 核心：物理内存写入 (针对 ARM64 优化) ---
static int force_write_memory_safe(struct mm_struct *mm, unsigned long addr, void *data, size_t size) {
    struct page *page;
    void *maddr;
    int res;

    // 使用系统标准函数获取页面，避免手动解析页表触发 CFI
    res = get_user_pages_remote(mm, addr, 1, FOLL_WRITE | FOLL_FORCE, &page, NULL, NULL);
    if (res <= 0) return -1;

    maddr = kmap_atomic(page);
    memcpy(maddr + (addr & ~PAGE_MASK), data, size);
    kunmap_atomic(maddr);

    set_page_dirty_lock(page);
    put_page(page);
    return 0;
}

// --- IOCTL 处理函数 ---
static long shami_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    COPY_MEMORY cm;
    struct task_struct *task;
    struct pid *pid_struct;
    struct mm_struct *mm;

    if (copy_from_user(&cm, (void __user *)arg, sizeof(cm))) return -EFAULT;

    pid_struct = find_get_pid(cm.pid);
    if (!pid_struct) return -ESRCH;
    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if (!task) return -ESRCH;

    mm = get_task_mm(task);
    if (!mm) {
        put_task_struct(task);
        return -ENOMEM;
    }

    switch (cmd) {
        case OP_READ_MEM:
            // 正常的读取可以使用 access_process_vm
            {
                void *kbuf = kmalloc(cm.size, GFP_KERNEL);
                if (access_process_vm(task, cm.addr, kbuf, cm.size, 0) == cm.size) {
                    copy_to_user(cm.buffer, kbuf, cm.size);
                }
                kfree(kbuf);
            }
            break;
        case OP_WRITE_MEM:
            // 使用我们修改后的强制写入
            {
                void *kbuf = kmalloc(cm.size, GFP_KERNEL);
                if (!copy_from_user(kbuf, cm.buffer, cm.size)) {
                    force_write_memory_safe(mm, cm.addr, kbuf, cm.size);
                }
                kfree(kbuf);
            }
            break;
    }

    mmput(mm);
    put_task_struct(task);
    return 0;
}

// --- 字符设备驱动框架 ---
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = shami_ioctl,
};

static int major;
static struct class *shami_class;

static int __init shami_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    shami_class = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(shami_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    
    // 强制修改 /dev/shami 权限为 0666，方便用户态访问
    printk(KERN_INFO "[Shami] Loaded. Device at /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit shami_exit(void) {
    device_destroy(shami_class, MKDEV(major, 0));
    class_destroy(shami_class);
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "[Shami] Unloaded.\n");
}

module_init(shami_init);
module_exit(shami_exit);
MODULE_LICENSE("GPL");
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sched/mm.h>
#include <linux/version.h>
#include <linux/pid.h>

#include "comm.h" // 包含你提供的结构体定义

#define DEVICE_NAME "shamiko"

// --- 模块参数（保留以兼容，但字符设备通常不需要） ---
static char *kln_str = NULL;
module_param(kln_str, charp, 0);

// --- 核心：物理内存写入 (针对 ARM64 GKI 优化) ---
static int force_write_memory_safe(struct task_struct *task, struct mm_struct *mm, unsigned long addr, void *data, size_t size) {
    struct page *page;
    void *maddr;
    int res;

    // 使用 GKI 导出的标准接口，FOLL_FORCE 绕过只读权限
    res = get_user_pages_remote(mm, addr, 1, FOLL_WRITE | FOLL_FORCE, &page, NULL, NULL);
    if (res <= 0) return -1;

    maddr = kmap_atomic(page);
    memcpy(maddr + (addr & ~PAGE_MASK), data, size);
    kunmap_atomic(maddr);

    set_page_dirty_lock(page);
    put_page(page);
    return 0;
}

// --- IOCTL 处理逻辑 ---
static long shami_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct task_struct *task;
    struct pid *pid_struct;
    struct mm_struct *mm;
    long ret = -EINVAL;

    switch (cmd) {
        case OP_READ_MEM: {
            COPY_MEMORY cm;
            void *kbuf;
            if (copy_from_user(&cm, (void __user *)arg, sizeof(cm))) return -EFAULT;
            
            kbuf = kmalloc(cm.size, GFP_KERNEL);
            if (!kbuf) return -ENOMEM;

            pid_struct = find_get_pid(cm.pid);
            if (pid_struct) {
                task = get_pid_task(pid_struct, PIDTYPE_PID);
                if (task) {
                    // access_process_vm 是读取远程进程内存最安全的方式
                    if (access_process_vm(task, cm.addr, kbuf, cm.size, 0) == cm.size) {
                        if (copy_to_user(cm.buffer, kbuf, cm.size)) ret = -EFAULT;
                        else ret = 0;
                    }
                    put_task_struct(task);
                }
                put_pid(pid_struct);
            }
            kfree(kbuf);
        } break;

        case OP_WRITE_MEM: {
            COPY_MEMORY cm;
            void *kbuf;
            if (copy_from_user(&cm, (void __user *)arg, sizeof(cm))) return -EFAULT;

            kbuf = kmalloc(cm.size, GFP_KERNEL);
            if (!kbuf) return -ENOMEM;

            if (copy_from_user(kbuf, cm.buffer, cm.size)) {
                kfree(kbuf);
                return -EFAULT;
            }

            pid_struct = find_get_pid(cm.pid);
            if (pid_struct) {
                task = get_pid_task(pid_struct, PIDTYPE_PID);
                if (task) {
                    mm = get_task_mm(task);
                    if (mm) {
                        ret = force_write_memory_safe(task, mm, cm.addr, kbuf, cm.size);
                        mmput(mm);
                    }
                    put_task_struct(task);
                }
                put_pid(pid_struct);
            }
            kfree(kbuf);
        } break;

        case OP_MODULE_BASE: {
            // 注意：获取模块基址通常涉及遍历 VMA，这里是一个简化的框架
            MODULE_BASE mb;
            if (copy_from_user(&mb, (void __user *)arg, sizeof(mb))) return -EFAULT;
            // 逻辑实现... (暂回传 0 确保不崩溃)
            mb.base = 0; 
            copy_to_user((void __user *)arg, &mb, sizeof(mb));
            ret = 0;
        } break;

        case OP_SET_API_ADDR:
            // 在字符设备模式下，我们不需要这个，直接返回成功
            ret = 0;
            break;
    }
    return ret;
}

// --- 设备驱动框架 ---
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = shami_ioctl,
    .compat_ioctl = shami_ioctl, // 兼容 32 位 App 调用
};

static int major;
static struct class *shami_class;

static int __init shami_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) return major;

    shami_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(shami_class)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(shami_class);
    }

    device_create(shami_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    printk(KERN_INFO "[Shami] Driver initialized at /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit shami_exit(void) {
    device_destroy(shami_class, MKDEV(major, 0));
    class_destroy(shami_class);
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "[Shami] Driver unloaded.\n");
}

module_init(shami_init);
module_exit(shami_exit);
MODULE_LICENSE("GPL");
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

// 引入刚才写好的头文件
#include "comm.h"

#define DEVICE_NAME "shami"

// --- 辅助函数：GUP 强力读取 ---
static int read_memory_force(struct mm_struct *mm, unsigned long addr, void *buffer, size_t size) {
    struct page *page;
    void *maddr;
    int res;
    size_t bytes_read = 0;
    
    while (bytes_read < size) {
        size_t offset = (addr + bytes_read) & ~PAGE_MASK;
        size_t bytes_to_copy = min(size - bytes_read, PAGE_SIZE - offset);

        // FOLL_FORCE: 强制读取
        res = get_user_pages_remote(mm, addr + bytes_read, 1, FOLL_FORCE, &page, NULL, NULL);
        
        if (res <= 0) {
            // 读取失败通常是地址无效或被保护
            return -1;
        }

        maddr = kmap_atomic(page);
        memcpy(buffer + bytes_read, maddr + offset, bytes_to_copy);
        kunmap_atomic(maddr);
        
        put_page(page);
        bytes_read += bytes_to_copy;
    }
    return 0;
}

// --- 辅助函数：GUP 强力写入 ---
static int write_memory_force(struct mm_struct *mm, unsigned long addr, void *data, size_t size) {
    struct page *page;
    void *maddr;
    int res;
    size_t bytes_written = 0;

    while (bytes_written < size) {
        size_t offset = (addr + bytes_written) & ~PAGE_MASK;
        size_t bytes_to_copy = min(size - bytes_written, PAGE_SIZE - offset);

        // FOLL_WRITE | FOLL_FORCE: 强制写入
        res = get_user_pages_remote(mm, addr + bytes_written, 1, FOLL_WRITE | FOLL_FORCE, &page, NULL, NULL);
        if (res <= 0) {
             return -1;
        }

        maddr = kmap_atomic(page);
        memcpy(maddr + offset, data + bytes_written, bytes_to_copy);
        kunmap_atomic(maddr);
        
        set_page_dirty_lock(page);
        put_page(page);
        bytes_written += bytes_to_copy;
    }
    return 0;
}

// --- IOCTL 主处理函数 ---
static long shami_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct task_struct *task;
    struct pid *pid_struct;
    struct mm_struct *mm;
    long ret = -EINVAL;
    COPY_MEMORY cm;
    void *kbuf = NULL;

    // 仅在读写操作时拷贝用户数据
    if (cmd == OP_READ_MEM || cmd == OP_WRITE_MEM) {
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm))) return -EFAULT;
        
        // 分配内核缓冲区
        kbuf = kmalloc(cm.size, GFP_KERNEL);
        if (!kbuf) return -ENOMEM;
    }

    switch (cmd) {
        case OP_READ_MEM: {
            pid_struct = find_get_pid(cm.pid);
            if (pid_struct) {
                task = get_pid_task(pid_struct, PIDTYPE_PID);
                if (task) {
                    mm = get_task_mm(task);
                    if (mm) {
                        // 执行强力读取
                        if (read_memory_force(mm, cm.addr, kbuf, cm.size) == 0) {
                            // 拷贝回用户空间
                            if (copy_to_user(cm.buffer, kbuf, cm.size)) ret = -EFAULT;
                            else ret = 0;
                        }
                        mmput(mm);
                    }
                    put_task_struct(task);
                }
                put_pid(pid_struct);
            }
        } break;

        case OP_WRITE_MEM: {
            // 先把要写的数据从用户态拿过来
            if (copy_from_user(kbuf, cm.buffer, cm.size)) {
                kfree(kbuf); return -EFAULT;
            }
            pid_struct = find_get_pid(cm.pid);
            if (pid_struct) {
                task = get_pid_task(pid_struct, PIDTYPE_PID);
                if (task) {
                    mm = get_task_mm(task);
                    if (mm) {
                        // 执行强力写入
                        if (write_memory_force(mm, cm.addr, kbuf, cm.size) == 0) ret = 0;
                        mmput(mm);
                    }
                    put_task_struct(task);
                }
                put_pid(pid_struct);
            }
        } break;
        
        default:
            ret = 0; // 其他命令直接返回成功，防止报错
            break;
    }

    if (kbuf) kfree(kbuf);
    return ret;
}

// --- 驱动注册逻辑 ---
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = shami_ioctl,
    .compat_ioctl = shami_ioctl,
};

static int major;
static struct class *shami_class;

static int __init shami_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) return major;
    shami_class = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(shami_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    printk(KERN_INFO "[Shami] Driver Loaded.\n");
    return 0;
}

static void __exit shami_exit(void) {
    device_destroy(shami_class, MKDEV(major, 0));
    class_destroy(shami_class);
    unregister_chrdev(major, DEVICE_NAME);
}

module_init(shami_init);
module_exit(shami_exit);
MODULE_LICENSE("GPL");

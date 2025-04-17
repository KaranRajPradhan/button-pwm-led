#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/gpio.h>

#define DEVICE_NAME "project"
#define SYSFS_DIR   "project"

#define GPIO_LED1 2     // Using GPIO 2 for LED1
#define GPIO_LED2 17    // Using GPIO 17 for LED2
#define GPIO_LED3 27    // Popular default for general use
#define GPIO_BTN1 5
#define GPIO_BTN2 6

#define MAX_DUTY 100
#define MIN_DUTY 10

static struct kobject *project_kobj;

static int irq_btn1, irq_btn2;
static u64 delta_avg_us = 0;
static int press_count = 0;

static int duty1 = MIN_DUTY;
static int duty2 = 0;
static int duty3 = 0;

static uint32_t *taddr = NULL;
static uint32_t *gpio_base = NULL;
static u64 last_hw_time = 0;

static ssize_t speed_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", delta_avg_us);
}

static ssize_t duty1_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", duty1);
}

static ssize_t duty1_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    sscanf(buf, "%d", &duty1);
    return count;
}

static ssize_t duty2_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", duty2);
}

static ssize_t duty2_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    sscanf(buf, "%d", &duty2);
    return count;
}

static ssize_t duty3_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", duty3);
}

static ssize_t duty3_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    sscanf(buf, "%d", &duty3);
    return count;
}

static struct kobj_attribute speed_attr = __ATTR_RO(speed);
static struct kobj_attribute duty1_attr = __ATTR(duty1, 0660, duty1_show, duty1_store);
static struct kobj_attribute duty2_attr = __ATTR(duty2, 0660, duty2_show, duty2_store);
static struct kobj_attribute duty3_attr = __ATTR(duty3, 0660, duty3_show, duty3_store);

static irqreturn_t button_isr(int irq, void *dev_id)
{
    u64 now = (u64)readl(taddr + 1) | ((u64)readl(taddr + 2) << 32);

    if (press_count > 0 && last_hw_time) {
        u64 delta = now - last_hw_time;
        delta_avg_us = (delta_avg_us * (press_count - 1) + delta) / press_count;
    }
    last_hw_time = now;
    press_count++;

    return IRQ_HANDLED;
}

static int __init project_init(void)
{
    int ret;

    // Create sysfs entries
    project_kobj = kobject_create_and_add(SYSFS_DIR, kernel_kobj);
    if (!project_kobj)
        return -ENOMEM;

    ret  = sysfs_create_file(project_kobj, &speed_attr.attr);
    ret |= sysfs_create_file(project_kobj, &duty1_attr.attr);
    ret |= sysfs_create_file(project_kobj, &duty2_attr.attr);
    ret |= sysfs_create_file(project_kobj, &duty3_attr.attr);

    if (ret)
        return ret;

    // Map system timer
    taddr = ioremap(0xfe003000, 0x10);
    if (!taddr)
        return -ENOMEM;

    // Map GPIO base
    gpio_base = ioremap(0xfe200000, 0x100);
    if (!gpio_base)
        return -ENOMEM;

    // Configure LED1 = GPIO2 (GPFSEL0, bits 6-8)
    writel((1 << 6), gpio_base);

    // Configure LED2 = GPIO17 (GPFSEL1, bits 21-23)
    writel((1 << 21), gpio_base + 1);

    // Configure LED3 = GPIO27 (GPFSEL2, bits 21-23)
    writel((1 << 21), gpio_base + 2);

    // Request IRQs
    irq_btn1 = gpio_to_irq(GPIO_BTN1);
    irq_btn2 = gpio_to_irq(GPIO_BTN2);

    request_irq(irq_btn1, button_isr, IRQF_TRIGGER_RISING, "btn1", NULL);
    request_irq(irq_btn2, button_isr, IRQF_TRIGGER_RISING, "btn2", NULL);

    pr_info("project module loaded\n");
    return 0;
}

static void __exit project_exit(void)
{
    free_irq(irq_btn1, NULL);
    free_irq(irq_btn2, NULL);

    if (gpio_base)
        iounmap(gpio_base);

    if (taddr)
        iounmap(taddr);

    kobject_put(project_kobj);
    pr_info("project module unloaded\n");
}

module_init(project_init);
module_exit(project_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("LED PWM Speed Tracker Driver with Sysfs and HW Timer");

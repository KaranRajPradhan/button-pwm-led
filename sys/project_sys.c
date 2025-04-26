#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/gpio.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/types.h>

#define DEVICE_NAME "project_sys"
#define GPIO_BASE_ADDR 0xFE200000
#define GPSET_OFFSET 0x1C
#define GPCLR_OFFSET 0x28
#define GPLEV_OFFSET 0x34

#define BUF_LEN 124

#define GPIO_LED1 2
#define GPIO_LED2 17
#define GPIO_LED3 12
#define GPIO_BTN1 5
#define GPIO_BTN2 6

#define TIME_SEC    0
#define TIME_100    2000000 // 2.0 ms Tim Period
#define TIME_75     1500000
#define TIME_50     1000000
#define TIME_25     500000
#define TIME_0      0

#define MAX_PRESSES 100


static struct hrtimer led1_timer, led2_timer, led3_timer, btn_poll_timer;
static struct kobject *project_kobj;
static uint32_t *addr = NULL;

static int led1_duty = 0, led2_duty = 0, led3_duty = 0;
static int last_btn1 = 1, last_btn2 = 1;
static int last_button_pressed = 0;
static int speed = 0;

static ktime_t led1_on, led1_off, led2_on, led2_off, led3_on, led3_off;
static bool led1_state, led2_state, led3_state;
static char read_buf[BUF_LEN + 1];

typedef struct {
    uint32_t timestamp;
    int button_id;
} button_event_t;

static button_event_t press_events[MAX_PRESSES];
static int press_idx = 0;

static void init_led_gpios(void)
{
    addr = ioremap(GPIO_BASE_ADDR, 4*16);
    if (!addr)
        return;
    // GPFSEL0: GPIO2 output (bits 6-8 = 001)
    writel((1 << 6), addr);
    // GPFSEL1: GPIO17 output (bits 21-23 = 001)
    // GPFSEL1: GPIO12 output (bits 6-8 = 001)
    writel(((1 << 21)|(1 << 6)), addr+1);
}

static enum hrtimer_restart led1_cb(struct hrtimer *timer)
{
    led1_state = !led1_state;
    writel((1<<GPIO_LED1), led1_state ? (addr+7) : (addr+10));
    hrtimer_forward_now(&led1_timer, led1_state ? led1_on : led1_off);
    
    return HRTIMER_RESTART;
}

static enum hrtimer_restart led2_cb(struct hrtimer *timer)
{
    led2_state = !led2_state;
    writel((1<<GPIO_LED2), led2_state ? (addr+7) : (addr+10));
    hrtimer_forward_now(&led2_timer, led2_state ? led2_on : led2_off);

    return HRTIMER_RESTART;
}

static enum hrtimer_restart led3_cb(struct hrtimer *timer)
{
    led3_state = !led3_state;
    writel((1<<GPIO_LED3), led3_state ? (addr+7) : (addr+10));
    hrtimer_forward_now(&led3_timer, led3_state ? led3_on : led3_off);

    return HRTIMER_RESTART;
}

static void record_press(int button_id)
{
    uint32_t now_sec = (uint32_t)(ktime_get_real_seconds());

    if (press_idx < MAX_PRESSES) {
        press_events[press_idx].timestamp = now_sec;
        press_events[press_idx].button_id = button_id;
        press_idx++;
    } else {
        int i;
        for (i = 1; i < MAX_PRESSES; i++)
            press_events[i-1] = press_events[i];
        press_events[MAX_PRESSES-1].timestamp = now_sec;
        press_events[MAX_PRESSES-1].button_id = button_id;
    }
}

static void calculate_speed(void)
{
    uint32_t now_sec = (uint32_t)(ktime_get_real_seconds());
    int count = 0;
    int i;
    int last_button = 0;

    // Purge old events
    int start_idx = press_idx;
    for (i = 0; i < press_idx; i++) {
        if (now_sec - press_events[i].timestamp <= 10) {
            start_idx = i;
            break;
        }
    }

    if (start_idx != 0 && start_idx < press_idx) {
        int new_idx = 0;
        for (i = start_idx; i < press_idx; i++) {
            press_events[new_idx++] = press_events[i];
        }
        press_idx = new_idx;
    }
    else if (start_idx == press_idx)
        press_idx = 0;

    // Count valid alternating presses
    for (i = 0; i < press_idx; i++) {
        if (press_events[i].button_id != last_button) {
            count++;
            last_button = press_events[i].button_id;
        }
    }

    speed = count;
    sprintf(read_buf, "%d\n", speed);
}

static enum hrtimer_restart btn_poll_cb(struct hrtimer *timer)
{
    uint32_t gplev = readl(addr + (GPLEV_OFFSET / 4));
    int btn1 = (gplev >> GPIO_BTN1) & 1;
    int btn2 = (gplev >> GPIO_BTN2) & 1;

    if (last_btn1 == 1 && btn1 == 0) { // BTN1 falling edge
        pr_info("BTN1 pressed\n");
        if (last_button_pressed != 1) {  // Only if last was not BTN1
            record_press(1);
            last_button_pressed = 1;
        }
    }

    if (last_btn2 == 1 && btn2 == 0) { // BTN2 falling edge
        pr_info("BTN2 pressed\n");
        if (last_button_pressed != 2) {  // Only if last was not BTN2
            record_press(2);
            last_button_pressed = 2;
        }
    }

    last_btn1 = btn1;
    last_btn2 = btn2;

    hrtimer_forward_now(timer, ktime_set(0, 1000000)); // 1ms
    return HRTIMER_RESTART;
}

// --- Sysfs Attributes ---

static ssize_t speed_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    calculate_speed();
    pr_info("Speed read: %d\n", speed);
    return sprintf(buf, "%d\n", speed);
}

static struct kobj_attribute speed_attr = __ATTR(speed, 0660, speed_show, NULL);

static ssize_t led1_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int duty;
    ktime_t on, off;

    if (sscanf(buf, "%d", &duty) != 1) {
        pr_info("device_write: bad format '%s'\n", buf);
        return -EINVAL;
    }

    switch (duty) {
        case 0:   pr_info("got duty 0\n"); on = ktime_set(0, TIME_0);   off = ktime_set(0, TIME_100); break;
        case 25:  pr_info("got duty 25\n"); on = ktime_set(0, TIME_25);  off = ktime_set(0, TIME_75); break;
        case 50:  pr_info("got duty 50\n"); on = ktime_set(0, TIME_50);  off = ktime_set(0, TIME_50); break;
        case 75:  pr_info("got duty 75\n"); on = ktime_set(0, TIME_75);  off = ktime_set(0, TIME_25); break;
        case 100: pr_info("got duty 100\n"); on = ktime_set(0, TIME_100); off = ktime_set(0,   TIME_0); break;
        default:  pr_info("device_write: invalid duty '%d'; only supports 0, 25, 50, 75, 100!\n", duty); return -EINVAL;
    }

    hrtimer_cancel(&led1_timer);
    led1_state=false;
    hrtimer_start(&led1_timer, ktime_set(0,0), HRTIMER_MODE_REL);

    return count;
}

static struct kobj_attribute led1_attr = __ATTR(led1, 0660, NULL, led1_store);

static ssize_t led2_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    

    return count;
}

static struct kobj_attribute led2_attr = __ATTR(led2, 0660, NULL, led2_store);

static ssize_t led3_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    

    return count;
}

static struct kobj_attribute led3_attr = __ATTR(led3, 0660, NULL, led3_store);

static struct attribute *attrs[] = {
    &speed_attr.attr,
    &led1_attr.attr,
    &led2_attr.attr,
    &led3_attr.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

// --- Module Init/Exit ---

static int __init project_init(void)
{
    int retval;

    pr_info("project_sys: Module initialized\n");
    addr = ioremap(GPIO_BASE_ADDR, 4*16);

    project_kobj = kobject_create_and_add(DEVICE_NAME, kernel_kobj);
    if (!project_kobj)
        return -ENOMEM;

    retval = sysfs_create_group(project_kobj, &attr_group);
    if (retval)
        kobject_put(project_kobj);

    pr_info("Initing LED GPIOs...\n");
    init_led_gpios();

    pr_info("Initing LED timers...\n");
    hrtimer_init(&led1_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hrtimer_init(&led2_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hrtimer_init(&led3_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    led1_timer.function = &led1_cb;
    led2_timer.function = &led2_cb;
    led3_timer.function = &led3_cb;

    pr_info("Initing button poll timer...\n");
    hrtimer_init(&btn_poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    btn_poll_timer.function = btn_poll_cb;
    hrtimer_start(&btn_poll_timer, ktime_set(0, 1000000), HRTIMER_MODE_REL);

    return retval;
}

static void __exit project_exit(void)
{
    hrtimer_cancel(&led1_timer);
    hrtimer_cancel(&led2_timer);
    hrtimer_cancel(&led3_timer);
    hrtimer_cancel(&btn_poll_timer);

    iounmap(addr);
    kobject_put(project_kobj);
    pr_info("project_sys: Module exited\n");
}

module_init(project_init);
module_exit(project_exit);

MODULE_LICENSE("GPL");

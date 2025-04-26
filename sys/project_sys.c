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

#define GPIO_LED1 2
#define GPIO_LED2 17
#define GPIO_LED3 12
#define GPIO_BTN1 5
#define GPIO_BTN2 6

#define TIME_25 500000
#define TIME_50 1000000
#define TIME_75 1500000
#define TIME_100 2000000

#define MAX_PRESSES 100

MODULE_LICENSE("GPL");

static struct hrtimer led1_timer, led2_timer, led3_timer, btn_poll_timer;
static struct kobject *project_kobj;
static uint32_t *addr = NULL;

static int led1_duty = 0, led2_duty = 0, led3_duty = 0;
static int last_btn1 = 1, last_btn2 = 1;
static int last_button_pressed = 0;
static int speed = 0;

typedef struct {
    uint32_t timestamp;
    int button_id;
} button_event_t;

static button_event_t press_events[MAX_PRESSES];
static int press_idx = 0;

static void set_led(int led_gpio, bool state)
{
    if (state)
        writel(1 << led_gpio, addr + (GPSET_OFFSET / 4));
    else
        writel(1 << led_gpio, addr + (GPCLR_OFFSET / 4));
}

static enum hrtimer_restart led_timer_cb(struct hrtimer *timer)
{
    if (timer == &led1_timer) {
        static bool on1 = false;
        on1 = !on1;
        set_led(GPIO_LED1, on1);
        hrtimer_forward_now(timer, ktime_set(0, led1_duty * 1000));
    } else if (timer == &led2_timer) {
        static bool on2 = false;
        on2 = !on2;
        set_led(GPIO_LED2, on2);
        hrtimer_forward_now(timer, ktime_set(0, led2_duty * 1000));
    } else if (timer == &led3_timer) {
        static bool on3 = false;
        on3 = !on3;
        set_led(GPIO_LED3, on3);
        hrtimer_forward_now(timer, ktime_set(0, led3_duty * 1000));
    }
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

    int start_idx = press_idx;
    for (i = 0; i < press_idx; i++) {
        if (now_sec - press_events[i].timestamp <= 10) {
            start_idx = i;
            break;
        }
    }

    if (start_idx != 0 && start_idx < press_idx) {
        int new_idx = 0;
        for (i = start_idx; i < press_idx; i++)
            press_events[new_idx++] = press_events[i];
        press_idx = new_idx;
    } else if (start_idx == press_idx) {
        press_idx = 0;
    }

    for (i = 0; i < press_idx; i++) {
        if (press_events[i].button_id != last_button) {
            count++;
            last_button = press_events[i].button_id;
        }
    }

    speed = count;
}

static enum hrtimer_restart btn_poll_cb(struct hrtimer *timer)
{
    uint32_t gplev = readl(addr + (GPLEV_OFFSET / 4));
    int btn1 = (gplev >> GPIO_BTN1) & 1;
    int btn2 = (gplev >> GPIO_BTN2) & 1;

    if (last_btn1 == 1 && btn1 == 0) {
        if (last_button_pressed != 1) {
            record_press(1);
            last_button_pressed = 1;
        }
    }

    if (last_btn2 == 1 && btn2 == 0) {
        if (last_button_pressed != 2) {
            record_press(2);
            last_button_pressed = 2;
        }
    }

    last_btn1 = btn1;
    last_btn2 = btn2;

    hrtimer_forward_now(timer, ktime_set(0, 1000000));
    return HRTIMER_RESTART;
}

// --- Sysfs Attributes ---

static ssize_t speed_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    calculate_speed();
    return sprintf(buf, "%d\n", speed);
}

static struct kobj_attribute speed_attr = __ATTR(speed, 0444, speed_show, NULL);

static ssize_t led1_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    sscanf(buf, "%d", &led1_duty);
    return count;
}

static struct kobj_attribute led1_attr = __ATTR(led1, 0220, NULL, led1_store);

static ssize_t led2_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    sscanf(buf, "%d", &led2_duty);
    return count;
}

static struct kobj_attribute led2_attr = __ATTR(led2, 0220, NULL, led2_store);

static ssize_t led3_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    sscanf(buf, "%d", &led3_duty);
    return count;
}

static struct kobj_attribute led3_attr = __ATTR(led3, 0220, NULL, led3_store);

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

    addr = ioremap(GPIO_BASE_ADDR, 0x100);

    project_kobj = kobject_create_and_add(DEVICE_NAME, kernel_kobj);
    if (!project_kobj)
        return -ENOMEM;

    retval = sysfs_create_group(project_kobj, &attr_group);
    if (retval)
        kobject_put(project_kobj);

    hrtimer_init(&led1_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hrtimer_init(&led2_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hrtimer_init(&led3_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hrtimer_init(&btn_poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

    led1_timer.function = led_timer_cb;
    led2_timer.function = led_timer_cb;
    led3_timer.function = led_timer_cb;
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
}

module_init(project_init);
module_exit(project_exit);

#include <linux/atomic.h> 
#include <linux/cdev.h> 
#include <linux/delay.h> 
#include <linux/device.h> 
#include <linux/fs.h> 
#include <linux/init.h> 
#include <linux/kernel.h>
#include <linux/module.h> 
#include <linux/printk.h> 
#include <linux/types.h> 
#include <linux/uaccess.h>
#include <linux/version.h> 
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/err.h>

#define DEVICE_NAME "project_dev"
#define SUCCESS 0
#define GPIO_BASE_ADDR  0xFE200000
#define GPSET_OFFSET    0x1C
#define GPCLR_OFFSET    0x28
#define GPLEV_OFFSET	0x34

#define BUF_LEN 124

#define TIME_SEC    0
#define TIME_100    2000000 // 2.0 ms Tim Period
#define TIME_75     1500000
#define TIME_50     1000000
#define TIME_25     500000
#define TIME_0      0

#define GPIO_LED1   2
#define GPIO_LED2   17
#define GPIO_LED3   12
#define GPIO_BTN1   5
#define GPIO_BTN2   6

#define MAX_PRESSES 100

static struct hrtimer led1_timer, led2_timer, led3_timer;
static struct hrtimer btn_poll_timer;
 
static int device_open(struct inode *, struct file *); 
static int device_release(struct inode *, struct file *); 
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *); 
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *); 

static struct class *cls; 
static int major;
static uint32_t* addr = NULL;

static struct file_operations chardev_fops = { 
    .read = device_read, 
    .write = device_write, 
    .open = device_open, 
    .release = device_release, 
};

enum {
    CDEV_NOT_USED,
    CDEV_EXCLUSIVE_OPEN,
};

static atomic_t already_open = ATOMIC_INIT(CDEV_NOT_USED);

static int last_btn1 = 1;
static int last_btn2 = 1;
static int last_button_pressed = 0;  // 0 = none, 1 = BTN1, 2 = BTN2
static int speed = 0;                // Number of valid alternations in last 10s
typedef struct {
    uint32_t timestamp;
    int button_id;
} button_event_t;
static button_event_t press_events[MAX_PRESSES];
static int press_idx = 0;

static ktime_t led1_on, led1_off, led2_on, led2_off, led3_on, led3_off;
static bool led1_state, led2_state, led3_state;
static char read_buf[BUF_LEN + 1];

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
    //pr_info("in led1_cb: led1_state=%d\n", led1_state);
    void __iomem *set = addr + GPSET_OFFSET;
    void __iomem *clr = addr + GPCLR_OFFSET;

    led1_state = !led1_state;
    writel((1<<GPIO_LED1), led1_state ? (addr+7) : (addr+10));
    hrtimer_forward_now(&led1_timer, led1_state ? led1_on : led1_off);
    
    return HRTIMER_RESTART;
}

static enum hrtimer_restart led2_cb(struct hrtimer *timer)
{
    void __iomem *set = addr + GPSET_OFFSET;
    void __iomem *clr = addr + GPCLR_OFFSET;

    led2_state = !led2_state;
    writel((1<<GPIO_LED2), led2_state ? (addr+7) : (addr+10));
    hrtimer_forward_now(&led2_timer, led2_state ? led2_on : led2_off);

    return HRTIMER_RESTART;
}

static enum hrtimer_restart led3_cb(struct hrtimer *timer)
{
    void __iomem *set = addr + GPSET_OFFSET;
    void __iomem *clr = addr + GPCLR_OFFSET;
    
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

static int __init chardev_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &chardev_fops);

    if (major < 0) {
        pr_alert("Registering char device failed with %d\n", major);
        return major;
    }

    pr_info("Major number assigned: %d\n", major);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cls = class_create(DEVICE_NAME);
#else
    cls = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

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
    hrtimer_start(&btn_poll_timer, ktime_set(0, 1000000), HRTIMER_MODE_REL); // 1ms

    pr_info("Device created on /dev/%s\n", DEVICE_NAME);

    return SUCCESS;
}

static void __exit chardev_exit(void) 
{
    hrtimer_cancel(&led1_timer);
    hrtimer_cancel(&led2_timer);
    hrtimer_cancel(&led3_timer);
    hrtimer_cancel(&btn_poll_timer);

    device_destroy(cls, MKDEV(major, 0)); 
    class_destroy(cls); 
 
    unregister_chrdev(major, DEVICE_NAME); 
}

static int device_open(struct inode *inode, struct file *file)
{
    if (atomic_cmpxchg(&already_open, CDEV_NOT_USED, CDEV_EXCLUSIVE_OPEN))
        return -EBUSY;

    pr_info("Button speed: %d\n", speed);
    // sprintf(read_buf, "%d\n", speed);
    calculate_speed();

    try_module_get(THIS_MODULE);

    return SUCCESS;
}

static int device_release(struct inode *inode, struct file *file) 
{ 
    atomic_set(&already_open, CDEV_NOT_USED); 
 
    module_put(THIS_MODULE); 
 
    return SUCCESS; 
}

static ssize_t device_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset)
{
    int bytes_read = 0;
    calculate_speed();
    // sprintf(read_buf, "%d\n", speed);
    const char *msg_ptr = read_buf;

    if (!*(msg_ptr + *offset)) {
        *offset = 0;
        return 0;
    }

    msg_ptr += *offset;

    while (length && *msg_ptr) {
        put_user(*(msg_ptr++), buffer++);
        length--;
        bytes_read++;
    }

    *offset += bytes_read;

    return bytes_read;
}

// write: "<led> <duty_percent>"
static ssize_t device_write(struct file *filp, const char __user *buffer, size_t length, loff_t *offset)
{
    char write_buf[BUF_LEN + 1] = {0};
    int led, duty;
    int i;
    ktime_t on, off;

    pr_info("device_write(%p, %p, %zu)\n", filp, buffer, length);

    for (i = 0; i < length; i++) {
        if (get_user(write_buf[i], buffer + i)) {
            pr_info("Error getting user-space message!\n");
            return -EFAULT;
        }
    }
    write_buf[length] = '\0';
    pr_info("A\n");
    if (sscanf(write_buf, "%d %d", &led, &duty) != 2) {
        pr_info("device_write: bad format '%s'\n", write_buf);
        return -EINVAL;
    }

    // Select on and off time based on duty
    switch (duty) {
        case 0:   pr_info("got duty 0\n"); on = ktime_set(0, TIME_0);   off = ktime_set(0, TIME_100); break;
        case 25:  pr_info("got duty 25\n"); on = ktime_set(0, TIME_25);  off = ktime_set(0, TIME_75); break;
        case 50:  pr_info("got duty 50\n"); on = ktime_set(0, TIME_50);  off = ktime_set(0, TIME_50); break;
        case 75:  pr_info("got duty 75\n"); on = ktime_set(0, TIME_75);  off = ktime_set(0, TIME_25); break;
        case 100: pr_info("got duty 100\n"); on = ktime_set(0, TIME_100); off = ktime_set(0,   TIME_0); break;
        default:  pr_info("device_write: invalid duty '%d'; only supports 0, 25, 50, 75, 100!\n", duty); return -EINVAL;
    }
    pr_info("B\n");
    switch (led) {
    case 1:  pr_info("led1\n"); led1_on=on; led1_off=off; hrtimer_cancel(&led1_timer); led1_state=false; hrtimer_start(&led1_timer, ktime_set(0,0), HRTIMER_MODE_REL); break;
    case 2:  pr_info("led2\n"); led2_on=on; led2_off=off; hrtimer_cancel(&led2_timer); led2_state=false; hrtimer_start(&led2_timer, ktime_set(0,0), HRTIMER_MODE_REL); break;
    case 3:  pr_info("led3\n"); led3_on=on; led3_off=off; hrtimer_cancel(&led3_timer); led3_state=false; hrtimer_start(&led3_timer, ktime_set(0,0), HRTIMER_MODE_REL); break;
    default: pr_info("device_write: invalid LED number '%d'\n", led); return -EINVAL;
    }
    pr_info("C\n");

    return length;
}


//BOTTOM
module_init(chardev_init); 
module_exit(chardev_exit); 
 
MODULE_LICENSE("GPL");

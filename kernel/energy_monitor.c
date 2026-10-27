#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define DEVICE_NAME "energy_monitor"
#define CLASS_NAME  "energy_monitor"
#define BUFFER_SIZE 160
#define UPDATE_INTERVAL_MS 1000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anbu");
MODULE_DESCRIPTION("Linux Smart Energy Monitoring Driver");
MODULE_VERSION("3.0");

static dev_t device_number;
static struct cdev energy_cdev;
static struct class *energy_class;
static struct device *energy_device;

static DEFINE_MUTEX(energy_mutex);
static struct delayed_work energy_work;
static DECLARE_WAIT_QUEUE_HEAD(energy_wait_queue);

static char energy_data[BUFFER_SIZE];

static unsigned int sample_count;
static unsigned int voltage_v;
static unsigned int current_ma;
static unsigned int power_mw;
static u64 energy_mwh;
static bool shutting_down;

struct energy_file_state {
    unsigned int last_seen_sample;
};

/*
 * Generate simulated measurements.
 *
 * These values are NOT real electrical measurements.
 * A real implementation would obtain readings from a
 * supported energy-monitoring sensor.
 */
static void update_energy_data(struct work_struct *work)
{
    unsigned int variation;

    mutex_lock(&energy_mutex);

    if (shutting_down) {
        mutex_unlock(&energy_mutex);
        return;
    }

    sample_count++;
    variation = sample_count % 10;

    voltage_v = 228 + (sample_count % 6);
    current_ma = 1200 + variation * 100;

    /* V * mA = mW */
    power_mw = voltage_v * current_ma;

    /*
     * Approximate energy accumulation.
     * One update represents approximately one second.
     *
     * mW * seconds / 3600 = mWh
     */
    energy_mwh += power_mw / 3600;

    scnprintf(
        energy_data,
        sizeof(energy_data),
        "sample=%u,voltage=%u,current=%u.%03u,"
        "power=%u.%03u,energy=%llu.%03llu\n",
        sample_count,
        voltage_v,
        current_ma / 1000,
        current_ma % 1000,
        power_mw / 1000,
        power_mw % 1000,
        (unsigned long long)(energy_mwh / 1000),
        (unsigned long long)(energy_mwh % 1000)
    );

    mutex_unlock(&energy_mutex);

    /* Wake poll/select/epoll waiters after publishing a complete sample. */
    wake_up_interruptible_poll(
        &energy_wait_queue,
        EPOLLIN | EPOLLRDNORM
    );

    /* Schedule the next update. */
    mutex_lock(&energy_mutex);
    if (!shutting_down)
        schedule_delayed_work(
            &energy_work,
            msecs_to_jiffies(UPDATE_INTERVAL_MS)
        );
    mutex_unlock(&energy_mutex);
}

static int energy_open(struct inode *inode, struct file *file)
{
    struct energy_file_state *state;

    state = kmalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;

    mutex_lock(&energy_mutex);
    state->last_seen_sample = sample_count - 1;
    mutex_unlock(&energy_mutex);

    file->private_data = state;

    pr_info("energy_monitor: Device opened\n");
    return 0;
}

static int energy_release(struct inode *inode, struct file *file)
{
    kfree(file->private_data);
    file->private_data = NULL;

    pr_info("energy_monitor: Device closed\n");
    return 0;
}

static ssize_t energy_read(
    struct file *file,
    char __user *user_buffer,
    size_t count,
    loff_t *offset)
{
    struct energy_file_state *state = file->private_data;
    loff_t initial_offset = *offset;
    ssize_t result;

    if (mutex_lock_interruptible(&energy_mutex))
        return -ERESTARTSYS;

    if (shutting_down) {
        mutex_unlock(&energy_mutex);
        return -ENODEV;
    }

    result = simple_read_from_buffer(
        user_buffer,
        count,
        offset,
        energy_data,
        strlen(energy_data)
    );

    if (result > 0 && initial_offset == 0)
        state->last_seen_sample = sample_count;

    mutex_unlock(&energy_mutex);

    return result;
}

static __poll_t energy_poll(struct file *file, poll_table *wait)
{
    struct energy_file_state *state = file->private_data;
    __poll_t mask = 0;

    poll_wait(file, &energy_wait_queue, wait);

    mutex_lock(&energy_mutex);
    if (shutting_down)
        mask = EPOLLHUP | EPOLLERR;
    else if (sample_count != state->last_seen_sample)
        mask = EPOLLIN | EPOLLRDNORM;
    mutex_unlock(&energy_mutex);

    return mask;
}

static const struct file_operations energy_fops = {
    .owner   = THIS_MODULE,
    .open    = energy_open,
    .release = energy_release,
    .read    = energy_read,
    .poll    = energy_poll,
    .llseek  = default_llseek,
};

static int __init energy_monitor_init(void)
{
    int result;

    mutex_lock(&energy_mutex);

    sample_count = 0;
    voltage_v = 230;
    current_ma = 1500;
    power_mw = voltage_v * current_ma;
    energy_mwh = 0;
    shutting_down = false;

    scnprintf(
        energy_data,
        sizeof(energy_data),
        "sample=0,voltage=230,current=1.500,"
        "power=345.000,energy=0.000\n"
    );

    mutex_unlock(&energy_mutex);

    result = alloc_chrdev_region(
        &device_number, 0, 1, DEVICE_NAME
    );

    if (result < 0)
        return result;

    cdev_init(&energy_cdev, &energy_fops);
    energy_cdev.owner = THIS_MODULE;

    result = cdev_add(&energy_cdev, device_number, 1);

    if (result < 0)
        goto error_unregister;

    energy_class = class_create(CLASS_NAME);

    if (IS_ERR(energy_class)) {
        result = PTR_ERR(energy_class);
        goto error_cdev;
    }

    energy_device = device_create(
        energy_class,
        NULL,
        device_number,
        NULL,
        DEVICE_NAME
    );

    if (IS_ERR(energy_device)) {
        result = PTR_ERR(energy_device);
        goto error_class;
    }

    INIT_DELAYED_WORK(&energy_work, update_energy_data);

    schedule_delayed_work(
        &energy_work,
        msecs_to_jiffies(UPDATE_INTERVAL_MS)
    );

    pr_info(
        "energy_monitor: Initialized; major=%d minor=%d\n",
        MAJOR(device_number),
        MINOR(device_number)
    );

    return 0;

error_class:
    class_destroy(energy_class);

error_cdev:
    cdev_del(&energy_cdev);

error_unregister:
    unregister_chrdev_region(device_number, 1);
    return result;
}

static void __exit energy_monitor_exit(void)
{
    /* Prevent rescheduling, then stop work before removing resources. */
    mutex_lock(&energy_mutex);
    shutting_down = true;
    mutex_unlock(&energy_mutex);

    cancel_delayed_work_sync(&energy_work);
    wake_up_interruptible_poll(
        &energy_wait_queue,
        EPOLLHUP | EPOLLERR
    );

    device_destroy(energy_class, device_number);
    class_destroy(energy_class);
    cdev_del(&energy_cdev);
    unregister_chrdev_region(device_number, 1);

    pr_info("energy_monitor: Driver unloaded safely\n");
}

module_init(energy_monitor_init);
module_exit(energy_monitor_exit);

#include <linux/module.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#define DS1302_ADDR_SECONDS  0x80
#define DS1302_ADDR_MINUTES  0x82
#define DS1302_ADDR_HOURS    0x84
#define DS1302_ADDR_DATE     0x86
#define DS1302_ADDR_MONTH    0x88
#define DS1302_ADDR_DOW      0x8A
#define DS1302_ADDR_YEAR     0x8C

// ioctl
#define DS1302_IOC_MAGIC  'd'
#define DS1302_IOC_GET    _IOR(DS1302_IOC_MAGIC, 0, struct ds1302_time)
#define DS1302_IOC_SET    _IOW(DS1302_IOC_MAGIC, 1, struct ds1302_time)

struct ds1302_time {
    unsigned char year;   // 0~99 (2000+year)
    unsigned char month;  // 1~12
    unsigned char date;   // 1~31
    unsigned char dow;    // 0~6 or 1~7 
    unsigned char hour;   // 0~23
    unsigned char min;    // 0~59
    unsigned char sec;    // 0~59
};

// GPIO BCM 번호 
static int gpio_ce  = 17;
static int gpio_clk = 27;
static int gpio_io  = 22;
module_param(gpio_ce,  int, 0444);
module_param(gpio_clk, int, 0444);
module_param(gpio_io,  int, 0444);

static DEFINE_MUTEX(ds_lock);

static inline unsigned char bcd2dec(unsigned char b)
{
    return ((b >> 4) * 10) + (b & 0x0F);
}
static inline unsigned char dec2bcd(unsigned char d)
{
    return ((d / 10) << 4) | (d % 10);
}

static inline void ce_high(void) { gpio_set_value(gpio_ce, 1); }
static inline void ce_low(void)  { gpio_set_value(gpio_ce, 0); }
static inline void clk_high(void){ gpio_set_value(gpio_clk, 1); }
static inline void clk_low(void) { gpio_set_value(gpio_clk, 0); }

static inline void clk_pulse(void)
{
    clk_high();
    ndelay(200);
    clk_low();
    ndelay(200);
}

static void io_dir_out(int val)
{
    gpio_direction_output(gpio_io, val);
}
static void io_dir_in(void)
{
    gpio_direction_input(gpio_io);
}

static void ds1302_tx_byte(unsigned char tx)
{
    int i;
    io_dir_out(0);

    // LSB first
    for (i = 0; i < 8; i++) {
        gpio_set_value(gpio_io, !!(tx & (1U << i)));
        clk_pulse();
    }
}

static unsigned char ds1302_rx_byte(void)
{
    int i;
    unsigned char temp = 0;

    io_dir_in();

    for (i = 0; i < 8; i++) {
        if (gpio_get_value(gpio_io))
            temp |= (1U << i);
        if (i != 7)
            clk_pulse();
    }
    return temp;
}

static void ds1302_begin(void)
{
    ce_high();
    ndelay(200);
}

static void ds1302_end(void)
{
    ndelay(200);
    ce_low();
    ndelay(200);
}

static void ds1302_write_reg(unsigned char addr, unsigned char data_dec)
{
    ds1302_begin();
    ds1302_tx_byte(addr);                // write addr
    ds1302_tx_byte(dec2bcd(data_dec));   // data
    ds1302_end();
}

static unsigned char ds1302_read_reg(unsigned char addr)
{
    unsigned char raw;
    ds1302_begin();
    ds1302_tx_byte(addr + 1);  // read = write+1
    raw = ds1302_rx_byte();
    ds1302_end();
    return bcd2dec(raw);
}

static void ds1302_get_time(struct ds1302_time *t)
{
    t->sec   = ds1302_read_reg(DS1302_ADDR_SECONDS);
    t->min   = ds1302_read_reg(DS1302_ADDR_MINUTES);
    t->hour  = ds1302_read_reg(DS1302_ADDR_HOURS);
    t->date  = ds1302_read_reg(DS1302_ADDR_DATE);
    t->month = ds1302_read_reg(DS1302_ADDR_MONTH);
    t->dow   = ds1302_read_reg(DS1302_ADDR_DOW);
    t->year  = ds1302_read_reg(DS1302_ADDR_YEAR);
}

static void ds1302_set_time(const struct ds1302_time *t)
{
    ds1302_write_reg(DS1302_ADDR_SECONDS, t->sec);
    ds1302_write_reg(DS1302_ADDR_MINUTES, t->min);
    ds1302_write_reg(DS1302_ADDR_HOURS,   t->hour);
    ds1302_write_reg(DS1302_ADDR_DATE,    t->date);
    ds1302_write_reg(DS1302_ADDR_MONTH,   t->month);
    ds1302_write_reg(DS1302_ADDR_DOW,     t->dow);
    ds1302_write_reg(DS1302_ADDR_YEAR,    t->year);
}

static long ds1302_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct ds1302_time t;

    if (_IOC_TYPE(cmd) != DS1302_IOC_MAGIC)
        return -ENOTTY;

    mutex_lock(&ds_lock);

    switch (cmd) {
    case DS1302_IOC_GET:
        ds1302_get_time(&t);
        mutex_unlock(&ds_lock);
        if (copy_to_user((void __user *)arg, &t, sizeof(t)))
            return -EFAULT;
        return 0;

    case DS1302_IOC_SET:
        if (copy_from_user(&t, (void __user *)arg, sizeof(t))) {
            mutex_unlock(&ds_lock);
            return -EFAULT;
        }
        ds1302_set_time(&t);
        mutex_unlock(&ds_lock);
        return 0;

    default:
        mutex_unlock(&ds_lock);
        return -ENOTTY;
    }
}

static const struct file_operations ds1302_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = ds1302_ioctl,
    .compat_ioctl   = ds1302_ioctl,
};

static struct miscdevice ds1302_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "ds1302",
    .fops  = &ds1302_fops,
    .mode  = 0666,
};

static int __init ds1302_init(void)
{
    int ret;

    // GPIO 요청
    ret = gpio_request(gpio_ce, "ds1302_ce");
    if (ret) return ret;
    ret = gpio_request(gpio_clk, "ds1302_clk");
    if (ret) { gpio_free(gpio_ce); return ret; }
    ret = gpio_request(gpio_io, "ds1302_io");
    if (ret) { gpio_free(gpio_ce); gpio_free(gpio_clk); return ret; }

    gpio_direction_output(gpio_ce, 0);
    gpio_direction_output(gpio_clk, 0);
    io_dir_out(0);

    ret = misc_register(&ds1302_misc);
    if (ret) {
        gpio_free(gpio_ce);
        gpio_free(gpio_clk);
        gpio_free(gpio_io);
        return ret;
    }

    pr_info("ds1302: loaded (ce=%d clk=%d io=%d)\n", gpio_ce, gpio_clk, gpio_io);
    return 0;
}

static void __exit ds1302_exit(void)
{
    misc_deregister(&ds1302_misc);
    gpio_free(gpio_ce);
    gpio_free(gpio_clk);
    gpio_free(gpio_io);
    pr_info("ds1302: unloaded\n");
}

module_init(ds1302_init);
module_exit(ds1302_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("DS1302 GPIO bitbang driver (/dev/ds1302)");

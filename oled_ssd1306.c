#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/string.h> 

#define SSD1306_ADDR 0x3C
#define OLED_IOC_MAGIC 'o'

typedef struct {
    __u8 x;       // 0~127
    __u8 page;    // 0~7
} oled_pos_t;

#define OLED_CLEAR  _IO(OLED_IOC_MAGIC, 0)
#define OLED_SETPOS _IOW(OLED_IOC_MAGIC, 1, oled_pos_t)

static const unsigned char font5x7[128][5] = {
    [0x20]={0},
    ['0']={0x3E,0x51,0x49,0x45,0x3E}, ['1']={0,0x42,0x7F,0x40,0},
    ['2']={0x42,0x61,0x51,0x49,0x46}, ['3']={0x21,0x41,0x45,0x4B,0x31},
    ['4']={0x18,0x14,0x12,0x7F,0x10}, ['5']={0x27,0x45,0x45,0x45,0x39},
    ['6']={0x3C,0x4A,0x49,0x49,0x30}, ['7']={0x01,0x71,0x09,0x05,0x03},
    ['8']={0x36,0x49,0x49,0x49,0x36}, ['9']={0x06,0x49,0x49,0x29,0x1E},
    [':']={0,0x36,0x36,0,0}, ['>']={0,0x41,0x3E,0x1C,0},
    ['A']={0x7E,0x09,0x09,0x09,0x7E}, ['B']={0x7F,0x49,0x49,0x49,0x36},
    ['C']={0x3E,0x41,0x41,0x41,0x22}, ['D']={0x7F,0x41,0x41,0x22,0x1C},
    ['E']={0x7F,0x49,0x49,0x49,0},    ['F']={0x7F,0x09,0x09,0x01,0},
    ['G']={0x3E,0x41,0x49,0x49,0x7A}, ['H']={0x7F,0x08,0x08,0x08,0x7F},
    ['I']={0,0x41,0x7F,0x41,0},       ['K']={0x7F,0x08,0x14,0x22,0x41},
    ['L']={0x7F,0x40,0x40,0x40,0},    ['M']={0x7F,0x02,0x04,0x02,0x7F},
    ['N']={0x7F,0x02,0x04,0x08,0x7F}, ['O']={0x3E,0x41,0x41,0x41,0x3E},
    ['P']={0x7F,0x09,0x09,0x09,0x06}, ['R']={0x7F,0x09,0x19,0x29,0x46},
    ['S']={0x46,0x49,0x49,0x49,0x31}, ['T']={0x01,0x01,0x7F,0x01,0x01},
    ['U']={0x3F,0x40,0x40,0x40,0x3F}, ['V']={0x1F,0x20,0x40,0x20,0x1F},
    ['Y']={0x01,0x02,0x7C,0x02,0x01}, ['W']={0x3F,0x40,0x38,0x40,0x3F},
    ['*']={0x14,0x08,0x3E,0x08,0x14}, ['!']={0,0,0x5F,0,0}, ['-']={8,8,8,8,8},
    ['[']={0x7F,0x41,0x41,0,0},       [']']={0,0,0x41,0x41,0x7F}
};

struct oled_dev {
    struct i2c_client *client;
    struct mutex lock;
    u8 x;
    u8 page;
};

static struct oled_dev g_oled;

// control byte: 0x00=cmd, 0x40=data
static int oled_send_cmd(u8 cmd)
{
    u8 buf[2] = {0x00, cmd};
    return i2c_master_send(g_oled.client, buf, 2);
}

static void oled_set_pos(u8 x, u8 page)
{
    if (page > 7) page = 7;
    if (x > 127) x = 127;

    g_oled.x = x;
    g_oled.page = page;

    oled_send_cmd(0xB0 | page);
    oled_send_cmd(0x00 | (x & 0x0F));
    oled_send_cmd(0x10 | (x >> 4));
}

// 화면 전체 지우기 (Bulk Transfer)
static void oled_clear(void)
{
    int p;
    u8 buf[129]; // Control(1) + Data(128)

    buf[0] = 0x40;
    memset(&buf[1], 0, 128); // 나머지는 0(Black)으로 채움

    for (p = 0; p < 8; p++) {
        oled_set_pos(0, p);
        // 한 페이지(128픽셀)를 한 번의 전송으로 처리
        i2c_master_send(g_oled.client, buf, 129);
    }
    oled_set_pos(0, 0);
}

static void oled_puts(const char *s, size_t n)
{
    // 화면 가로 128픽셀 / 글자폭 6 = 약 21자
    // 버퍼 크기: Control(1) + 21자 * 6바이트 = 127바이트 (넉넉하게 140 잡음)
    u8 buf[140]; 
    int i, j;
    int buf_idx = 1; // 0번은 Control Byte

    buf[0] = 0x40; // Data Mode

    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\0') break;
        if ((unsigned char)c < 32 || (unsigned char)c > 127) c = ' ';

        // 버퍼가 가득 차면(혹시 모를 긴 문자열) 먼저 전송
        if (buf_idx + 6 > sizeof(buf)) {
            i2c_master_send(g_oled.client, buf, buf_idx);
            buf_idx = 1; // 리셋
        }

        const unsigned char *g = font5x7[(int)c];

        // 폰트 비트맵 복사
        for (j = 0; j < 5; j++) {
            buf[buf_idx++] = g[j];
        }
        buf[buf_idx++] = 0x00; // 글자 사이 공백 1픽셀
    }

    // 버퍼에 남은 데이터가 있으면 전송
    if (buf_idx > 1) {
        i2c_master_send(g_oled.client, buf, buf_idx);
    }
}

static long oled_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    oled_pos_t pos;

    mutex_lock(&g_oled.lock);

    switch (cmd) {
    case OLED_CLEAR:
        oled_clear();
        break;

    case OLED_SETPOS:
        if (copy_from_user(&pos, (void __user *)arg, sizeof(pos))) {
            mutex_unlock(&g_oled.lock);
            return -EFAULT;
        }
        oled_set_pos(pos.x, pos.page);
        break;

    default:
        mutex_unlock(&g_oled.lock);
        return -ENOTTY;
    }

    mutex_unlock(&g_oled.lock);
    return 0;
}

static ssize_t oled_write(struct file *file, const char __user *ubuf,
                          size_t len, loff_t *off)
{
    char kbuf[128];
    size_t n = len;

    if (n == 0) return 0;
    if (n > sizeof(kbuf)) n = sizeof(kbuf) - 1; // Null safety

    if (copy_from_user(kbuf, ubuf, n))
        return -EFAULT;
    
    kbuf[n] = '\0'; // 문자열 끝 보장

    mutex_lock(&g_oled.lock);
    oled_puts(kbuf, n);
    mutex_unlock(&g_oled.lock);

    return len;
}

static const struct file_operations oled_fops = {
    .owner          = THIS_MODULE,
    .write          = oled_write,
    .unlocked_ioctl = oled_ioctl,
};

static struct miscdevice oled_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "oled",
    .fops  = &oled_fops,
    .mode  = 0666,
};

static int oled_hw_init(void)
{
    static const u8 init_seq[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40,
        0x81, 0xFF, 0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0xF0, 0xD9, 0x22, 0xDA, 0x12, 0xDB,
        0x20, 0x8D, 0x14, 0xAF
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(init_seq); i++)
        oled_send_cmd(init_seq[i]);

    oled_clear();
    return 0;
}

static int oled_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;

    g_oled.client = client;
    mutex_init(&g_oled.lock);
    g_oled.x = 0;
    g_oled.page = 0;

    ret = oled_hw_init();
    if (ret)
        return ret;

    ret = misc_register(&oled_misc);
    if (ret)
        return ret;

    dev_info(&client->dev, "OLED SSD1306 char device registered: /dev/oled\n");
    return 0;
}

static void oled_remove(struct i2c_client *client)
{
    misc_deregister(&oled_misc);
}

/* DT(overlay) 매칭용 */
static const struct of_device_id oled_of_match[] = {
    { .compatible = "custom,ssd1306-oled" },
    { }
};
MODULE_DEVICE_TABLE(of, oled_of_match);

static const struct i2c_device_id oled_id[] = {
    { "oled_ssd1306_char", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, oled_id);

static struct i2c_driver oled_driver = {
    .driver = {
        .name = "oled_ssd1306_char",
        .of_match_table = oled_of_match,
    },
    .probe    = oled_probe,
    .remove   = oled_remove,
    .id_table = oled_id,
};

module_i2c_driver(oled_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwan-gi");
MODULE_DESCRIPTION("SSD1306 OLED char driver (Optimized Bulk Transfer)");
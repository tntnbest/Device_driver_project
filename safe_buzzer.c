#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pwm.h> 
#include <linux/timer.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "safe_buzzer"

MODULE_LICENSE("GPL");

static dev_t dev_num;
static struct cdev cdev;
static struct class *cls;

// 하드웨어 PWM 구조체
static struct pwm_device *pwm0 = NULL;
static struct timer_list stop_timer; // 소리를 끄기 위한 타이머

// 1kHz 소리 설정 (단위: 나노초)
#define PWM_PERIOD_NS 1000000   // 1ms (1kHz)
#define PWM_DUTY_NS   500000    // 0.5ms (Duty 50%)

// 소리 끄기 콜백 함수
void stop_buzzer_func(struct timer_list *t) {
    if (pwm0) {
        pwm_disable(pwm0);
    }
}

static ssize_t driver_write(struct file *f, const char __user *u, size_t c, loff_t *o) {
    int ms = 0;
    struct pwm_state state; // 추가

    if (copy_from_user(&ms, u, sizeof(int))) return -EFAULT;

    // 현재 PWM 상태 가져오기
    pwm_get_state(pwm0, &state);

    if (ms > 0) {
        state.period = PWM_PERIOD_NS;
        state.duty_cycle = PWM_DUTY_NS;
        state.enabled = true;
        pwm_apply_state(pwm0, &state); // 설정 적용

        mod_timer(&stop_timer, jiffies + msecs_to_jiffies(ms));
    } else {
        state.enabled = false;
        pwm_apply_state(pwm0, &state);
        del_timer(&stop_timer);
    }
    return c;
}

static struct file_operations fops = { .owner=THIS_MODULE, .write=driver_write };

static int __init safe_pwm_init(void) {
    alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    cdev_init(&cdev, &fops); cdev_add(&cdev, dev_num, 1);
    cls = class_create(THIS_MODULE, "pwm_buzzer_class");
    device_create(cls, NULL, dev_num, NULL, DRIVER_NAME);

    pwm0 = pwm_request(0, "safe_pwm");
    if (IS_ERR(pwm0)) {
        printk("PWM request failed!\n");
        return PTR_ERR(pwm0);
    }

    timer_setup(&stop_timer, stop_buzzer_func, 0);
    return 0;
}

static void __exit safe_pwm_exit(void) {
    if (pwm0) {
        pwm_disable(pwm0);
        pwm_free(pwm0);
    }
    del_timer_sync(&stop_timer);
    device_destroy(cls, dev_num); class_destroy(cls); cdev_del(&cdev); unregister_chrdev_region(dev_num, 1);
}

module_init(safe_pwm_init);
module_exit(safe_pwm_exit);
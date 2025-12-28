#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/jiffies.h>

#define DRIVER_NAME "safe_rotary"
#define CLASS_NAME "safe_rotary_class"
#define S1_GPIO 20
#define S2_GPIO 21
#define SW_GPIO 16
#define ROT_DEBOUNCE_MS 50

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkk");
MODULE_DESCRIPTION("rotary driver");

static dev_t device_number;
static struct cdev rotary_cdev;
static struct class *rotary_class;

static int irq_s1;
static int irq_sw;

static long rotary_value = 0;
static int btn_state = 0; // 1: Short, 2: Long
static int data_ready = 0;
static unsigned long last_rot_interrupt = 0;
static DECLARE_WAIT_QUEUE_HEAD(rotary_wait_queue);

// 버튼 롱프레스 감지를 위한 커널 타이머
static struct timer_list btn_timer;

// 1. 로터리 인터럽트 핸들러 (Falling Edge)
static irqreturn_t rot_handler(int irq, void *dev_id) {
    unsigned long current_time = jiffies;
    unsigned long debounce_jiffies = msecs_to_jiffies(ROT_DEBOUNCE_MS);

    // 디바운싱 체크: 마지막 인터럽트로부터 설정된 MS가 지나지 않았으면 무시
    if (time_before(current_time, last_rot_interrupt + debounce_jiffies)) {
        return IRQ_HANDLED;
    }
    last_rot_interrupt = current_time;

    // S1이 Falling일 때 S2의 레벨을 읽어 방향 판별
    if (gpio_get_value(S2_GPIO)) {
        rotary_value++;
    } else {
        rotary_value--;
    }

    data_ready = 1;
    wake_up_interruptible(&rotary_wait_queue);
    return IRQ_HANDLED;
}

// 버튼 타이머 콜백 (1초 경과 시 호출)
static void btn_timer_func(struct timer_list *t) {
    btn_state = 2; // Long Press 발생
    data_ready = 1;
    wake_up_interruptible(&rotary_wait_queue);
}

// 버튼 인터럽트 핸들러 (누름/뗌 양방향 감지)
static irqreturn_t btn_handler(int irq, void *dev_id) {
    int btn_val = gpio_get_value(SW_GPIO);

    if (btn_val == 0) { // 누름 (Falling)
        // 1초 뒤에 터지는 타이머 설정
        mod_timer(&btn_timer, jiffies + msecs_to_jiffies(1000));
    } 
    else { // 뗌 (Rising)
        // 타이머가 아직 실행 전이라면 취소하고 Short Press 처리
        if (del_timer(&btn_timer)) {
            btn_state = 1; // Short Press
            data_ready = 1;
            wake_up_interruptible(&rotary_wait_queue);
        }
    }
    return IRQ_HANDLED;
}

// Application 인터페이스
static ssize_t rotary_read(struct file *file, char __user *user_buff, size_t count, loff_t *ppos) {
    char buffer[32];
    int len;

    // 데이터가 준비될 때까지 대기 (Timeout 50ms 설정으로 앱 프리징 방지)
    if (wait_event_interruptible_timeout(rotary_wait_queue, data_ready != 0, msecs_to_jiffies(50)) <= 0) {
        return 0;
    }

    data_ready = 0;

    // 우선순위: 버튼 상태 확인 후 로터리 값 확인
    if (btn_state == 2) {
        len = snprintf(buffer, sizeof(buffer), "BTN_LONG\n");
    } else if (btn_state == 1) {
        len = snprintf(buffer, sizeof(buffer), "BTN_SHORT\n");
    } else {
        len = snprintf(buffer, sizeof(buffer), "%ld\n", rotary_value);
    }

    btn_state = 0; // 상태 초기화
    if (copy_to_user(user_buff, buffer, len)) return -EFAULT;

    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read  = rotary_read
};

// 초기화 함수
static int __init rotary_driver_init(void) {
    int ret;

    // 장치 번호 할당
    if ((ret = alloc_chrdev_region(&device_number, 0, 1, DRIVER_NAME)) < 0) return ret;

    // 문자 장치 초기화 및 등록
    cdev_init(&rotary_cdev, &fops);
    if ((ret = cdev_add(&rotary_cdev, device_number, 1)) < 0) {
        unregister_chrdev_region(device_number, 1);
        return ret;
    }

    // 클래스 및 장치 파일 생성 (/dev/safe_rotary)
    rotary_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(rotary_class)) {
        cdev_del(&rotary_cdev);
        unregister_chrdev_region(device_number, 1);
        return PTR_ERR(rotary_class);
    }
    device_create(rotary_class, NULL, device_number, NULL, DRIVER_NAME);

    // GPIO 요청 및 설정
    gpio_request(S1_GPIO, "s1"); gpio_direction_input(S1_GPIO);
    gpio_request(S2_GPIO, "s2"); gpio_direction_input(S2_GPIO);
    gpio_request(SW_GPIO, "sw"); gpio_direction_input(SW_GPIO);

    // 버튼 롱프레스 타이머 설정
    timer_setup(&btn_timer, btn_timer_func, 0);

    // 인터럽트 요청
    irq_s1 = gpio_to_irq(S1_GPIO);
    irq_sw = gpio_to_irq(SW_GPIO);

    // S1: Falling Edge 감지 (회전 감지용)
    ret = request_irq(irq_s1, rot_handler, IRQF_TRIGGER_FALLING, "rot_irq_s1", NULL);
    if (ret) {
        return ret;
    }

    // SW: 누름(Falling)과 뗌(Rising) 모두 감지 (타이머 제어용)
    ret = request_irq(irq_sw, btn_handler, IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "btn_irq_sw", NULL);
    if (ret) {
        free_irq(irq_s1, NULL); 
        return ret;
    }

    printk(KERN_INFO "Safe Rotary Driver initialized successfully\n");
    return 0;
}

static void __exit rotary_driver_exit(void) {
    del_timer_sync(&btn_timer);
    free_irq(irq_s1, NULL);
    free_irq(irq_sw, NULL);
    
    gpio_free(S1_GPIO);
    gpio_free(S2_GPIO);
    gpio_free(SW_GPIO);

    device_destroy(rotary_class, device_number);
    class_destroy(rotary_class);
    cdev_del(&rotary_cdev);
    unregister_chrdev_region(device_number, 1);
    
    printk(KERN_INFO "Safe Rotary Driver exited\n");
}

module_init(rotary_driver_init);
module_exit(rotary_driver_exit);
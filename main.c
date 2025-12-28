#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/time.h>

#define ROT_DEV  "/dev/safe_rotary"
#define BUZ_DEV  "/dev/safe_buzzer"
#define RTC_DEV  "/dev/ds1302"
#define OLED_DEV "/dev/oled"

// ===== OLED ioctl 정의 =====
#define OLED_IOC_MAGIC 'o'
typedef struct {
    unsigned char x;      // 0~127
    unsigned char page;   // 0~7
} oled_pos_t;

#define OLED_CLEAR  _IO(OLED_IOC_MAGIC, 0)
#define OLED_SETPOS _IOW(OLED_IOC_MAGIC, 1, oled_pos_t)

// ===== DS1302 ioctl 정의 =====
struct ds1302_time { unsigned char y, m, d, w, h, min, s; };
#define RTC_GET _IOR('d', 0, struct ds1302_time)
#define RTC_SET _IOW('d', 1, struct ds1302_time)

// fd
int rot_fd;
int buz_fd;
int oled_fd;

// 상태 정의
enum { STATE_MENU, STATE_GAME, STATE_SETTING };
int current_state = STATE_MENU;
int menu_cursor = 0;

// 게임 변수
int current_val = 50, time_left = 60;
int targets[4];
int stage = 0;
int game_clear = 0;
long last_beep_interval = 0;

// 설정 변수
int setting_step = 0;
struct ds1302_time temp_time;

long get_ms() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000) + (t.tv_usec / 1000);
}

void beep(int ms) { write(buz_fd, &ms, sizeof(int)); }
void stop_beep() { int z = 0; write(buz_fd, &z, sizeof(int)); }

static void oled_init_drv(void)
{
    oled_fd = open(OLED_DEV, O_RDWR);
    if (oled_fd < 0) {
        perror("OLED open fail");
        exit(1);
    }
    ioctl(oled_fd, OLED_CLEAR);
}

static void oled_cls_drv(void)
{
    ioctl(oled_fd, OLED_CLEAR);
}

static void oled_setpos_drv(int x, int page)
{
    oled_pos_t p;
    if (x < 0) x = 0;
    if (x > 127) x = 127;
    if (page < 0) page = 0;
    if (page > 7) page = 7;

    p.x = (unsigned char)x;
    p.page = (unsigned char)page;
    ioctl(oled_fd, OLED_SETPOS, &p);
}

static void oled_str_drv(int x, int page, const char *s)
{
    oled_setpos_drv(x, page);
    write(oled_fd, s, strlen(s));
}

// 성공음 
static void success_sound(void)
{
    beep(80);  usleep(40 * 1000);
    beep(80);  usleep(40 * 1000);
    beep(120); usleep(60 * 1000);
    beep(80);  usleep(40 * 1000);
    beep(80);  usleep(40 * 1000);
    beep(250); usleep(150 * 1000);
}

static void fireworks_sound(void)
{
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 12; i++) {
            beep(25);
            usleep(15 * 1000);
        }
        usleep(120 * 1000);
    }
    stop_beep();
}

static void fireworks_oled(void)
{
    oled_cls_drv();

    // 중앙 정렬 텍스트
    oled_str_drv(22, 1, "SAFE UNLOCKED!");
    oled_str_drv(34, 3, "CONGRATS!!");

    for (int i = 0; i < 6; i++) {
        oled_str_drv(25, 5, "*** BOOM ***");
        usleep(120 * 1000);
        oled_str_drv(25, 5, "             "); 
        usleep(120 * 1000);
    }
}

static void success_show(void)
{
    success_sound();
    fireworks_oled();
    fireworks_sound();
}

// Safe unsigned char adjust helpers (time setting)
static unsigned char wrap_add_u8(unsigned char base, int delta, int max_inclusive)
{
    int v = (int)base + delta;
    int mod = max_inclusive + 1;
    // C의 음수 % 처리 때문에 안전하게 보정
    v %= mod;
    if (v < 0) v += mod;
    return (unsigned char)v;
}

int main() {
    int rtc, p_val = 0, first = 1, p_sec = -1;
    long last_sec = 0;
    char buf[32];
    struct ds1302_time t;

    // OLED
    oled_init_drv();

    rot_fd = open(ROT_DEV, O_RDONLY);
    if (rot_fd < 0) { perror("Rotary open fail"); return 1; }

    buz_fd = open(BUZ_DEV, O_WRONLY);
    if (buz_fd < 0) { perror("Buzzer open fail"); return 1; }

    rtc = open(RTC_DEV, O_RDWR);
    if (rtc < 0) { perror("RTC open fail"); return 1; }

    srand(time(NULL));

    while (1) {
        long now = get_ms();
        int n = read(rot_fd, buf, 31);
        int delta = 0, btn_s = 0, btn_l = 0;

        if (n > 0) {
            buf[n] = 0;
            if (!strncmp(buf, "BTN_LONG", 8)) btn_l = 1;
            else if (!strncmp(buf, "BTN_SHORT", 9)) btn_s = 1;
            else {
                int curr = atoi(buf);
                if (!first) delta = curr - p_val;
                p_val = curr; first = 0;
            }
        }

        // 메인 메뉴
        if (current_state == STATE_MENU) {
            if (delta > 0) menu_cursor = 1;
            else if (delta < 0) menu_cursor = 0;

            if (btn_s) {
                if (menu_cursor == 0) {
                    current_state = STATE_GAME;
                    oled_cls_drv();

                    printf("\n[DEBUG] Answers: ");
                    for (int i = 0; i < 4; i++) {
                        targets[i] = rand() % 101;
                        printf("%d ", targets[i]);
                    }
                    printf("\n");

                    current_val = 50;
                    time_left = 60;
                    game_clear = 0;
                    stage = 0;
                    last_sec = now;
                    last_beep_interval = now;
                    beep(50);
                } else {
                    current_state = STATE_SETTING;
                    oled_cls_drv();
                    ioctl(rtc, RTC_GET, &temp_time);
                    setting_step = 0;
                    beep(50);
                }
                continue;
            }

            if (rtc > 0 && ioctl(rtc, RTC_GET, &t) >= 0 && t.s != p_sec) {
                snprintf(buf, 32, "TIME %02d:%02d:%02d", t.h, t.min, t.s);
                oled_str_drv(10, 0, buf);
                p_sec = t.s;
            }

            oled_str_drv(20, 2, "[ UNLOCK SAFE ]");
            oled_str_drv(10, 4, menu_cursor == 0 ? "> START GAME" : "  START GAME");
            oled_str_drv(10, 6, menu_cursor == 1 ? "> SETTINGS  " : "  SETTINGS  ");
        }

        // 설정 모드
        else if (current_state == STATE_SETTING) {
            if (delta != 0) {
                if (setting_step == 0) {
                    temp_time.h = wrap_add_u8(temp_time.h, delta, 23);
                }
                else if (setting_step == 1) {
                    temp_time.min = wrap_add_u8(temp_time.min, delta, 59);
                }
                else {
                    temp_time.s = wrap_add_u8(temp_time.s, delta, 59);
                }
            }

            if (btn_s) {
                setting_step++;
                beep(50);
                if (setting_step > 2) {
                    ioctl(rtc, RTC_SET, &temp_time);
                    current_state = STATE_MENU;
                    oled_cls_drv();
                    continue;
                }
            }

            oled_str_drv(30, 2, "SET TIME");

            char h_str[16], m_str[16], s_str[16];
            snprintf(h_str, sizeof(h_str), setting_step == 0 ? ">%02d" : " %02d", temp_time.h);
            snprintf(m_str, sizeof(m_str), setting_step == 1 ? ">%02d" : " %02d", temp_time.min);
            snprintf(s_str, sizeof(s_str), setting_step == 2 ? ">%02d" : " %02d", temp_time.s);

            oled_str_drv(20, 4, h_str);
            oled_str_drv(42, 4, ":");
            oled_str_drv(55, 4, m_str);
            oled_str_drv(77, 4, ":");
            oled_str_drv(90, 4, s_str);
        }

        // 게임 모드
        else if (current_state == STATE_GAME) {
            if (btn_l) {
                stop_beep();
                current_state = STATE_MENU;
                oled_cls_drv();
                continue;
            }

            if (game_clear) {
                // 성공 연출은 1회만 실행하도록
                success_show();
                // 연출 후 메뉴로 자동 복귀
                current_state = STATE_MENU;
                oled_cls_drv();
                stop_beep();
                continue;
            }

            if (delta != 0) {
                current_val += delta;
                if (current_val < 0) current_val = 0;
                if (current_val > 100) current_val = 100;
            }

            if (now - last_sec >= 1000) {
                time_left--;
                last_sec = now;
                if (time_left <= 0) {
                    current_state = STATE_MENU;
                    oled_cls_drv();
                    stop_beep();
                    continue;
                }
            }

            // 화면 갱신
            snprintf(buf, 32, "TIMER: %02d", time_left);
            oled_str_drv(30, 0, buf);

            // 목표 표시 
            char tmp[8];
            for (int i = 0; i < 4; i++) {
                int x_pos = 10 + (i * 30);
                if (i < stage) snprintf(tmp, sizeof(tmp), "%02d", targets[i]);
                else snprintf(tmp, sizeof(tmp), "**");
                oled_str_drv(x_pos, 3, tmp);
            }

            snprintf(buf, 32, "INPUT: %-3d", current_val);
            oled_str_drv(30, 5, buf);

            // 버튼 입력 처리
            if (btn_s) {
                if (current_val == targets[stage]) {
                    stage++;
                    beep(100); usleep(50 * 1000); beep(100);
                    if (stage >= 4) {
                        game_clear = 1;   // 다음 루프에서 success_show()
                        oled_cls_drv();
                        continue;
                    }
                } else {
                    oled_str_drv(30, 6, "WRONG!");
                    beep(200);
                    usleep(200 * 1000);
                    oled_str_drv(30, 6, "      ");
                }
            }

            // 근접 비프 (다음 정답 기준)
            if (stage < 4) {
                int dist = abs(targets[stage] - current_val);
                if (dist > 0 && dist < 40) {
                    int interval = dist * 40 + 100;
                    if (now - last_beep_interval > interval) {
                        beep(30);
                        last_beep_interval = now;
                    }
                }
            }
        }
    }

    close(rot_fd);
    close(buz_fd);
    close(rtc);
    close(oled_fd);
    return 0;
}

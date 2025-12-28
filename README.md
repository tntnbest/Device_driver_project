# 🔐 Safe Unlock System  
### Linux Device Driver 기반 금고 해제 & RTC 시계 시스템

> Raspberry Pi 4 · Linux Kernel Module  
> Intel Edge SW Academy Project

---

## 📌 프로젝트 요약

본 프로젝트는 **리눅스 디바이스 드라이버를 직접 설계**하여  
로터리 엔코더, OLED, RTC, PWM 부저를 제어하는 임베디드 시스템이다.  
입력 안정성과 실시간성을 핵심 목표로 하여,  
드라이버–애플리케이션 역할을 명확히 분리한 구조로 구현했다.

---

## 🎯 핵심 목표 (Driver 관점)

- GPIO / I2C / PWM 기반 커널 디바이스 드라이버 구현  
- 로터리 엔코더 입력 **노이즈 및 값 튐 문제 해결**  
- 인터럽트 vs 폴링 방식 비교를 통한 안정성 중심 설계  
- `/dev` 인터페이스 기반 Driver–Application 분리 구조 확립  

---

## 🧱 시스템 구조

User ↔ Application (Logic/UI) ↔ Device Driver (Kernel) ↔ Hardware

---

## 🎥 동작 화면 (GIF)

### ▶ 전체 시스템 흐름
![system_overview](docs/gif/system_overview.gif)

### ▶ 금고 해제 게임 (START_GAME)
![game_mode](docs/gif/game_mode.gif)

### ▶ 시간 설정 모드 (TIME_SETTING)
![time_setting](docs/gif/time_setting.gif)

---

## 🔧 디바이스 드라이버 구성

| Driver | 주요 역할 |
|------|---------|
| Rotary Driver | 로터리 회전 / 버튼 입력 처리 |
| OLED Driver | I2C 기반 UI 출력 |
| RTC Driver | 시간 조회 및 설정 |
| Buzzer Driver | PWM 기반 힌트 및 알림 |

---

## 🧪 오실로스코프 분석 (입력 안정성 검증)

### ▶ 로터리 엔코더 파형 분석
- 회전 방향(CW / CCW) 및 버튼 입력 파형 확인
- 인터럽트 방식에서 발생하는 **노이즈 및 타이밍 불안정 현상 확인**

![rotary_scope](docs/oscilloscope/rotary_encoder_scope.png)

---

### ▶ PWM 부저 파형 분석
- 주기 변화에 따른 출력 파형 확인
- 거리 기반 힌트음 동작 검증

![buzzer_scope](docs/oscilloscope/buzzer_pwm_scope.png)

---

## 🛠 주요 문제 해결

- **로터리 입력 튐 현상**
  - 인터럽트 방식 → 노이즈에 취약
  - **hrtimer 기반 1ms 폴링 방식으로 전환**
  - 입력 안정성 및 예측 가능한 CPU 부하 확보

- **OLED 출력 지연**
  - I2C 전송 구조 개선
  - UI 반응성 향상

---

## 🧠 프로젝트를 통해 얻은 역량

- Linux Kernel Module 설계 및 구현 경험  
- 하드웨어 노이즈 기반 문제 분석 능력  
- 실시간 시스템에서 안정성 중심 설계 사고  
- Driver–Application 분리 구조 이해  

---

## 👤 팀 구성

- **김기환** : 디바이스 드라이버 설계, 입력 안정성 개선, 시스템 구조 설계  
- **이동현** : 애플리케이션 로직, UI 구성 및 테스트  


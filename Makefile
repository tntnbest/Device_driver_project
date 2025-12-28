obj-m := oled_ssd1306.o rotary_interupt.o ds1302.o safe_buzzer.o

KDIR := /home/ubuntu/linux

ARCH := arm64
CROSS_COMPILE := aarch64-linux-gnu-

PWD := $(shell pwd)

all:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean

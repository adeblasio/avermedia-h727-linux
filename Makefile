obj-m += avertm62.o

BUILD_DIR := /tmp/avertm62_build

all:
	@rm -rf $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)
	@cp avertm62.c $(BUILD_DIR)/
	@cp -r kheaders $(BUILD_DIR)/
	@echo "obj-m += avertm62.o" > $(BUILD_DIR)/Makefile
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(BUILD_DIR) modules
	@cp $(BUILD_DIR)/avertm62.ko .

clean:
	@rm -rf $(BUILD_DIR)
	@rm -f *.o *.ko *.mod.c *.mod *.symvers *.order .*.cmd

load: all
	sudo modprobe videodev
	sudo modprobe videobuf2-v4l2
	sudo modprobe videobuf2-vmalloc
	-sudo rmmod avertm62 2>/dev/null
	sudo insmod avertm62.ko
	sudo dmesg | tail -n 15

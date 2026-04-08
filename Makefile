# Makefile for remote_mouse_server (imx8mp device side)
# 
# 本地编译:  make
# 交叉编译:  make CC=aarch64-linux-gnu-gcc
# 清理:      make clean

CC      ?= gcc
CFLAGS  := -Wall -Wextra -O2
TARGET  := remote_mouse_server
SRC     := remote_mouse_server.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f $(TARGET)

/*************************************************************************
	> File Name: remote_mouse_server.c
	> Author: jhonconal
	> Mail: jhonconal2016@gmail.com
	> Created Time: Wed Apr  8 16:31:32 2026
	> Copyright (C) 2025 jhonconal,All Rights Reserved.
 ************************************************************************/
// remote_mouse_server.c
// 设备端(imx8mp)远程鼠标服务器
// 接收客户端发送的绝对坐标，通过 uinput 注入到系统
//
// 通信协议:
//   握手阶段（服务端 -> 客户端，8字节）:
//     [0..3]  int32_t  screen_width   - 设备屏幕宽度
//     [4..7]  int32_t  screen_height  - 设备屏幕高度
//
//   数据阶段（客户端 -> 服务端，每帧12字节，小端序）:
//     [0..3]  int32_t  abs_x      - 绝对X坐标 (0 ~ screen_width-1)
//     [4..7]  int32_t  abs_y      - 绝对Y坐标 (0 ~ screen_height-1)
//     [8..11] int32_t  buttons    - 按钮状态 bit0=Left, bit1=Right, bit2=Middle
//
// 用法:
//   ./remote_mouse_server                    # 自动检测分辨率
//   ./remote_mouse_server 1920 1080          # 手动指定分辨率
//   ./remote_mouse_server -p 8888            # 指定端口
//   ./remote_mouse_server -p 8888 1280 720   # 指定端口和分辨率

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <dirent.h>
#include <glob.h>

#define DEFAULT_PORT  9999
#define MOUSE_DEV     "/dev/uinput"
#define FRAME_SIZE    (3 * sizeof(int32_t))

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// 发送输入事件
static void emit(int fd, uint16_t type, uint16_t code, int32_t val) {
    struct input_event ie = {0};
    ie.type = type;
    ie.code = code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0) {
        perror("write input_event");
    }
}

// 安全读取 n 字节
static int read_exact(int fd, void *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, (char *)buf + total, n - total);
        if (r <= 0) return -1;
        total += r;
    }
    return 0;
}

// 安全写入 n 字节
static int write_exact(int fd, const void *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t w = write(fd, (const char *)buf + total, n - total);
        if (w <= 0) return -1;
        total += w;
    }
    return 0;
}

// ---- 屏幕分辨率自动检测 ----

// 方法1: 通过 framebuffer 获取
static int detect_resolution_fb(int32_t *w, int32_t *h) {
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) return -1;

    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    if (vinfo.xres > 0 && vinfo.yres > 0) {
        *w = (int32_t)vinfo.xres;
        *h = (int32_t)vinfo.yres;
        return 0;
    }
    return -1;
}

// 方法2: 通过 DRM sysfs 获取
static int detect_resolution_drm(int32_t *w, int32_t *h) {
    // 尝试读取 /sys/class/drm/card*-*/modes
    glob_t globbuf;
    if (glob("/sys/class/drm/card*-*/modes", 0, NULL, &globbuf) != 0) {
        return -1;
    }

    int found = 0;
    for (size_t i = 0; i < globbuf.gl_pathc && !found; i++) {
        FILE *fp = fopen(globbuf.gl_pathv[i], "r");
        if (!fp) continue;

        char line[64];
        if (fgets(line, sizeof(line), fp)) {
            int rw = 0, rh = 0;
            if (sscanf(line, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
                *w = (int32_t)rw;
                *h = (int32_t)rh;
                found = 1;
            }
        }
        fclose(fp);
    }

    globfree(&globbuf);
    return found ? 0 : -1;
}

// 方法3: 通过 xrandr 获取 (如果有 X11)
static int detect_resolution_xrandr(int32_t *w, int32_t *h) {
    FILE *fp = popen("xrandr --current 2>/dev/null | grep '\\*' | head -1", "r");
    if (!fp) return -1;

    char line[256];
    int found = 0;
    if (fgets(line, sizeof(line), fp)) {
        int rw = 0, rh = 0;
        if (sscanf(line, " %dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            *w = (int32_t)rw;
            *h = (int32_t)rh;
            found = 1;
        }
    }
    pclose(fp);
    return found ? 0 : -1;
}

// 综合检测：fb -> drm -> xrandr
static int detect_screen_resolution(int32_t *w, int32_t *h) {
    if (detect_resolution_drm(w, h) == 0) {
        printf("[INFO] Resolution detected via DRM sysfs: %dx%d\n", *w, *h);
        return 0;
    }
    if (detect_resolution_fb(w, h) == 0) {
        printf("[INFO] Resolution detected via framebuffer: %dx%d\n", *w, *h);
        return 0;
    }
    if (detect_resolution_xrandr(w, h) == 0) {
        printf("[INFO] Resolution detected via xrandr: %dx%d\n", *w, *h);
        return 0;
    }
    return -1;
}

// ---- uinput 设备创建 ----
static int create_uinput_device(int32_t screen_w, int32_t screen_h) {
    int uifd = open(MOUSE_DEV, O_WRONLY | O_NONBLOCK);
    if (uifd < 0) {
        perror("Cannot open /dev/uinput");
        return -1;
    }

    ioctl(uifd, UI_SET_EVBIT, EV_KEY);
    ioctl(uifd, UI_SET_EVBIT, EV_ABS);

    ioctl(uifd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uifd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(uifd, UI_SET_KEYBIT, BTN_MIDDLE);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Remote Virtual Mouse");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    // X 轴: 0 ~ screen_w-1
    uidev.absmin[ABS_X]  = 0;
    uidev.absmax[ABS_X]  = screen_w - 1;
    uidev.absfuzz[ABS_X] = 0;
    uidev.absflat[ABS_X] = 0;

    // Y 轴: 0 ~ screen_h-1
    uidev.absmin[ABS_Y]  = 0;
    uidev.absmax[ABS_Y]  = screen_h - 1;
    uidev.absfuzz[ABS_Y] = 0;
    uidev.absflat[ABS_Y] = 0;

    ioctl(uifd, UI_SET_ABSBIT, ABS_X);
    ioctl(uifd, UI_SET_ABSBIT, ABS_Y);

    if (write(uifd, &uidev, sizeof(uidev)) < 0) {
        perror("write uidev");
        close(uifd);
        return -1;
    }

    if (ioctl(uifd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(uifd);
        return -1;
    }

    printf("[INFO] Virtual mouse created (ABS mode, %dx%d)\n", screen_w, screen_h);
    return uifd;
}

// ---- 用法提示 ----
static void print_usage(const char *prog) {
    printf("Usage: %s [-p port] [width height]\n", prog);
    printf("  -p port       TCP listening port (default: %d)\n", DEFAULT_PORT);
    printf("  width height  Manual screen resolution override\n");
    printf("  If no resolution is given, auto-detect via fb0/DRM/xrandr\n");
    printf("\nExamples:\n");
    printf("  %s                      # auto-detect resolution\n", prog);
    printf("  %s 1920 1080            # manual 1920x1080\n", prog);
    printf("  %s -p 8888 1280 720     # port 8888, 1280x720\n", prog);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int32_t screen_w = 0, screen_h = 0;
    int manual_res = 0;

    // 解析命令行参数
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (i + 1 < argc && !manual_res) {
            screen_w = atoi(argv[i]);
            screen_h = atoi(argv[i + 1]);
            manual_res = 1;
            i += 2;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    // 如果没有手动指定分辨率，自动检测
    if (!manual_res) {
        if (detect_screen_resolution(&screen_w, &screen_h) != 0) {
            fprintf(stderr, "[ERROR] Cannot detect screen resolution.\n");
            fprintf(stderr, "        Please specify manually: %s <width> <height>\n", argv[0]);
            return 1;
        }
    } else {
        if (screen_w <= 0 || screen_h <= 0) {
            fprintf(stderr, "[ERROR] Invalid resolution: %dx%d\n", screen_w, screen_h);
            return 1;
        }
        printf("[INFO] Using manual resolution: %dx%d\n", screen_w, screen_h);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // 1. 创建 uinput 设备
    int uifd = create_uinput_device(screen_w, screen_h);
    if (uifd < 0) return 1;

    // 2. 创建 TCP 服务器
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        ioctl(uifd, UI_DEV_DESTROY);
        close(uifd);
        return 1;
    }

    int optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port        = htons(port);

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(sockfd);
        ioctl(uifd, UI_DEV_DESTROY);
        close(uifd);
        return 1;
    }

    if (listen(sockfd, 1) < 0) {
        perror("listen");
        close(sockfd);
        ioctl(uifd, UI_DEV_DESTROY);
        close(uifd);
        return 1;
    }

    printf("[INFO] Server listening on port %d (screen %dx%d)\n", port, screen_w, screen_h);

    // 支持多次连接，使用 select 实现 accept 超时
    printf("[INFO] Server ready, waiting for connections...\n");
    while (g_running) {
        // 使用 select 等待连接，设置 1 秒超时
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int ret = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                // 被信号中断（如 Ctrl+C）
                break;
            }
            perror("select");
            continue;
        } else if (ret == 0) {
            // 超时，继续循环检查 g_running
            continue;
        }
        
        // 有连接请求
        int connfd = accept(sockfd, NULL, NULL);
        if (connfd < 0) {
            if (!g_running) break;
            perror("accept");
            continue;
        }

        int flag = 1;
        setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        printf("[INFO] Client connected!\n");

        // ---- 握手: 发送屏幕分辨率给客户端 ----
        int32_t handshake[2] = { screen_w, screen_h };
        if (write_exact(connfd, handshake, sizeof(handshake)) < 0) {
            printf("[WARN] Failed to send handshake, client disconnected.\n");
            close(connfd);
            continue;
        }
        printf("[INFO] Sent resolution to client: %dx%d\n", screen_w, screen_h);

        // 3. 主循环：接收绝对坐标并注入 uinput
        int32_t frame[3];
        int last_btn = 0;

        while (g_running) {
            // 使用 select 等待客户端数据，设置 1 秒超时
            fd_set readfds_conn;
            struct timeval timeout_conn;
            FD_ZERO(&readfds_conn);
            FD_SET(connfd, &readfds_conn);
            timeout_conn.tv_sec = 1;
            timeout_conn.tv_usec = 0;
            
            int ret_conn = select(connfd + 1, &readfds_conn, NULL, NULL, &timeout_conn);
            if (ret_conn < 0) {
                if (errno == EINTR) {
                    printf("[INFO] Connection select interrupted by signal\n");
                    break;
                }
                perror("select on connection");
                break;
            } else if (ret_conn == 0) {
                // 超时，继续循环检查 g_running
                continue;
            }
            
            if (read_exact(connfd, frame, FRAME_SIZE) < 0) {
                printf("[INFO] Client disconnected.\n");
                break;
            }

            int32_t abs_x   = frame[0];
            int32_t abs_y   = frame[1];
            int32_t buttons = frame[2];

            // 边界保护
            if (abs_x < 0) abs_x = 0;
            if (abs_x >= screen_w) abs_x = screen_w - 1;
            if (abs_y < 0) abs_y = 0;
            if (abs_y >= screen_h) abs_y = screen_h - 1;

            emit(uifd, EV_ABS, ABS_X, abs_x);
            emit(uifd, EV_ABS, ABS_Y, abs_y);

            if ((buttons & 1) != (last_btn & 1))
                emit(uifd, EV_KEY, BTN_LEFT, (buttons & 1) ? 1 : 0);
            if ((buttons & 2) != (last_btn & 2))
                emit(uifd, EV_KEY, BTN_RIGHT, (buttons & 2) ? 1 : 0);
            if ((buttons & 4) != (last_btn & 4))
                emit(uifd, EV_KEY, BTN_MIDDLE, (buttons & 4) ? 1 : 0);
            last_btn = buttons;

            emit(uifd, EV_SYN, SYN_REPORT, 0);
        }

        close(connfd);
    }

    printf("[INFO] Shutting down...\n");
    ioctl(uifd, UI_DEV_DESTROY);
    close(uifd);
    close(sockfd);
    return 0;
}

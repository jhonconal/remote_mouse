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
//   ./remote_mouse_server -s 1920x1080       # 显式指定分辨率 (-s 1920x1080 或 -s 1920 1080)
//   ./remote_mouse_server -r 90              # 自动检测分辨率并旋转90度(适用竖屏)
//   ./remote_mouse_server -res 1080x1920 -r 90  # 指定竖屏分辨率与旋转角度
//   ./remote_mouse_server -p 8888 -s 1280x720   # 指定端口与分辨率

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

// ---- 屏幕分辨率解析助手 ----
static int parse_resolution_arg(const char *arg, const char *next_arg, int32_t *w, int32_t *h, int *consumed) {
    if (!arg) return -1;
    int rw = 0, rh = 0;
    // 尝试解析 "1920x1080" 或 "1920X1080"
    if (sscanf(arg, "%dx%d", &rw, &rh) == 2 || sscanf(arg, "%dX%d", &rw, &rh) == 2) {
        if (rw > 0 && rh > 0) {
            *w = (int32_t)rw;
            *h = (int32_t)rh;
            *consumed = 1;
            return 0;
        }
    }
    // 尝试解析 "1920" "1080" 两个独立参数
    if (next_arg != NULL) {
        if (sscanf(arg, "%d", &rw) == 1 && sscanf(next_arg, "%d", &rh) == 1) {
            if (rw > 0 && rh > 0) {
                *w = (int32_t)rw;
                *h = (int32_t)rh;
                *consumed = 2;
                return 0;
            }
        }
    }
    return -1;
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

// 综合检测：fb -> drm -> xrandr (内部实现)
static int detect_screen_resolution_internal(int32_t *w, int32_t *h, int verbose) {
    if (detect_resolution_drm(w, h) == 0) {
        if (verbose) printf("[INFO] Resolution detected via DRM sysfs: %dx%d\n", *w, *h);
        return 0;
    }
    if (detect_resolution_fb(w, h) == 0) {
        if (verbose) printf("[INFO] Resolution detected via framebuffer: %dx%d\n", *w, *h);
        return 0;
    }
    if (detect_resolution_xrandr(w, h) == 0) {
        if (verbose) printf("[INFO] Resolution detected via xrandr: %dx%d\n", *w, *h);
        return 0;
    }
    return -1;
}

static int detect_screen_resolution(int32_t *w, int32_t *h) {
    return detect_screen_resolution_internal(w, h, 1);
}

static int detect_screen_resolution_silent(int32_t *w, int32_t *h) {
    return detect_screen_resolution_internal(w, h, 0);
}

// ---- 坐标旋转转换 ----
static void transform_coordinates(int32_t in_x, int32_t in_y,
                                  int32_t screen_w, int32_t screen_h,
                                  int rotation,
                                  int32_t *out_x, int32_t *out_y) {
    int32_t max_x, max_y;
    switch (rotation) {
        case 90:
            *out_x = (screen_h - 1) - in_y;
            *out_y = in_x;
            max_x = screen_h - 1;
            max_y = screen_w - 1;
            break;
        case 180:
            *out_x = (screen_w - 1) - in_x;
            *out_y = (screen_h - 1) - in_y;
            max_x = screen_w - 1;
            max_y = screen_h - 1;
            break;
        case 270:
            *out_x = in_y;
            *out_y = (screen_w - 1) - in_x;
            max_x = screen_h - 1;
            max_y = screen_w - 1;
            break;
        case 0:
        default:
            *out_x = in_x;
            *out_y = in_y;
            max_x = screen_w - 1;
            max_y = screen_h - 1;
            break;
    }
    if (*out_x < 0) *out_x = 0;
    if (*out_x > max_x) *out_x = max_x;
    if (*out_y < 0) *out_y = 0;
    if (*out_y > max_y) *out_y = max_y;
}

// ---- uinput 设备创建 ----
static int create_uinput_device(int32_t screen_w, int32_t screen_h, int rotation) {
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

    int32_t max_x = screen_w - 1;
    int32_t max_y = screen_h - 1;
    if (rotation == 90 || rotation == 270) {
        max_x = screen_h - 1;
        max_y = screen_w - 1;
    }

    // X 轴: 0 ~ max_x
    uidev.absmin[ABS_X]  = 0;
    uidev.absmax[ABS_X]  = max_x;
    uidev.absfuzz[ABS_X] = 0;
    uidev.absflat[ABS_X] = 0;

    // Y 轴: 0 ~ max_y
    uidev.absmin[ABS_Y]  = 0;
    uidev.absmax[ABS_Y]  = max_y;
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

    printf("[INFO] Virtual mouse created (ABS mode, bounds X: 0..%d, Y: 0..%d, rotation: %d°)\n",
           max_x, max_y, rotation);
    return uifd;
}

// ---- 用法提示 ----
static void print_usage(const char *prog) {
    int32_t w = 0, h = 0;
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p port                 TCP listening port (default: %d)\n", DEFAULT_PORT);
    printf("  -s, -res, --resolution  Screen resolution, e.g. -s 1920x1080 or -s 1920 1080\n");
    printf("  -r rotation             Screen rotation angle in degrees: 0, 90, 180, 270 (default: 0)\n");
    printf("  -h, --help              Show this help message\n");
    printf("\nScreen Detection Info:\n");
    if (detect_screen_resolution_silent(&w, &h) == 0) {
        printf("  Current detected resolution: %dx%d (%s)\n", w, h, (w < h) ? "Portrait/竖屏" : "Landscape/横屏");
        if (w < h) {
            printf("\n[NOTICE / 提示] 检测到当前屏幕为竖屏 (Width %d < Height %d)！\n", w, h);
            printf("                如果输入坐标与显示画面方向不匹配，请使用 -r 参数设置旋转角度。\n");
            printf("                例如: %s -r 90  或  %s -r 270\n", prog, prog);
        }
    } else {
        printf("  Screen resolution auto-detection failed or unavailable.\n");
    }
    printf("\nExamples:\n");
    printf("  %s                              # auto-detect resolution\n", prog);
    printf("  %s -s 1920x1080                 # manual resolution via -s\n", prog);
    printf("  %s -res 1080x1920 -r 90         # manual portrait resolution with 90° rotation\n", prog);
    printf("  %s -p 8888 -s 1280x720          # custom port and resolution\n", prog);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int rotation = 0;
    int32_t screen_w = 0, screen_h = 0;
    int manual_res = 0;

    // 解析命令行参数
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rotation = atoi(argv[i + 1]);
            if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
                fprintf(stderr, "[ERROR] Invalid rotation angle: %s. Supported angles: 0, 90, 180, 270\n", argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-res") == 0 ||
                   strcmp(argv[i], "--res") == 0 || strcmp(argv[i], "--resolution") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[ERROR] Missing argument for %s\n", argv[i]);
                return 1;
            }
            int consumed = 0;
            const char *next_arg = (i + 2 < argc) ? argv[i + 2] : NULL;
            if (parse_resolution_arg(argv[i + 1], next_arg, &screen_w, &screen_h, &consumed) == 0) {
                manual_res = 1;
                i += 1 + consumed;
            } else {
                fprintf(stderr, "[ERROR] Invalid resolution format for %s: %s\n", argv[i], argv[i + 1]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    // 如果没有手动指定分辨率，自动检测
    if (!manual_res) {
        if (detect_screen_resolution(&screen_w, &screen_h) != 0) {
            fprintf(stderr, "[ERROR] Cannot detect screen resolution.\n");
            fprintf(stderr, "        Please specify manually: %s -s <width>x<height>\n", argv[0]);
            return 1;
        }
    } else {
        if (screen_w <= 0 || screen_h <= 0) {
            fprintf(stderr, "[ERROR] Invalid resolution: %dx%d\n", screen_w, screen_h);
            return 1;
        }
        printf("[INFO] Using manual resolution: %dx%d\n", screen_w, screen_h);
    }

    if (screen_w < screen_h) {
        printf("[NOTICE] Portrait screen mode detected (Width %d < Height %d).\n", screen_w, screen_h);
        printf("         If input directions are inverted or mismatched, specify '-r 90' or '-r 270'.\n");
    }
    if (rotation != 0) {
        printf("[INFO] Screen rotation enabled: %d°\n", rotation);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // 1. 创建 uinput 设备
    int uifd = create_uinput_device(screen_w, screen_h, rotation);
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

    printf("[INFO] Server listening on port %d (screen %dx%d, rotation %d°)\n", port, screen_w, screen_h, rotation);

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

            int32_t final_x = 0, final_y = 0;
            transform_coordinates(abs_x, abs_y, screen_w, screen_h, rotation, &final_x, &final_y);

            emit(uifd, EV_ABS, ABS_X, final_x);
            emit(uifd, EV_ABS, ABS_Y, final_y);

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


/**
 * cam_stream.c — 精简 WebSocket 视频推流 (纯 socket, 无 httpd 依赖)
 * ==================================================================
 * - 直接 TCP socket 监听 80 端口
 * - 手动 WebSocket 握手 + Binary Frame 推送
 * - JPEG 320x240, quality 50, ~10fps
 */

#include "cam_stream.h"
#include "esp_camera.h"
#include "ui_layout.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "esp_cache.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <errno.h>
#include <limits.h>

static const char *TAG = "CamWS";

/* ── 全局 ── */
static int              g_listen_fd  = -1;
static int              g_clients    = 0;
static SemaphoreHandle_t g_mutex     = NULL;
static int64_t          g_last_send_ms = 0;

#define MAX_CLIENTS 3
static int g_client_fds[MAX_CLIENTS] = {-1, -1, -1};

/* ── 诊断计数 ── */
static int      g_fb_ok       = 0;   /* fb_get 成功次数  */
static int      g_fb_fail     = 0;   /* fb_get 失败次数  */
static volatile int  g_pause_count = 0;  /* ASR/TTS 事务引用计数: >0 暂停推流, 让出 WiFi 上行
                                          * (计数而非 bool: 防 A 事务未结束时 B 事务的 resume 提前放行) */
static portMUX_TYPE g_pause_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t  g_diag_ms     = 0;   /* 上次 FB 帧头日志时间 */
static int64_t  g_bc_ms       = 0;   /* 上次广播日志时间 */

/* ── 精简 HTML (内嵌) ── */
static const char HTML[] =
"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
"Connection: close\r\n\r\n"
"<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport'"
" content='width=device-width,initial-scale=1'><title>智瞳伴读</title>"
"<style>body{margin:0;background:#111;display:flex;flex-direction:column;"
"align-items:center;justify-content:center;min-height:100vh;font-family:sans-serif}"
"h2{color:#aaa;margin:8px 0}#stream{max-width:100vw;max-height:85vh;"
"border-radius:4px}#status{color:#888;font-size:12px;}"
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px}"
".live{background:#0f0}.dead{background:#f00}</style></head><body>"
"<h2>智瞳伴读</h2>"
"<span id='status'><span class='dot dead' id='dot'></span>等待连接...</span>"
"<img id='stream' src='' alt='stream'>"
"<script>"
"var img=document.getElementById('stream');"
"var dot=document.getElementById('dot');"
"var st=document.getElementById('status');"
"var url='ws://'+location.host+'/';"
"var ws=null,wdt=null;function poke(){clearTimeout(wdt);wdt=setTimeout(function(){try{ws.close()}catch(e){}},15000)};"
"function connect(){"
" ws=new WebSocket(url);ws.binaryType='blob';"
" ws.onopen=function(){st.textContent='已连接';dot.className='dot live'};"
" ws.onmessage=function(e){poke();"
"  var b=new Blob([e.data],{type:'image/jpeg'});"
"  var o=img.src;img.src=URL.createObjectURL(b);"
"  if(o&&o.startsWith('blob:'))URL.revokeObjectURL(o);"
" };"
" ws.onclose=function(){st.textContent='断开,重连中...';dot.className='dot dead';setTimeout(connect,1500)};"
" ws.onerror=function(){ws.close()};"
"}connect();</script>"
"</body></html>";

/* ═══════════════════════════════════════════════════════════
 *  WebSocket 握手响应 (RFC6455)
 *  Sec-WebSocket-Accept = base64( SHA1( client_key + GUID ) )
 * ═══════════════════════════════════════════════════════════ */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static void ws_accept_key(const char *client_key, char out[32])
{
    unsigned char sha[20];
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s", client_key, WS_GUID);
    mbedtls_sha1((const unsigned char*)buf, strlen(buf), sha);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char*)out, 32, &olen, sha, 20);
    out[olen] = 0;
}

/* ═══════════════════════════════════════════════════════════
 *  WS Binary Frame 发送 (纯 socket)
 * ═══════════════════════════════════════════════════════════ */
static int ws_send_binary(int fd, const uint8_t *data, size_t len)
{
    uint8_t hdr[10];
    int hl;
    hdr[0] = 0x82;
    if (len < 126) {
        hdr[1] = (uint8_t)len; hl = 2;
    } else if (len <= 65535) {
        hdr[1] = 126; hdr[2] = (uint8_t)(len>>8); hdr[3] = (uint8_t)len; hl = 4;
    } else {
        hdr[1] = 127; hl = 10;
        for (int i=0;i<8;i++) hdr[2+i] = (uint8_t)(len>>(56-i*8));
    }
    /* 非阻塞发送: 发不出去就跳过本帧, TCP 重传后台继续, 链路恢复自动续流
     * 返回: 0=整帧发出; 1=链路忙干净跳过(连接保留); -1=流损坏/硬错误(必须关连接) */
    int off = 0;
    while (off < hl) {
        int n = send(fd, hdr + off, hl - off, MSG_DONTWAIT);
        if (n < 0) return off > 0 ? -1 : 1;   /* 帧头都没发出去→干净跳过 */
        off += n;
    }
    off = 0;
    int64_t t_start = esp_timer_get_time();
    while (off < (int)len) {
        int n = send(fd, data + off, len - off, MSG_DONTWAIT);
        if (n < 0) {
            /* EAGAIN: sndbuf 满 (热点 RTT 84-300ms 时慢启动 cwnd 涨得慢, 必然发生)。
             * 等 ACK 滑窗 + cwnd 增长, 1s 内等不到才判死 (帧头已发数据没发完→流损坏)。
             * 实测: 200ms 不够 (51.77s EAGAIN 判死), RTT 84ms 下 1s≈12 个 RTT, cwnd 涨够。 */
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                esp_timer_get_time() - t_start < 1000000) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            return -1;
        }
        off += n;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  WS 广播
 * ═══════════════════════════════════════════════════════════ */
static void ws_broadcast(const uint8_t *data, size_t len)
{
    if (g_pause_count > 0) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_client_fds[i] >= 0) {
            int r = ws_send_binary(g_client_fds[i], data, len);
            if (r == -1) {
                ESP_LOGW(TAG, "WS send fail errno=%d (%s), closing fd=%d",
                         errno, strerror(errno), g_client_fds[i]);
                close(g_client_fds[i]);
                g_client_fds[i] = -1;
                g_clients--;
            }
            /* r==1: 链路忙, 跳过本帧, 连接保留 */
        }
    }
    /* 诊断: 1 秒限流广播日志 (独立节流, 不与 FB 日志互相吞) */
    int64_t now = esp_timer_get_time() / 1000;
    if (now - g_bc_ms > 1000) {
        g_bc_ms = now;
        ESP_LOGI(TAG, "BC: %u bytes -> %d clients", (unsigned)len, g_clients);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  接受新连接 & WebSocket 握手
 * ═══════════════════════════════════════════════════════════ */
static void handle_accept(int fd)
{
    /* 收发超时保护: 链路坏时 send/recv 不会永久卡死任务 */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* 发送超时放宽到 10s: RF 偶发丢包时 TCP 重传要好几秒才恢复, 3s 太短会误杀连接 */
    tv.tv_sec = 10;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* 热点 RTT 100-300ms: BDP≈9KB > 默认 sndbuf 5.7KB → 每帧必 EAGAIN; 加大缓冲
     * (PSRAM 卸载已开, 不挤内部 RAM) */
    int sndbuf = 16384;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    char buf[1024];
    int n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) { close(fd); return; }
    buf[n] = 0;

    /* 诊断: 打印请求首行 */
    char line1[80];
    int li = 0;
    while (li < 79 && buf[li] && buf[li] != '\r' && buf[li] != '\n') { line1[li] = buf[li]; li++; }
    line1[li] = 0;
    ESP_LOGI(TAG, "REQ: %s", line1);

    /* 检查 WebSocket 升级请求 */
    if (strstr(buf, "Upgrade: websocket") || strstr(buf, "upgrade: websocket")) {
        /* 提取 Sec-WebSocket-Key (浏览器每次随机, 必须算 accept) */
        char *kp = strstr(buf, "Sec-WebSocket-Key:");
        if (!kp) kp = strstr(buf, "sec-websocket-key:");
        if (!kp) { close(fd); return; }
        kp += 18; /* strlen("Sec-WebSocket-Key:") */
        while (*kp == ' ') kp++;
        char key[64];
        int i = 0;
        while (kp[i] && kp[i] != '\r' && kp[i] != '\n' && i < 63) { key[i] = kp[i]; i++; }
        key[i] = 0;

        char acc[32];
        ws_accept_key(key, acc);

        char rsp[256];
        int rl = snprintf(rsp, sizeof(rsp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "\r\n", acc);
        send(fd, rsp, rl, 0);

        /* 客户端套接字改非阻塞: send 永不卡住摄像头任务 */
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        /* 禁 Nagle: 每个 256B 小段立即发出, 不等 ACK 合并成大帧 (大帧在此环境丢包严重) */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_client_fds[i] < 0) {
                g_client_fds[i] = fd;
                g_clients++;
                ESP_LOGI(TAG, "WS client (clients=%d)", g_clients);
                return;
            }
        }
        ESP_LOGW(TAG, "Too many clients");
        close(fd);
        return;
    }

    /* 普通 HTTP → 返回 HTML (非阻塞小块发送 + EAGAIN 重试, 3s 上限) */
    const char *p = HTML;
    int left = (int)sizeof(HTML) - 1;
    int total = 0, chunk_n = 0;
    int64_t t_start = esp_timer_get_time();
    while (left > 0) {
        int n = send(fd, p, left > 256 ? 256 : left, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* sndbuf 满: 等 ACK 滑窗后重试, 保证页面完整送达 */
                if (esp_timer_get_time() - t_start > 3000000) {
                    ESP_LOGW(TAG, "HTTP send timeout (sent %d/%u)", total, (unsigned)(sizeof(HTML)-1));
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGW(TAG, "HTTP chunk%d send ret=%d errno=%d (sent %d/%u)",
                     chunk_n, n, errno, total, (unsigned)(sizeof(HTML)-1));
            break;
        }
        total += n; p += n; left -= n; chunk_n++;
    }
    ESP_LOGI(TAG, "HTTP sent %d/%u in %d chunks", total, (unsigned)(sizeof(HTML)-1), chunk_n);
    close(fd);
}

/* ═══════════════════════════════════════════════════════════
 *  TCP 服务器任务
 * ═══════════════════════════════════════════════════════════ */
static void tcp_server_task(void *arg)
{
    (void)arg;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { ESP_LOGE(TAG, "socket fail"); return; }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(80),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind fail"); close(g_listen_fd); return;
    }
    if (listen(g_listen_fd, 5) < 0) {
        ESP_LOGE(TAG, "listen fail"); close(g_listen_fd); return;
    }

    /* 非阻塞 */
    int flags = fcntl(g_listen_fd, F_GETFL, 0);
    fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "TCP server :80");

    while (1) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            handle_accept(cfd);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 *  抓图 + 广播任务
 * ═══════════════════════════════════════════════════════════ */
static void cam_stream_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        if (g_pause_count > 0) {   /* ASR/TTS 事务期间: 不抓帧不广播, 省 DMA/CPU/WiFi 上行 */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (now - g_last_send_ms < 200) {   /* 5fps: 热点 BDP 减半, 干扰死区更抗揍 */
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        g_last_send_ms = now;

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            g_fb_fail++;
            xSemaphoreGive(g_mutex);
            /* 诊断: 5 秒一次心跳 */
            int64_t now2 = esp_timer_get_time() / 1000;
            if (now2 - g_diag_ms > 5000) {
                g_diag_ms = now2;
                ESP_LOGW(TAG, "fb_get NULL x%d (ok=%d, clients=%d)", g_fb_fail, g_fb_ok, g_clients);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        g_fb_ok++;

        /* PSRAM DMA 写入后 cache 失效, 否则 CPU 读到旧数据 (内存测试残留模式) */
        esp_cache_msync(fb->buf, fb->len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        /* 诊断: 1 秒限流持续打印帧头 (不依赖串口抓取时机) */
        if (now - g_diag_ms > 1000) {
            g_diag_ms = now;
            ESP_LOGI(TAG, "FB[%d]: len=%u head=%02x%02x %02x%02x %02x%02x %02x%02x (hw=%u)",
                g_fb_ok, (unsigned)fb->len,
                fb->buf[0], fb->buf[1], fb->buf[2], fb->buf[3],
                fb->buf[4], fb->buf[5], fb->buf[6], fb->buf[7],
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        if (g_clients > 0 && fb->format == PIXFORMAT_JPEG)
            ws_broadcast(fb->buf, fb->len);

        esp_camera_fb_return(fb);
        xSemaphoreGive(g_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ═══════════════════════════════════════════════════════════
 *  公开: 初始化
 * ═══════════════════════════════════════════════════════════ */
void cam_stream_init(void)
{
    g_mutex = xSemaphoreCreateMutex();
    xTaskCreate(tcp_server_task, "tcp_srv", 8192, NULL, 1, NULL);
    xTaskCreate(cam_stream_task, "cam_ws", 8192, NULL, 1, NULL);
    ESP_LOGI(TAG, "Ready (raw socket)");
}

void cam_stream_pause(bool pause)
{
    int count;
    portENTER_CRITICAL(&g_pause_lock);
    if (pause) {
        if (g_pause_count < INT_MAX) g_pause_count++;
    } else if (g_pause_count > 0) {
        g_pause_count--;
    }
    count = g_pause_count;
    portEXIT_CRITICAL(&g_pause_lock);
    ESP_LOGI(TAG, "stream %s (ref=%d)",
             count == 0 ? "resumed" : "PAUSED", count);
}

# TTS 语音输出 — 架构设计

> 2026-08-20 定稿。打通「AI 聊天回复」和「绘本朗读」两条链路的最终音频出口。
> 设计稿由队伍提供，本文档整合了审查修订（见 §9）。
>
> **✅ 2026-09-17 现状**: 音频全链可用——SD 槽 J2 飞线已补焊修复，开机音/TTS 恢复出声。
> 验证履历：9-12 开机问候实听出声（有实拍视频留证）、9-15 串口零错误、9-17 修复后实测出声。
> 注意：飞线曾因搬运震断（9-15），演示前通电听开机琶音自检。

## 1. 项目背景与约束

- 硬件平台：ESP32-S3（无内置 DAC），FreeRTOS，PSRAM 8MB
- 音频外设：**板载无任何音频输出电路**（原理图已证实，见 §2）
- 核心矛盾：音频输出与摄像头推流（CPU/PSRAM 竞争）、麦克风录音（声学回声）存在资源冲突

## 2. 硬件连接定义（关键决策）

### 2.1 原理图事实（SCH_ESP32-S3-EYE-MB_20211201_V2.2.pdf，已存 docs/hardware/）

板上音频相关只有 **I2S MEMS 麦克风 MSM261S4030H0R**：

| 信号 | GPIO |
|------|------|
| DMIC_I2S_SCK | IO41 |
| DMIC_I2S_WS | IO42 |
| DMIC_I2S_SDO | IO2 |

**板上没有 NS4150、没有 ES8311、没有喇叭**（官方 S3-EYE 是纯视觉板）。
HARDWARE.md 中「板载功放 NS4150」的记载有误，待修正。

### 2.2 输出路径三选一

| 方案 | 链路 | 成本 | 备注 |
|------|------|------|------|
| **A（推荐）** | I2S1 → MAX98357A 模块 → 3W 喇叭 | ~3 元 | 模块自带 Class-D，免飞线功放 |
| B | I2S1 → ES8311/小 DAC → NS4150 → 喇叭 | 多一颗芯片 | 两颗模拟器件串联，不推荐 |
| C | LEDC PWM → RC 低通 → NS4150/直推小喇叭 → 喇叭 | ~0 元 | 音质 8bit 级，朗读够用 |

- 固件现状：I2S0 已被 mic 占用（新驱动 `driver/i2s_std.h`，`i2s_chan_handle_t`）
- 输出用 **I2S1**（S3 第二 I2S 外设），与 mic 完全独立，无冲突

### 2.3 I2S1 引脚定案：复用 SD 卡槽三根线（2026-08-21 定）

依据官方 BSP 头文件 `esp32_s3_eye.h`（esp-bsp 仓库）：

| I2S1 信号 | GPIO | 原用途 |
|-----------|------|--------|
| BCLK | **39** | SD_CLK |
| WS | **38** | SD_CMD |
| DOUT | **40** | SD_D0 |

**决策理由**：
- 固件没有任何 SD 卡代码，比赛演示不插卡，三根脚物理空闲（高置信）
- 板上唯一确定空闲脚只有 IO14（BSP 未使用），凑不齐 3 根 I2S
- MAX98357A 的 SD_MODE（关断脚）直接接 3V3 常开，不占 GPIO；V1 接受轻微底噪，后续可改接 IO3（LED）做静音控制

**风险**：SD 卡槽引脚与 GPIO 直连（含 10K 上拉），插入 TF 卡时电气冲突 → 演示期间禁止插卡即可（低风险）
**备用**：IO14 + IO1 + IO3（各带不确定性，需实测），仅当 38/39/40 被证明不可用时启用

## 3. FreeRTOS 任务架构

### 3.1 新增任务：voice_task

- 优先级：比 cam_stream_task 高一级（不必顶到 configMAX_PRIORITIES-1，避免抢系统任务）
- 栈大小：8192 bytes（TLS/mbedtls 吃栈，跑稳后按 `uxTaskGetStackHighWaterMark` 水位下调）
- 核心动作：阻塞于 voice_queue → HTTP TTS → 解码 → I2S 播放

### 3.2 核心数据结构

```c
typedef struct {
    char text[512];          // 待合成文本
    uint8_t interrupt;       // 1=强制打断当前播放并清空队列
} voice_msg_t;

QueueHandle_t xVoiceQueue;          // 长度 1，xQueueOverwrite
volatile bool is_voice_playing = false;   // 供 mic_monitor / UI 轮询
```

## 4. 核心 API（对外仅 3 个）

```c
void voice_speak_async(const char *text);  // 非阻塞，队列满则覆盖旧消息
void voice_interrupt(void);                // 清队列 + 立即停止 I2S 播放
void voice_set_volume(uint8_t vol);        // 0-100 软件衰减 PCM
```

## 5. 端到端业务流

**链路 A：孩子提问** — BOOT键 → mic_monitor 录 1.5s WAV → MiMo ASR（已有✅）
→ MiMo Chat（已有✅）→ `voice_speak_async(chat_reply)` → voice_task → HTTP POST MiMo TTS
→ 接收音频流 → 解码 → I2S1 → MAX98357A → 喇叭

**链路 B：绘本朗读** — 摄像头帧 → OCR → 阅读顺序排序（算法已有✅未接入）
→ 循环 `voice_speak_async(句子)` → AI Agent 控制 25 分钟节奏（外部定时触发）

## 6. 冲突解决策略

| 冲突 | 方案 |
|------|------|
| 说话 vs 听（回声） | 播放前拉高 `is_voice_playing`；mic_monitor 录音前轮询，true 则丢弃本次触发。**V1 不做 AEC** |
| 说话 vs 推流 | voice_task 优先级高于 cam_stream_task；推流丢帧保命已实现；PCM 分段读取（每块 512B），不一次性大内存 |
| 新话 vs 旧话 | xVoiceQueue 长度 1 + xQueueOverwrite 覆盖；新消息到达即停止当前播放（i2s 驱动 flush/stop） |

**交互决策（修订）**：播放期间 BOOT 键 = 打断键（停朗读 + 进提问模式）。
原「孩子插嘴打断」依赖播放时继续监听，与回声规避矛盾，V1 砍掉，改用按键。

## 7. 风险与规避

| 风险 | 规避 |
|------|------|
| MiMo TTS 返回格式未知 | 先读 Content-Type；`Accept: audio/wav` 请求头强制 WAV，省解码 CPU |
| 网络带宽紧张（MSS 536 环境） | 上行请求体仅 ~50B；下行音频用 esp_http_client 流式接收（buffer 2048），边收边喂 I2S，不攒全 |
| 外部 DAC/I2S 未验证 | Phase 1 先本地预置「叮咚」PCM 跑通通路，再接 HTTP |
| TLS 栈需求 | 栈 8192 起，水位监控后下调 |

## 8. 实施步骤

1. **P0 硬件哑测**：买 MAX98357A 模块 + 喇叭，接 I2S1（引脚按 §9-6 确认），跑 ESP-IDF i2s 例程放 1kHz 正弦波
2. **P1 Voice 骨架**：voice_task + 队列 + voice_speak_async，仅打日志，验证调度与栈水位
3. **P2 HTTP TTS 接入**：照抄 ASR 的 esp_http_client 代码，文本→MiMo TTS，缓存原始音频
4. **P3 解码播放联动**：P1+P2 合并，「收一包→解码→写一包」流水线
5. **P4 集成互斥**：is_voice_playing + mic_monitor 轮询 + cam_stream 优先级调整

**交付标准**：main.c 调 `voice_speak_async("你好，小朋友")`，喇叭清晰发声，无看门狗复位。

## 9. 审查修订记录（2026-08-20）

1. **NS4150 是模拟输入功放**，不接 I2S；原稿「I2S→DAC→NS4150」双放大且器件混淆（高置信）
2. **MAX98357 本身就是 I2S 功放**，直连喇叭，后面不需 NS4150（高置信）
3. **板载 NS4150 不存在**——官方 V2.2 原理图证实无音频输出电路，需外接（高置信，依据官方 SCH PDF）
4. **NS4150 SD 脚接 GPIO 的要求不成立**——板上没有 NS4150；若未来用方案 B/C 需另留使能脚
5. 栈 6144→8192（TLS 偏紧，中置信）；优先级改为「比 cam 高一级」（系统任务安全，中置信）
6. I2S 引脚规划：mic 已占 IO41/42/2 + I2S0；输出 I2S1 定案为 **38/39/40**（复用 SD 卡三根线，依据 esp-bsp 官方头文件 esp32_s3_eye.h，见 §2.3）
7b. **官方 BSP 明确 BSP_CAPS_AUDIO_SPEAKER = 0**：板无喇叭能力，音频输出必须全外挂（高置信）
7. i2s 新驱动（driver/i2s_std.h）没有 `i2s_zero_dma_buffer`，打断实现需用 `i2s_channel_disable/enable` 或 flush（中置信，实施时验证）

## 10. 实测记录（2026-08-21，P2/P3 完成）

- **MiMo TTS 实测**：POST /v1/chat/completions，model=mimo-v2.5-tts，
  `messages:[{role:"assistant",content:文本}]` + `audio:{format:"wav",voice:"冰糖"}`。
  响应 JSON，base64 WAV 在 `choices[0].message.audio.data`
- **实测音色白名单**（文档里的 default_zh 报错不存在）：mimo_default、冰糖、茉莉、苏打、白桦、Mia、Chloe、Milo、Dean
- **实测音频**：24kHz / mono / 16bit，44B 标准头；6 字≈54KB WAV≈72KB JSON；14 字≈176KB WAV≈236KB JSON，合成+下载 4-5.5 秒
- **固件现状**（main/voice.c + voice.h）：
  - I2S1 = IO39/38/40 @24kHz，voice_task 栈 8K 优先级 3，队列长度 1 overwrite
  - 缓冲 280KB 显式 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`（sdkconfig 无 SPIRAM_USE_MALLOC，标准 malloc 不进 PSRAM）
  - 播放统一栈块中转（不直喂 PSRAM 给 DMA）+ 块间 s_stop 打断 + 音量软件衰减
  - mic_monitor 录音前轮询 voice_is_playing（回声规避已接线）
- **三个坑（已修）**：
  1. RSA 中断耗尽 → `CONFIG_MBEDTLS_HARDWARE_MPI=n`（软件 RSA，握手慢几百 ms 无感）
  2. malloc 不进 PSRAM → TTS 缓冲显式 heap_caps_malloc
  3. 内部堆碎片（esp-aes 失败、ping task 创建失败）→ mbedtls EXTERNAL_MEM_ALLOC 落地 + watchdog ping session 常驻复用（不再每轮 create/delete）
- **稳定性**：连续 3 次冷启动 TTS 全链成功（HTTP 200 → base64 解码 → I2S 播放），watchdog ping 正常
- **待办**：P0 硬件到货听响（voice_test_tone 1kHz 正弦波已就绪）；main.c 有 TODO-P2 临时测试入口（启动 8s 后自动合成一句），验收后删除

### 10.1 更新（2026-08-29，web Codex 版合并 + 全链板上验证）

- **合并后架构**：generation 抢断机制（voice_next_generation / voice_cancelled，HTTP read 循环、解码前、每 512-sample PCM 块边界检查点）+ 响应缓冲 64KB→1MB 动态翻倍（heap_caps_realloc SPIRAM|8BIT）+ wav_parse 严格校验 + json 转义/Unicode 解析 + ASR 套件（1.5s 录音、静音检测 ASR_SILENCE_PEAK=200、3s 截止、HTTP 错误分类、BOOT 中断先打断 TTS 再录音）。凭据保留在 gitignored 的 secrets.h（未采用 Kconfig.projbuild）
- **实测全链**（SW-AES 版）：WiFi 172.20.10.3 → Certificate validated → TTS HTTP 200, 205347B/12.8s → PCM 24000Hz/16bit/153600B → 播放完毕；ASR BOOT 键 → "ASR text: xxx" 8.3s 完成；mic 任务栈 28608/32768（87%，未溢出，值得留意）
- **坑 4（已修）— 硬件 AES 的 DMA bounce 耗尽**：`CONFIG_MBEDTLS_HARDWARE_AES=y` + `MBEDTLS_EXTERNAL_MEM_ALLOC=y`（TLS 缓冲在 PSRAM）时，每条 TLS record 解密需在内部 MALLOC_CAP_DMA 池申请 1600B 中转块（esp_aes_process_dma_ext_ram），但 DMA 池被 WiFi 驱动吃光（32×1600 动态 + 10×1600 静态 rx buffer），实测 `DMA free=980 max=640` → 分配必败 → TTS read 0 字节。**定案：`CONFIG_MBEDTLS_HARDWARE_AES=n`**（软件 AES 全程 PSRAM，零 bounce；TLS 解密 ~10-20MB/s，≤1MB 响应无感）。曾误判 SW-AES 破坏 WPA2 握手——实际是手机热点挂死导致 cc00 掉线（12 次、恰好 10s 一次），热点重启后两版均正常
- **注意**：idf.py 重新生成 sdkconfig 会吞掉手工注释，配置原因以此文档为准；IDF 本地 esp_aes.c 4 处失败点加了诊断日志 patch（失败时打印分配大小 + DMA/内部堆状态）
- **坑 5 后续（8-29 深夜，真根因 + lwip patch）**：分块写后定位到卡死点随机（4096/8192/16384 都卡过），返回 `-26752=MBEDTLS_ERR_SSL_WANT_WRITE`——不是缓冲问题（TCP_SND_BUF 5760→65535 无改善），是 **lwip 初始拥塞窗口 `pcb->cwnd=1`（1×MSS=536B）**：慢启动从 536B 每 RTT 翻倍，干扰环境 RTT~2s 爬到 8KB 需 4-5 RTT，死窗口一撞就停。**Patch（IDF 本地 tcp.c 两处 tcp_connect/tcp_alloc）`cwnd=1→8`**（RFC 3390 IW=8×MSS）：8KB 1 RTT 达、64KB ~4 RTT。另有 sdkconfig：`LWIP_TCP_SND_BUF_DEFAULT/WND_DEFAULT 5760→65535`、`LWIP_TCP_RECVMBOX_SIZE 6→16`。经验：lwip cwnd 以 MSS 为单位；视频流活着是因为持续小帧绕过了爬坡，突发大上传才暴露
- **坑 5 终局（8-30，DMA 池耗尽 + 段池耗尽双瓶颈，已修）**：①**DMA 池耗尽**（主凶）：WiFi 驱动动态 TX buffer 每块 1600B 从 MALLOC_CAP_DMA 池现取，视频流+LCD+I2S+camera 运行后池底剩 ~1KB，<1600B 时分配失败 → lwip ERR_MEM → WANT_WRITE -26752。**修：`DYNAMIC_TX=y` + `CACHE_TX_BUFFER_NUM 32→16` + `DYNAMIC_RX_BUFFER_NUM 32→16`**（省 51.2KB DMA；STATIC_TX 51.2KB 常驻路线死刑——DMA 剩 76B 连 mic_asr 32KB 栈任务都建不出来）。验证：32KB 上传 1s 完成、全链 5.5s OK。**排除清单**：PBUF_POOL_SIZE 16→32（卡点不动）、MEMP_NUM_TCP_SEG 32、SND_BUF/WND 65535、cwnd=8、省电、服务器/网络（curl 同热点 2.65s 200）。②**MEMP_NUM_TCP_SEG=16 耗尽**（次凶，DMA 修复后暴露）：MSS 536（iPhone 热点）下 16 段=在途仅 8.6KB，HTTPS 64KB body 写到 ~32KB 处段结构耗尽死等 ACK（三轮回卡点 32768/36864 吻合；写阶段 timeout 10s/12s 截止/换连接重发/心跳救卡全部工作，失败 380s→53s→35s）。**修：lwipopts.h 覆盖 `MEMP_NUM_TCP_SEG=48`**（25.7KB 在途，~1.3KB RAM）。另：`timeout_ms` 必须建连前生效（SO_SNDTIMEO 建连时写入，open 后 set 无效，codex 指正）；timeout 10→20s 因校园网下 MiMo 响应 >10s 曾假失败（HTTP -1）；ASR 收尾日志含 int/spi/dma 三池 free；DMA 无泄漏（事务后 dma_free 恢复 10888，卡时 2160 是上传中 WiFi 缓冲正常占用）

## 11. 参考资料

- 原理图：docs/hardware/S3EYE_V22_sch.pdf（SCH_ESP32-S3-EYE-MB_20211201_V2.2）
- 官方 BSP 引脚宏：esp-bsp `bsp/esp32_s3_eye/include/bsp/esp32_s3_eye.h`
- MiMo TTS 协议参考：[OpenClaw xiaomi provider](https://raw.githubusercontent.com/openclaw/openclaw/main/docs/providers/xiaomi.md)、[MiMo API 官方文档](https://mimo.mi.com/docs/zh-CN/api/audio/tts)
- 固件 mic 参考实现：main/main.c `do_asr()` + I2S 初始化（i2s_std 新驱动）

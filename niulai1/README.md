# 智瞳·伴读 — AI 儿童阅读陪伴终端

2026 openvela AI 硬件开发者大赛 参赛作品 | 队伍：啊对对队

## 作品简介

把一本普通绘本放在摄像头下，"智瞳·伴读"就能看懂书页内容——孩子可以指着书问"这是什么？"，它会用自然语音回答、朗读、甚至声情并茂地讲故事。屏幕上住着一只"阅读精灵"，它会随着孩子的阅读进度成长升级，让阅读变成一场游戏。旨在为双职工家庭减轻孩子的教育压力。

## 选题方向

**AI 硬件产品创新**：端侧感知 + 云端 AI + 本地交互三位一体的完整硬件产品，
落地图形（LVGL 界面）、AI（MiMo Chat/ASR/TTS 云端链路）、多媒体（摄像头图传 + 音频输出）三项核心能力，
内置阅读时长监测与护眼提醒的"主动 + 执行"场景设计，面向双职工家庭的陪读刚需。
平台情况说明见下方「openvela 说明」。

## 硬件

| 硬件 | 型号 | 状态 |
|------|------|:---:|
| **主控** | ESP32-S3-EYE V2.2（8MB Flash + 8MB PSRAM） | ✅ |
| **摄像头** | OV2640 (DVP) — 板载 | ✅ |
| **麦克风** | I2S MEMS 数字麦克风 — 板载 (I2S0) | ✅ |
| **显示屏** | ST7789 (240×320) — 板载 (SPI) | ✅ |
| **扬声器** | MAX98357A 外接模块 + 3W 喇叭 (I2S1，复用 SD 卡槽引脚) | ✅ |
| **WiFi** | 802.11 b/g/n — 板载 | ✅ |

## 功能

### ✅ 稳定功能（已板上验证，演示可用）

| 功能 | 状态 |
|------|:---:|
| 开机提示音（本地合成，不依赖网络，兼音频链路自检）+ 开机问候 TTS | ✅ |
| 麦克风实时音量条 | ✅ |
| 阅读精灵（LVGL 小狐狸，8 级成长形象 + 升级弹跳动画，情绪状态 Mood 文字显示） | ✅ |
| XP 成长值持久化（NVS，断电平级不丢） | ✅ |
| HTTP 图传（电脑端实时查看画面，5fps 推流 + 弱网自适应） | ✅ |
| WiFi 连接（2.4GHz）+ MiMo TTS 播放链 | ✅ |

> **✅ 音频硬件状态（2026-09-17）**：SD 卡槽 J2 飞线已补焊修复，开机音/TTS 恢复出声。
> 音频全链验证履历：9-12 开机问候实听出声（有实拍视频留证）、9-15 串口零错误、
> 9-17 修复后实测出声。注意：飞线曾因搬运震断（9-15），演示前请轻拿轻放，
> 通电听一下开机琶音即硬件自检。

### ⚠️ 未稳定功能（开发中，不建议演示）

| 功能 | 状态 |
|------|:---:|
| 短按 BOOT → 录音 → ASR 识别 → 云端对话 → TTS 回答 | ⚠️ 曾跑通，有未定位偶发崩溃 |
| 长按 BOOT → 拍照 → 云端 OCR → TTS 朗读 | ⚠️ 链路未完整验证 |
| 阅读时长监测 → 25 分钟护眼提醒 + 眼保健操引导 | ⚠️ 未充分验证 |
| WiFi 健康看门狗 | ⚠️ 未充分验证 |

## 目录结构

```
zhitong-budub/
├── firmware-esp32/              # ESP32-S3-EYE 固件 (ESP-IDF v5.2.2)
│   ├── main/                    # 主程序 + 语音/摄像头/UI/成长系统
│   │   ├── main.c               # 入口 + WiFi/摄像头/按键/ASR/看门狗
│   │   ├── voice.c              # TTS + 开机提示音 + I2S1 输出
│   │   ├── cam_stream.c         # WebSocket JPEG 推流
│   │   ├── chat_client.c        # MiMo Chat API
│   │   ├── ocr_reader.c         # 绘本 OCR
│   │   ├── reading_manager.c    # 阅读状态机 + 25 分钟提醒
│   │   ├── fox_companion.cpp    # 阅读精灵 (LVGL)
│   │   ├── xp_system.cpp        # 成长值持久化
│   │   ├── ui_layout.cpp        # 界面布局
│   │   └── secrets.h.example    # 凭据模板（复制为 secrets.h 填写）
│   ├── components/              # esp_camera + lvgl
│   ├── managed_components/      # esp_jpeg（随包附带，离线也能编译）
│   ├── partitions.csv           # 自定义分区 (2MB app)
│   ├── sdkconfig.defaults       # 关键配置 (软件 AES + MSS 536 图传弱网定案)
│   └── docs/hardware/           # 原理图、接线标注图
├── skills/                      # 大赛 Skill 交付 (×2)
│   ├── reading-companion.md     # 绘本伴读 Skill
│   └── eye-care-reminder.md     # 护眼提醒 Skill
├── logs/                        # AI 开发日志（大赛要求，凭据已脱敏）
│   ├── 智瞳伴读_AI开发日志_20260704-0803.md
│   └── 智瞳伴读_AI开发日志_20260804-0916.md
└── docs/
    ├── HARDWARE.md              # 硬件接线总表
    ├── IO_pinout_table.md       # 引脚复用明细
    ├── TTS_DESIGN.md            # 语音链路设计 (含踩坑记录)
    ├── OPENVELA_MIGRATION.md    # openvela 迁移路线图
    ├── DEFENSE.md               # 答辩要点
    └── test_mimo.py             # MiMo API 电脑侧调试脚本
```

## 运行方式

需要 ESP-IDF v5.2.x（Windows PowerShell 环境，`export.ps1` 后执行）：

```bash
cd firmware-esp32
cp main/secrets.h.example main/secrets.h   # 填入 MiMo key + WiFi
idf.py build                               # 编译
idf.py -p COM3 flash                       # 烧录 (板子接哪个串口填哪个)
```

要点：
- ESP32-S3 只支持 2.4GHz WiFi，热点请固定 2.4GHz 频段（iPhone 开"最大兼容性"）
- `sdkconfig.defaults` 已含关键定案：`CONFIG_MBEDTLS_HARDWARE_AES=n`（软件 AES，
  HW AES + PSRAM TLS 缓冲会耗尽 DMA 池），勿改
- 图传弱网定案（2026-09-16）：MSS 536（环境会周期性丢大 TCP 段）+ WiFi/LWIP 内存
  卸载进 PSRAM + TCP 收发缓冲 16KB + 5fps 限速推流；send 遇 EAGAIN 不再关连接，
  改 5ms 间隔重试（1s 上限）；网页断线 1.5s 自动重连。详见 docs/DEFENSE.md 创新点

## 引脚映射（摘要）

| 外设 | 引脚 |
|------|------|
| 摄像头 OV2640 | SCCB: 4/5, XCLK: 15, PCLK: 13, VSYNC: 6, HREF: 7, D0-7: 11/9/8/10/12/18/17/16 |
| LCD ST7789 (SPI) | SCLK: 21, MOSI: 47, CS: 44, DC: 43, RST: 45 |
| 麦克风 (I2S0) | DIN: 2, BCLK: 41, WS: 42 |
| 扬声器 (I2S1) | BCLK: 39, WS: 38, DOUT: 40（复用 SD 卡槽焊点，见 docs/HARDWARE.md） |
| 按键 | BOOT: GPIO0（短按 ASR / 长按 OCR） |

## 云端

| 项目 | 值 |
|------|-----|
| 端点 | `https://token-plan-cn.xiaomimimo.com/v1/chat/completions` |
| 聊天 | `mimo-v2.5` |
| 语音识别 | `mimo-v2.5-asr` |
| 语音合成 | `mimo-v2.5-tts`（voice: 冰糖） |
| 认证 | `Authorization: Bearer <你的 key>`（key 在 main/secrets.h，勿提交公开仓库） |

## openvela 说明

本仓库固件基于 ESP-IDF（FreeRTOS）实现并已在 ESP32-S3-EYE 真机全链路验证，
作为比赛作品的工程实现与演示载体。openvela (NuttX) 迁移路线图见
`docs/OPENVELA_MIGRATION.md`（含已验证项与待办项）。

## AI Coding 使用说明

本作品全程采用 AI Coding 开发（Claude Code），AI 参与需求拆解、方案设计、编码、调试与文档撰写全流程：

| 环节 | 协作方式 |
|------|---------|
| 方案设计 | AI 产出语音链路（单写者队列 + 代际抢断）、图传弱网策略、成长系统数据模型等设计（见 docs/） |
| 编码 | AI 编写驱动与业务代码，人工编译、烧录与真机实测反馈 |
| 调试 | AI 参与疑难排障：硬件 AES 的 DMA 池耗尽定位、lwip 慢启动致大包卡死、视频流弱网五件套对策、音频飞线 A/B 测试定位 |
| 文档 | 全部技术文档、答辩材料、README 由 AI 起草迭代 |

交付物：
- **大赛 Skill ×2**（skills/ 目录）：reading-companion（绘本伴读场景）、eye-care-reminder（护眼提醒场景），Markdown 形式、可复用
- **AI Coding 完整对话日志 ×2**（logs/ 目录）：2026-07-04 ~ 2026-08-03、2026-08-04 ~ 2026-09-16，含逐日开发摘要与关键会话节选，凭据已脱敏

AI 对开发效率的实际帮助：排障类问题多数一次定位根因（如上表调试环节案例），
"烧录—实测—反馈"循环显著提速，使 4 名大一学生在两个多月内完成
摄像头 / 屏幕 / 麦克风 / 功放 / WiFi / 云端 AI 六条链路的真机打通。

## 已知问题

1. 演示前重启手机热点：热点抽风会导致 TTS/ASR 超时（DNS/TLS 随机失败）
2. ASR 栈占用高（49KB 专用任务栈），勿在 do_asr 路径加大局部数组
3. 音频走 SD 卡槽飞线（I2S1，TF 卡槽 J2 焊点），曾因搬运震断（9-15，9-17 已补焊修复）——
   演示前听一下开机琶音自检；若现场再断，跳过音频环节即可（其余功能不受影响）

## 团队

- 队伍名称: 啊对对队
- 成员: 古苑婷、陈子宁、陈心妍、梁敏

## 开源协议

Apache 2.0（见 LICENSE）

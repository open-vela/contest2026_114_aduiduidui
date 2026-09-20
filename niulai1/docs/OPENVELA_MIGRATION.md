# openvela 迁移作战手册

> 2026-09-01 定。目标：9-20 前把智瞳伴读迁到 openvela + ai_agent 并完成大赛提交。
> 依据：官方 docs 仓 dev-ai-contest-2026 分支（本地克隆于 /tmp/openvela-docs，GitCode 镜像）。

## 0. 判定标准（为什么必须迁）

《大赛总览》第 119 行：

> 基于 openvela 开发的判定标准：项目须使用 openvela 开源项目（NuttX 内核仓库除外）
> 提供的系统能力，且至少落地图形、AI、多媒体三项核心能力之一。

ESP-IDF 版纯 FreeRTOS 不满足 → 必须迁。官方已铺路：

- vendor_espressif 官方 esp32s3-eye BSP：摄像头 /dev/video0、LCD /dev/lcd0（LVGL 可跑）、
  麦克风 /dev/audio/pcm_in0、WiFi、SD、按键全驱动
- ai_agent 官方预置 esp32s3-eye defconfig + fix_esp32s3.sh 补丁脚本
- ai_agent 原生吃 MiMo key：`router_set mimo <key>`
- mini_memo 官方 LVGL 应用样板：voice_channel（PTT+ASR）、velaclaw Client（LLM）、
  cron_service（定时提醒）——狐狸 UI 移植照它抄

## 1. Ubuntu 22.04 环境搭建（今晚做，约 3-5 小时）

> 官方明说不支持 WSL/Docker 编译。Ubuntu 实机，要求 16GB RAM + 40GB 磁盘。

```bash
# 1.0 环境检查
free -h && df -h ~

# 1.1 依赖
sudo apt update
sudo apt install git curl cmake python3 libc++abi-dev build-essential

# 1.2 Git LFS（不装会拉到损坏的指针文件）
curl -s https://packagecloud.io/install/repositories/github/git-lfs/script.deb.sh | sudo bash
sudo apt-get install git-lfs && git lfs install

# 1.3 repo 工具（国内直连 googleapis 不通，用清华镜像）
curl -sSL "https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/repo" > repo
chmod +x repo && sudo mv repo /usr/local/bin

# 1.4 git 身份（用报名时的 GitHub 账号！日志归属校验用）
git config --global user.name "YuantingGu"
git config --global user.email "<报名时的邮箱>"
```

## 2. 拉全量源码（今晚挂机，1-3 小时）

```bash
mkdir -p ~/openvela && cd ~/openvela

# manifest 仓库 = 咱们专属仓本身（内含队伍 xml + linkfile 自引用）
# GitCode 国内直连；失败换 Gitee（gitee.com/open-vela/manifests.git）
repo init -u https://gitcode.com/open-vela/contest2026_114_aduiduidui.git \
    -b dev-ai-contest-2026 -m contest2026_114_aduiduidui.xml \
    --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs

repo sync -c -j8   # 中断可重复执行增量续传
```

注意事项：
- 工具链仓 `openvela-toolchain-external/prebuilts_gcc_linux-x86_64_xtensa-esp32s3-elf`
  的 remote 指向 `../`（GitCode 上未必有镜像）。若 sync 卡它：
  单独 `git clone https://github.com/open-vela-toolchain-external/...` 塞进对应路径，或开代理重试。
- 全量 sync 几百个仓。若磁盘紧张可只留 esp32s3 相关（不推荐，模拟器也要用）。

## 3. Ubuntu 侧装 ESP-IDF（Linux 版，构建需要）

```bash
cd ~
git clone -b v5.2.2 --recursive https://gitee.com/EspressifSystems/esp-idf.git
# gitee 镜像没有则用 github.com/espressif/esp-idf + 代理，或 esp-gitee-tools
cd esp-idf && ./install.sh esp32s3 && source export.sh
```

## 4. 编译 ai_agent 固件（目标：nuttx/nuttx.bin）

依据官方 ai_agent_quickstart 六.1（esp32s3-eye 章节）：

```bash
cd ~/openvela
cp packages/ai_agent/defconfigs/esp32s3-eye/esp32s3-eye_defconfig \
   nuttx/boards/xtensa/esp32s3/esp32s3-eye/configs/ai_agent/defconfig

source ~/esp-idf/export.sh
export CCACHE_DISABLE=1

./build.sh esp32s3-eye:ai_agent distclean
bash packages/ai_agent/fix_esp32s3.sh &   # 补丁脚本，编译期间后台常驻
./build.sh esp32s3-eye:ai_agent
```

成功标志：`Generated: nuttx.bin`。
已知可忽略：`ccache: error: execute_noreturn`、`expr: syntax error`。
OOM：`./build.sh ... -j2`。

## 5. 烧录（回 Windows 做）

```powershell
# Windows 侧，ESP-IDF 环境里 esptool 已有
esptool --chip esp32s3 --port COM3 --baud 460800 `
    --before default-reset --after hard-reset write-flash 0x0 nuttx\nuttx.bin
```

首次启动：USB-CDC 115200 8N1 → `nsh>` → `ai_agent` → vela> 配网配 key：
```
set_wifi <ssid> <pass>
router_set mimo <key>
ask 你好
```

## 6. 开发路线（不依赖真机部分在 Windows 侧直接写）

| # | 工作 | 依赖 | 状态 |
|---|------|------|------|
| 1 | 环境+源码+编译 nuttx.bin | Ubuntu | 今晚 |
| 2 | QEMU 模拟器跑 ai_agent（goldfish-arm64-v8a-ap） | 步骤 1 | 板子修好前的主开发台 |
| 3 | 狐狸 LVGL 应用 app/zhitong_fox/（照 mini_memo 骨架） | 无（纯代码） | 可在 Windows 写 |
| 4 | 自定义伴读 Skill（/data/agent/skills/*.md） | 无（纯文本） | ✅ 9-12 完成（skills/ ×2） |
| 5 | cron_service 25 分钟阅读提醒（定时主动场景） | 步骤 3 | |
| 6 | MAX98357A 音频输出板级扩展（官方 BSP 缺口，差异化） | 真机 | ✅ ESP-IDF 侧 9-12/9-17 板上验证出声（9-17 飞线补焊修复）；openvela 侧待真机 |
| 7 | 烧录验证 + 演示视频 + README + logs/ + zip | 全链 | 9-20 前 |

## 7. 提交清单（9-20 截止）

- 专属仓：app/zhitong_fox/（manifest linkfile → packages/demos/contest2026_114_zhitong_fox）
- skills/：自定义 Agent Skill ×2（reading-companion / eye-care-reminder，满足"至少一个 Skill"要求）
- logs/：8-9 月 AI 日志（本地 JSONL 导出转 md）
- README.md 重写为作品说明
- 演示视频 ≤5 分钟 + 作品介绍 docx/pdf/pptx
- CLA 签署（首次 PR 时 /check-cla）
- 飞书表单 zip 命名 `<队伍名称>-<作品名称>-<仓库名称>.zip`

## 8. 已知坑（提前记）

- defconfig 改了不生效 → rm -rf cmake_out/ 或 menuconfig 改
- LVGL 非线程安全 → 非 LVGL 线程用 lv_async_call()
- mini_memo STACKSIZE=40960（LLM 调用吃栈）
- 火山引擎 ASR 需另配 key（ai_agent 语音走火山，非 MiMo）
- MiMo key 9/20 前轮换（源码公开后旧 key 作废）

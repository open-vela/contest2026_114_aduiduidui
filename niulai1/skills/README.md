# skills/ — 智瞳·伴读 自定义 Agent Skill

满足大赛"至少使用一个 Skill"要求（共 2 个，超出最低要求）。

| Skill | 文件 | 场景 |
|-------|------|------|
| 绘本伴读 | reading-companion.md | 核心交互：识别绘本 → 儿童化讲解 → TTS 朗读 → 精灵联动 |
| 护眼提醒 | eye-care-reminder.md | 主动关怀：25 分钟阅读后提醒休息 + 眼保健操引导 |

## 部署（openvela 真机，迁移中）

> 当前固件在 ESP32-S3-EYE 上基于 ESP-IDF 实现；openvela 环境尚未搭建，
> 以下为迁移后的部署用法（见 docs/OPENVELA_MIGRATION.md 路线图）。

Skill 为纯文本 markdown，放置到板载 Agent 技能目录即生效：

```sh
# 通过 USB-CDC 串口（nsh）推送到板上
nsh> cp /mnt/skills/reading-companion.md /data/agent/skills/
nsh> cp /mnt/skills/eye-care-reminder.md /data/agent/skills/
```

启动 ai_agent 后即可在对话中触发（无需重启板子，新增/修改技能热加载）。

## 验证方式

```sh
vela> ask 给我讲讲这本书          # 触发 reading-companion
vela> ask 今天读了多久            # 触发 eye-care-reminder
```

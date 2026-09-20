/**
 * xp_system.h — 经验值与等级成长系统
 * =====================================
 * 智瞳伴读点读笔 — 阅读精灵成长逻辑
 *
 * 设计:
 *   - 全静态变量, 零 malloc, 无内存碎片
 *   - 升级时自动联动 fox_companion 的 update_fox_level()
 *   - 线程安全: FreeRTOS 临界区保护 add_xp()
 */

#ifndef XP_SYSTEM_H
#define XP_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 成长常量 ── */
#define XP_LEVEL_MAX         7       /**< 最高等级 (对应狐狸 0~7)      */
#define XP_PER_LEVEL        200      /**< 每级所需经验值               */
#define DIALOG_XP            10      /**< 一次有效对话获得的经验        */

/* ── 公共 API ── */

/**
 * 初始化经验系统。
 * 从 NVS (命名空间 zhitong) 恢复上次的等级/XP; 无记录时回退 Lv.0/0。
 * 应在固件启动时调用一次。
 */
void xp_system_init(void);

/**
 * 增加经验值。
 * - 累加到 current_xp
 * - 若累积 >= XP_PER_LEVEL 且未满级, 自动升级
 * - 升级时自动调用 update_fox_level(new_level)
 * - 满级(7)后不再累加
 *
 * @param xp  获得的经验值 (通常为 DIALOG_XP)
 */
void add_xp(int xp);

/**
 * 获取当前等级 (0~7)。
 */
int  xp_get_level(void);

/**
 * 获取当前经验值 (0 ~ XP_PER_LEVEL-1)。
 */
int  xp_get_current(void);

/**
 * 获取满级所需经验值。
 */
int  xp_get_max(void);

/**
 * 是否已满级。
 * @return 1=满级, 0=未满
 */
int  xp_is_maxed(void);

/**
 * 直接设置等级 (跳过 XP 累积, 用于 demo/调试)。
 * 会自动联动 update_fox_level()。
 */
void xp_set_level(int level);

/**
 * 同时设置等级和经验值 (不触发升级动画, 仅更新内部状态)。
 * 用于外部逻辑自行管理等级/XP 时同步到 UI。
 * 若 level 与当前不同则自动联动 update_fox_level()。
 */
void xp_set_state(int level, int xp);

#ifdef __cplusplus
}
#endif

#endif /* XP_SYSTEM_H */

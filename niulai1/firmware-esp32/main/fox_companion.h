/**
 * 智瞳伴读 — 小狐狸陪伴精灵 (Fox Companion)
 * =============================================
 * 面向幼儿的 8 阶段成长型角色系统。
 * 目标平台: ESP32-S3-EYE (ST7789 240x240, LVGL v8, ESP-IDF)
 *
 * 设计理念:
 *   - 毛绒可爱风 (fluffy kawaii style)
 *   - 8 阶段逐级解锁外观配件 (星星→披风→书本→王冠→光芒)
 *   - 每阶段切换时触发 LVGL 缩放弹跳动画 (bounce)
 *   - 内存优化: 所有图像存 Flash (.rodata), LVGL 引用无需拷贝
 *
 * 参考图像:
 *   基础狐: 橙毛 / 圆脸 / 大黑瞳高光眼 / 粉色内耳 / 白吻 / 黑鼻
 */

#ifndef FOX_COMPANION_H
#define FOX_COMPANION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* ================================================================
 *  1. 8 阶段枚举: FoxLevel
 * ================================================================ */
typedef enum {
    FOX_LV_0 = 0,   /**< 阶段0: 初始光秃秃的小狐狸 (基础形态)               */
    FOX_LV_1 = 1,   /**< 阶段1: 手捧金色星星的小狐狸                        */
    FOX_LV_2 = 2,   /**< 阶段2: 身披黄色小披风的小狐狸                      */
    FOX_LV_3 = 3,   /**< 阶段3: 手捧一本翻开的童话书的小狐狸                 */
    FOX_LV_4 = 4,   /**< 阶段4: 披风 + 星星 (披风+手里捧星)                   */
    FOX_LV_5 = 5,   /**< 阶段5: 披风 + 书本 (披风+手捧童话书)                 */
    FOX_LV_6 = 6,   /**< 阶段6: 戴着小小阅读王冠的学霸狐狸                    */
    FOX_LV_7 = 7,   /**< 阶段7: 王冠+披风+书本+周身金光                      */
    FOX_LEVEL_MAX = 8
} FoxLevel;

/* ================================================================
 *  2. 情绪状态枚举
 * ================================================================ */
typedef enum {
    FOX_EMOTE_IDLE      = 0,   /**< 待机: 微笑眨眼                           */
    FOX_EMOTE_HAPPY     = 1,   /**< 开心: 眯眼张嘴笑, 星星眼                  */
    FOX_EMOTE_SURPRISED = 2,   /**< 惊讶: 圆眼大睁, 嘴巴 O 形                 */
    FOX_EMOTE_READING   = 3,   /**< 读书: 眼睛向下看, 带专注神态              */
    FOX_EMOTE_SLEEPY    = 4,   /**< 犯困: 半闭眼, 打哈欠                      */
    FOX_EMOTE_PROUD     = 5,   /**< 骄傲/庆祝: 挺胸, 闪光                     */
    FOX_EMOTE_CURIOUS   = 6,   /**< 好奇: 歪头, 问号气泡                      */
    FOX_EMOTE_ENCOURAGE = 7,   /**< 鼓励: 回答失败/休息后继续                  */
    FOX_EMOTE_MAX       = 8
} FoxEmotion;

typedef enum {
    FOX_EVENT_IDLE = 0,
    FOX_EVENT_READING_START,
    FOX_EVENT_OCR_SUCCESS,
    FOX_EVENT_QUESTION,
    FOX_EVENT_ANSWER_SUCCESS,
    FOX_EVENT_ANSWER_ERROR,
    FOX_EVENT_BREAK,
    FOX_EVENT_BREAK_DONE,
} FoxEvent;

/* ================================================================
 *  3. FoxState 结构体 — 单例, 栈内/静态分配
 * ================================================================ */
typedef struct {
    FoxLevel        level;                  /**< 当前等级 0~7                */
    FoxEmotion      emotion;                /**< 当前情绪                    */
    lv_obj_t       *img_obj;               /**< LVGL 图像对象指针          */
    lv_anim_t       anim;                  /**< 复用的动画结构 (防碎片)     */
    bool            anim_busy;             /**< 动画进行中标志              */
    uint32_t        last_update_ms;        /**< 上次刷新时间戳 (ms)         */
    const char     *level_name;            /**< 当前等级名称 (仅指针,不分配) */
} FoxState;

/* ================================================================
 *  4. 8 个 lv_img_dsc_t 声明 — 像素体在 fox_images.h
 * ================================================================ */
extern const lv_img_dsc_t img_fox_level_0;
extern const lv_img_dsc_t img_fox_level_1;
extern const lv_img_dsc_t img_fox_level_2;
extern const lv_img_dsc_t img_fox_level_3;
extern const lv_img_dsc_t img_fox_level_4;
extern const lv_img_dsc_t img_fox_level_5;
extern const lv_img_dsc_t img_fox_level_6;
extern const lv_img_dsc_t img_fox_level_7;

/** 快速索引表 (FoxLevel → lv_img_dsc_t*) — O(1) 查图, 零分配 */
extern const lv_img_dsc_t *const g_fox_img_table[8];

/* ================================================================
 *  5. 公共 API
 * ================================================================ */

/**
 * 初始化狐狸精灵界面。
 * - 创建 LVGL image 对象并居中放置在活动屏幕上
 * - 设置默认等级 FOX_LV_0, 默认情绪 FOX_EMOTE_IDLE
 * - 分配最小内存, 复用动画结构
 *
 * @param parent  父容器, 传 NULL 则默认 lv_scr_act()
 */
void lv_fox_init(lv_obj_t *parent);

/**
 * 切换狐狸等级 (0~7)。
 * - 自动更新图像源
 * - 触发缩放弹跳动画 (0.5→1.15→1.0, 300ms)
 * - 动画期间阻塞重复触发 (anim_busy 互斥)
 *
 * @param level  0~7, 超出范围自动 clamp
 */
void update_fox_level(int level);

/**
 * 切换狐狸情绪 (不换图, 仅状态标记)。
 * 后续可与 LVGL 动画组合实现眨眼/歪头等效果。
 *
 * @param emotion  FOX_EMOTE_*
 */
void fox_set_emotion(FoxEmotion emotion);

/** 根据业务事件驱动 8 状态情绪机；只改状态，不直接调用 LVGL。 */
void fox_handle_event(FoxEvent event);

/** 返回稳定的英文状态名，供 UI/日志显示。 */
const char *fox_get_emotion_name(void);

/**
 * 获取当前等级名称 (只读字符串)。
 * @return 如 "LV_0 初始狐" / "LV_7 究极狐" 等
 */
const char *fox_get_level_name(void);

/**
 * 获取全局 FoxState 指针 (只读, 供外部查询状态)。
 */
const FoxState *fox_get_state(void);

/**
 * 手动触发一次缩放弹跳动画 (不切换等级)。
 * 可用于升级以外的场景 (如: 用户摸头, 答题正确等)。
 */
void fox_play_bounce(void);

#ifdef __cplusplus
}
#endif

#endif /* FOX_COMPANION_H */

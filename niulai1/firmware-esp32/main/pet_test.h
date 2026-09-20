/**
 * pet_test.h — 宠物逻辑测试 (纯 C, 非阻塞)
 * ===========================================
 * 模拟经验积累、等级升级、形态切换。
 * 在主循环中不断调用 pet_logic_test_update() 即可。
 */

#ifndef PET_TEST_H
#define PET_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 每帧调用一次 (非阻塞)。
 * 内部用时间戳判断, 每 1000ms 自动 +10 XP,
 * XP >= 200 时升级并换狐狸图, 等级 0~7 循环。
 */
void pet_logic_test_update(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_TEST_H */

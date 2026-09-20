/**
 * test_demo.h — 测试/演示模块
 * =============================
 * 集中管理所有临时调试代码, 正式发布时删掉此文件即可
 */

#ifndef TEST_DEMO_H
#define TEST_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 演示: LV_1→LV_8 循环切换, 每 1.5s 切换一次
 * 阻塞运行, 放在 app_main 最后调用
 */
void test_fox_cycle_demo(void);

#ifdef __cplusplus
}
#endif

#endif

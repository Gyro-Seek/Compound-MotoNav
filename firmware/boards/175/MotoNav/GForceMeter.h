#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// 板载 QMI8658 → 设备自身 G 值状态机。
// GForceImu.h 负责采样、去重力、投影成水平两轴（单位 1/1000 g，毫 g），
// 再调用 feed() 把 (lat, lon) 灌进来。本类纯数学、无 Arduino/Wire 依赖：
// 固件绘制与单元测试共用同一份逻辑。
//
// 表情等级（随合成 |G| 增大递进，减小时相反，带迟滞防抖）：
//   0 CALM    微笑        1 GRIN     张大嘴笑
//   2 BOUNCE  蹦蹦跳跳    3 EXCITED  兴奋
//   4 DIZZY   眩晕        5 SPIN     转圈
class GForceMeter {
public:
  static constexpr int LEVELS = 6;
  static constexpr float MAX_G = 1.30f;      // 雷达满量程
  static constexpr int LIMIT_MG = 4000;      // 单轴输入限幅（±4g）
  // 注意：不能叫 STALE_MS —— 全局 FirmwareConfig.h 已有同名宏（6000），会被展开成数字常量。
  static constexpr uint32_t GF_STALE_MS = 1200; // 超过这么久没采样就衰减回 0

  // 从 level i 升到 i+1 的 |G| 阈值 / 从 level i 降到 i-1 的阈值（迟滞带）。
  // 用函数内常量而非类内数组成员：兼容 C++11/17，不需要类外定义。
  static float upThreshold(int i) {
    static constexpr float v[LEVELS] = {0.12f, 0.30f, 0.52f, 0.78f, 1.05f, 3.0f};
    return v[i < 0 ? 0 : (i >= LEVELS ? LEVELS - 1 : i)];
  }
  static float downThreshold(int i) {
    static constexpr float v[LEVELS] = {0.00f, 0.09f, 0.26f, 0.47f, 0.72f, 0.96f};
    return v[i < 0 ? 0 : (i >= LEVELS ? LEVELS - 1 : i)];
  }

  float gx = 0, gy = 0;                       // 平滑后的横向 / 纵向 G
  int level = 0;
  uint32_t lastFed = 0;                       // 最后一次有效采样的时间
  bool everFed = false;

  // 板载 IMU 采样入口：lat/lon 为毫 g。越界值忽略，不影响当前状态。
  bool feed(int latMilliG, int lonMilliG, uint32_t now) {
    if (latMilliG < -LIMIT_MG || latMilliG > LIMIT_MG ||
        lonMilliG < -LIMIT_MG || lonMilliG > LIMIT_MG) return false;
    // 指数平滑（约 25Hz 采样时 0.35 的跟随速度）
    const float a = 0.35f;
    gx += (latMilliG / 1000.0f - gx) * a;
    gy += (lonMilliG / 1000.0f - gy) * a;
    lastFed = now; everFed = true;
    return true;
  }

  // 每帧调用：采样中断衰减 + 等级迟滞更新
  void tick(uint32_t now) {
    if (everFed && (uint32_t)(now - lastFed) > GF_STALE_MS) {
      gx *= 0.82f; gy *= 0.82f;
      if (fabsf(gx) < 0.004f) gx = 0;
      if (fabsf(gy) < 0.004f) gy = 0;
    }
    float m = magnitude();
    if (level < LEVELS - 1 && m > upThreshold(level)) ++level;
    else if (level > 0 && m < downThreshold(level)) --level;
  }

  float magnitude() const { return sqrtf(gx * gx + gy * gy); }

  // 雷达里的红色实心小球坐标。cx,cy = 雷达圆心，r = 雷达半径。
  // 方向取 (gx,gy)，大小按 |G| / MAX_G 归一并夹到圆内。
  void ball(float cx, float cy, float r, float &bx, float &by) const {
    float m = magnitude();
    if (m < 1e-5f) { bx = cx; by = cy; return; }
    float k = m / MAX_G;
    if (k > 1.0f) k = 1.0f;
    float reach = r * 0.84f * k;
    bx = cx + gx / m * reach;
    by = cy - gy / m * reach;
  }

  // "0.85G"；buf 至少 8 字节
  void text(char *buf, size_t n) const {
    float m = magnitude();
    if (m > 9.99f) m = 9.99f;
    snprintf(buf, n, "%.2fG", m);
  }

  // 复位（离开 G 页 / 重新进页面时）
  void reset() { gx = 0; gy = 0; level = 0; everFed = false; lastFed = 0; }
};

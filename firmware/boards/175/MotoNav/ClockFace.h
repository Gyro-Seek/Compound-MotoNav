#pragma once
// iOS 时钟 App（世界时钟）模拟表盘的指针角度计算。
// 纯数学、无 Arduino 依赖：固件绘制与单元测试共用同一份逻辑。
#include <math.h>

namespace ClockFace {
struct Hands {
  float hourDeg;      // 0 = 12 点，顺时针，单位：度
  float minuteDeg;
  float secondDeg;
};

// fraction = 当前秒内的 0~1 小数（做扫秒动画时用；不做动画就传 0）。
inline Hands hands(int hour, int minute, int second, float fraction = 0.0f) {
  float s = (float)second + fraction;
  if (s < 0.0f) s = 0.0f;
  float m = (float)minute + s / 60.0f;
  float h = (float)(hour % 12) + m / 60.0f;
  Hands out;
  out.secondDeg = s * 6.0f;             // 60 秒 = 360 度
  out.minuteDeg = m * 6.0f;             // 60 分 = 360 度
  out.hourDeg   = h * 30.0f;            // 12 小时 = 360 度
  return out;
}

// 由圆心、半径、角度求表盘上的点；r 允许为负（指针尾部配重端在中心另一侧）。
inline void tip(float cx, float cy, float r, float deg, float &x, float &y) {
  // 不能命名为 PI：Arduino.h 里有 #define PI，会把变量名替换成字面量。
  const float kDegToRad = 3.14159265358979323846f / 180.0f;
  float a = deg * kDegToRad;
  x = cx + sinf(a) * r;
  y = cy - cosf(a) * r;
}
}  // namespace ClockFace

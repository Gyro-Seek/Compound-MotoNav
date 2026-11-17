#ifdef NDEBUG
#undef NDEBUG
#endif
// GForceMeter.h 是纯数学模块（无 Arduino 依赖），固件与单测共用同一份逻辑。
// 覆盖：feed 限幅、指数平滑、等级迟滞、采样中断衰减、雷达球方向与夹取、文本、复位。
// 数据来源为板载 QMI8658 经 GForceImu.h 处理后的毫 g 值，不再解析任何命令。
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../boards/175/MotoNav/GForceMeter.h"

static bool nearf(float a, float b, float eps = 1e-4f) { return fabsf(a - b) < eps; }

// 连续喂同一个值，让指数平滑收敛。
static void feed(GForceMeter &m, int lat, int lon, uint32_t t0, int n, uint32_t step = 50) {
  for (int i = 0; i < n; i++) m.feed(lat, lon, t0 + (uint32_t)i * step);
}

int main() {
  // ---- 初始状态 ----
  GForceMeter m;
  assert(m.gx == 0 && m.gy == 0 && m.level == 0 && m.lastFed == 0 && !m.everFed);
  assert(nearf(m.magnitude(), 0));

  // ---- 越界一律拒绝，且不改动任何状态 ----
  const int bad[][2] = {{5000, 1}, {1, -5000}, {-4001, 0}, {0, 4001}, {1234567, 1}};
  for (const auto &v : bad) assert(!m.feed(v[0], v[1], 100));
  assert(m.gx == 0 && m.gy == 0 && m.level == 0 && m.lastFed == 0 && !m.everFed);

  // ---- 合法值：符号、边界 ----
  assert(m.feed(-320, 150, 1000));
  assert(m.everFed && m.lastFed == 1000);
  assert(nearf(m.gx, -0.32f * 0.35f));        // 一次平滑，a = 0.35
  assert(nearf(m.gy,  0.15f * 0.35f));
  assert(m.feed(4000, -4000, 1010));          // 上下限都接受
  assert(m.feed(-4000, -4000, 1020));
  assert(m.feed(0, 0, 1030));

  // ---- 平滑收敛 ----
  GForceMeter c;
  feed(c, 1000, 0, 1000, 60);
  assert(nearf(c.gx, 1.0f, 1e-3f) && nearf(c.gy, 0, 1e-3f));
  assert(nearf(c.magnitude(), 1.0f, 1e-3f));

  // ---- 雷达球：静止在圆心 ----
  float bx = 0, by = 0;
  GForceMeter z;
  z.ball(100, 100, 40, bx, by);
  assert(nearf(bx, 100) && nearf(by, 100));

  // ---- 雷达球：横向 G 向右、纵向 G 向上（屏幕 y 轴向下）----
  GForceMeter r; feed(r, 1000, 0, 1000, 40);
  r.ball(100, 100, 40, bx, by);
  assert(bx > 100 + 20 && nearf(by, 100, 0.5f));
  GForceMeter u; feed(u, 0, 1000, 1000, 40);
  u.ball(100, 100, 40, bx, by);
  assert(nearf(bx, 100, 0.5f) && by < 100 - 20);
  GForceMeter l; feed(l, -1000, 0, 1000, 40);
  l.ball(100, 100, 40, bx, by);
  assert(bx < 100 - 20 && nearf(by, 100, 0.5f));
  GForceMeter d; feed(d, 0, -1000, 1000, 40);
  d.ball(100, 100, 40, bx, by);
  assert(nearf(bx, 100, 0.5f) && by > 100 + 20);

  // ---- 雷达球：距离随 |G| 增大；超过 MAX_G 夹到圆内 ----
  GForceMeter small; feed(small, 300, 0, 1000, 40);
  small.ball(100, 100, 40, bx, by);
  const float dSmall = fabsf(bx - 100);
  r.ball(100, 100, 40, bx, by);
  const float dBig = fabsf(bx - 100);
  assert(dBig > dSmall);
  GForceMeter huge; feed(huge, 4000, 4000, 1000, 60);
  huge.ball(100, 100, 40, bx, by);
  const float dHuge = sqrtf((bx - 100) * (bx - 100) + (by - 100) * (by - 100));
  assert(dHuge <= 40 * 0.84f + 1e-2f);

  // ---- 文本 ----
  char t[8];
  z.text(t, sizeof(t)); assert(std::strcmp(t, "0.00G") == 0);
  c.text(t, sizeof(t)); assert(std::strcmp(t, "1.00G") == 0);
  huge.gx = 20; huge.gy = 0;
  huge.text(t, sizeof(t)); assert(std::strcmp(t, "9.99G") == 0);  // 上限夹取

  // ---- 阈值迟滞带单调（上升阈值 > 下一级的下降阈值）----
  for (int i = 0; i < GForceMeter::LEVELS - 1; i++)
    assert(GForceMeter::upThreshold(i) > GForceMeter::downThreshold(i + 1));

  // ---- 等级：大 G 逐级升到 5，再逐级降回 0 ----
  GForceMeter lv;
  lv.tick(1000);
  assert(lv.level == 0);
  for (int i = 0; i < 20; i++) {
    lv.feed(1300, 0, 10000 + (uint32_t)i * 50);
    lv.tick(10000 + (uint32_t)i * 50);
  }
  assert(lv.level == GForceMeter::LEVELS - 1);
  for (int i = 0; i < 40; i++) {
    lv.feed(0, 0, 20000 + (uint32_t)i * 50);
    lv.tick(20000 + (uint32_t)i * 50);
  }
  assert(lv.level == 0);
  assert(nearf(lv.gx, 0, 1e-3f));

  // 迟滞：在阈值带内来回微调不应反复跳级。
  GForceMeter hy;
  for (int i = 0; i < 60; i++) {
    hy.feed(i % 2 ? 280 : 270, 0, 30000 + (uint32_t)i * 50);
    hy.tick(30000 + (uint32_t)i * 50);
  }
  const int h0 = hy.level;
  for (int i = 0; i < 10; i++) {
    hy.feed(i % 2 ? 280 : 270, 0, 33000 + (uint32_t)i * 50);
    hy.tick(33000 + (uint32_t)i * 50);
  }
  assert(hy.level == h0);

  // ---- 采样中断衰减：1200ms 没数据后 gx 逐步归零 ----
  GForceMeter s;
  feed(s, 1000, 0, 1000, 40);                // lastFed = 1000 + 39*50 = 2950
  const float before = s.gx;
  s.tick(2950 + GForceMeter::GF_STALE_MS + 1);
  assert(s.gx < before && s.gx > 0);
  for (int i = 0; i < 120; i++) s.tick(5000 + (uint32_t)i * 100);
  assert(s.gx == 0 && s.gy == 0);

  // ---- 复位 ----
  c.reset();
  assert(c.gx == 0 && c.gy == 0 && c.level == 0 && c.lastFed == 0 && !c.everFed);

  std::puts("PASS: feed limits, smoothing, hysteresis levels, stale decay, ball direction/clamp, text, reset");
}

#ifdef NDEBUG
#undef NDEBUG
#endif
// GForceImu 纯数学层：去重力 + 自适应水平基投影。
// 关键性质：静止恒 0、任意安装倾角自动对齐、竖直颠簸被投影掉、
// 持续过弯的稳态 G 不被低通吃掉（门控冻结）、限幅与异常降级。
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include "../boards/175/MotoNav/GForceImu.h"

static bool nearf(float a, float b, float eps = 1e-3f) { return fabsf(a - b) < eps; }
static int magOf(int lat, int lon) { return (int)lroundf(sqrtf((float)lat * lat + (float)lon * lon)); }

int main() {
  int lat = -1, lon = -1;

  // ---- 1) 首帧只播种：返回 false、输出 0、基线归一化到 1g ----
  GForceImu a;
  assert(!a.primed && !a.valid);
  assert(!a.feed(0.6f, 0.0f, 0.8f, lat, lon));
  assert(lat == 0 && lon == 0 && a.primed && !a.valid);
  assert(nearf(a.gLP[0], 0.6f) && nearf(a.gLP[1], 0.0f) && nearf(a.gLP[2], 0.8f));
  assert(nearf(sqrtf(a.gLP[0] * a.gLP[0] + a.gLP[1] * a.gLP[1] + a.gLP[2] * a.gLP[2]), 1.0f));

  // ---- 2) 水平放置静止 (0,0,1)：连续喂都输出 0 ----
  GForceImu b;
  assert(!b.feed(0, 0, 1, lat, lon));
  for (int i = 0; i < 100; i++) {
    assert(b.feed(0, 0, 1, lat, lon));
    assert(lat == 0 && lon == 0);
  }
  assert(b.valid);

  // ---- 3) 倾斜安装静止 (0.6,0,0.8)：水平基自动对齐，重力被完全扣除 ----
  GForceImu c;
  assert(!c.feed(0.6f, 0, 0.8f, lat, lon));
  for (int i = 0; i < 100; i++) {
    assert(c.feed(0.6f, 0, 0.8f, lat, lon));
    assert(magOf(lat, lon) <= 1);          // 毫 g 级残差
  }

  // ---- 4) 水平加速度：0.5g 应完整投影出来 ----
  GForceImu d;
  assert(!d.feed(0, 0, 1, lat, lon));     // 播种纯重力
  assert(d.feed(0.5f, 0, 1, lat, lon));   // 水平 +0.5g（模长 > 1g，门控冻结基线）
  assert(magOf(lat, lon) == 500);
  // 反向 -0.5g 应得到大小相同、方向相反的向量
  GForceImu d2;
  assert(!d2.feed(0, 0, 1, lat, lon));
  int pLat = 0, pLon = 0, nLat = 0, nLon = 0;
  assert(d2.feed(0.5f, 0, 1, pLat, pLon));
  assert(d2.feed(-0.5f, 0, 1, nLat, nLon));
  assert(magOf(nLat, nLon) == 500);

  // ---- 5) 竖直颠簸 (0,0,1.5)：沿重力方向，投影到水平面后应接近 0 ----
  GForceImu e;
  assert(!e.feed(0, 0, 1, lat, lon));
  assert(e.feed(0, 0, 1.5f, lat, lon));
  assert(magOf(lat, lon) <= 2);

  // ---- 6) 门控冻结：持续 0.5g 喂 300 帧也不应被学成重力而衰减 ----
  GForceImu f;
  assert(!f.feed(0, 0, 1, lat, lon));
  assert(f.feed(0.5f, 0, 1, lat, lon));
  for (int i = 0; i < 300; i++) assert(f.feed(0.5f, 0, 1, lat, lon));
  assert(magOf(lat, lon) >= 490);          // 稳态 G 保持，不会退回微笑
  assert(nearf(sqrtf(f.gLP[0] * f.gLP[0] + f.gLP[1] * f.gLP[1] + f.gLP[2] * f.gLP[2]), 1.0f, 0.02f));

  // ---- 7) 倾斜安装下的水平加速度：沿水平基 e2 方向 0.5g ----
  // up=(0.6,0,0.8)；e1=(0,1,0)；e2=up×e1=(-0.8,0,0.6)；a = up + 0.5*e2 = (0.2,0,1.1)
  GForceImu g;
  assert(!g.feed(0.6f, 0, 0.8f, lat, lon));
  assert(g.feed(0.2f, 0, 1.1f, lat, lon));
  assert(magOf(lat, lon) >= 490 && magOf(lat, lon) <= 510);

  // ---- 8) 限幅 ±4000 毫 g ----
  GForceImu h;
  assert(!h.feed(0, 0, 1, lat, lon));
  assert(h.feed(50.0f, -50.0f, 1.0f, lat, lon));
  assert(lat == GForceImu::LIMIT_MG || lat == -GForceImu::LIMIT_MG);
  assert(lon == GForceImu::LIMIT_MG || lon == -GForceImu::LIMIT_MG);

  // ---- 9) 异常输入：NaN / Inf / 自由落体 → 不产出，且状态可恢复 ----
  GForceImu i;
  assert(!i.feed(0, 0, 1, lat, lon));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  assert(!i.feed(nan, 0, 1, lat, lon));
  assert(!i.feed(inf, 0, 1, lat, lon));
  assert(lat == 0 && lon == 0 && !i.valid);
  assert(i.feed(0, 0, 1, lat, lon));       // 之后仍能正常工作
  assert(lat == 0 && lon == 0);

  // ---- 10) reset：重新播种 ----
  f.reset();
  assert(!f.primed && !f.valid && f.gLP[0] == 0 && f.gLP[1] == 0 && f.gLP[2] == 0);
  assert(!f.feed(0, 0, 1, lat, lon));
  assert(f.primed);

  // ---- 11) 常量合理性 ----
  assert(GForceImu::LIMIT_MG == 4000);
  assert(GForceImu::G_MIN < 1.0f && GForceImu::G_MAX > 1.0f);
  assert(GForceImu::alpha() > 0 && GForceImu::alpha() < 0.1f);
  assert(GForceImu::gate() > 0 && GForceImu::gate() < 0.2f);

  std::puts("PASS: IMU gravity removal, auto horizon basis, tilt invariance, vertical bump rejection, steady-G gate, clamp, NaN/Inf, reset");
}

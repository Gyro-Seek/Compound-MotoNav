#pragma once
#include <stdint.h>
#include <math.h>

// ============================================================================
// 板载 QMI8658 六轴 IMU → 设备自身「水平 G 值」
// ============================================================================
// 微雪 ESP32-S3-Touch-AMOLED-1.75 板载 QMI8658（0x6B / 0x6A），与触摸 CST9217、
// 电源 AXP2101、RTC PCF85063 共用 I2C（SDA=15 / SCL=14，见 pin_config.h）。
//
// 分两层，便于单元测试：
//   1) GForceImu：纯数学，无 Arduino / Wire 依赖。三轴比力(g) → 水平面二维 G(毫 g)。
//   2) gforceImuInit()/gforceImuSample()：Wire 驱动，仅 Arduino 编译；非 Arduino
//      环境提供恒失败的 stub，让渲染测试能直接 include 本头。
//
// 为什么不用 attitude 融合：设备安装角度（车把上的倾角）未知且随装法变化。
// 这里用「慢速低通估计重力矢量 → 构造与重力正交的水平基 → 投影线性加速度」：
//   · 静止时输出恒为 0（重力被完全扣除），语义等价于手机系统的「线性加速度」；
//   · 任意安装倾角都自动对齐水平面，不需要用户标定坐标轴；
//   · 沿竖直方向的颠簸被投影掉，只保留加减速 / 转弯 / 侧倾，雷达球不会乱抖。
// ============================================================================

class GForceImu {
public:
  // 25Hz 采样：0.025 → 时间常数 dt/alpha ≈ 1.6s，长弯道的持续 G 也不会立刻被吃掉
  static float alpha() { return 0.025f; }        // 25Hz 采样 → 基线约 1.6s 收敛
  static constexpr int LIMIT_MG = 4000;          // 单轴输出限幅（±4g）
  static constexpr float G_MIN = 0.35f;          // |重力| 低于此视为自由落体 / 异常
  static constexpr float G_MAX = 1.80f;          // |重力| 高于此视为强冲击 / 异常
  // 重力基线更新门控：比力模长偏离 1g 超过这个值，说明存在真实线性加速度，
  // 此时冻结基线，避免把「持续过弯的稳态 G」慢慢学成重力、让表情中途退回微笑。
  static float gate() { return 0.06f; }

  bool primed = false;
  bool valid = false;                            // 最近一次投影是否有效
  float gLP[3] = {0, 0, 0};                     // 重力低通估计（设备坐标系，单位 g）

  void reset() { primed = false; valid = false; gLP[0] = gLP[1] = gLP[2] = 0; }

  // 输入 ax/ay/az：加速度计原始比力，单位 g（含重力，未去偏置）。
  // 输出 lat/lon：水平面两个正交轴上的线性加速度，单位 1/1000 g（毫 g）。
  // 返回 false 表示「尚未校准好」或「本次数据异常」，调用方应跳过这一帧。
  bool feed(float ax, float ay, float az, int &latMg, int &lonMg) {
    latMg = 0; lonMg = 0;
    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az)) { valid = false; return false; }

    // 首帧播种时归一化到 1g：即使进页面那一刻设备正在加速，重力基线也只取
    // 「方向」而不带入加速度幅值，后续由门控低通继续修正。
    if (!primed) {
      const float n0 = sqrtf(ax * ax + ay * ay + az * az);
      if (n0 < G_MIN || !isfinite(n0)) { valid = false; return false; }
      gLP[0] = ax / n0; gLP[1] = ay / n0; gLP[2] = az / n0;
      primed = true; valid = false;
      return false;
    }

    // 门控低通：只有读数接近纯重力（模长 ≈ 1g）时才更新基线。
    // 这样长弯道的持续侧向 G 不会被吃掉，设备安装角的缓慢变化仍能跟上。
    const float an = sqrtf(ax * ax + ay * ay + az * az);
    if (fabsf(an - 1.0f) < gate()) {
      const float a = alpha();
      gLP[0] += (ax - gLP[0]) * a;
      gLP[1] += (ay - gLP[1]) * a;
      gLP[2] += (az - gLP[2]) * a;
    }

    const float gn = sqrtf(gLP[0] * gLP[0] + gLP[1] * gLP[1] + gLP[2] * gLP[2]);
    if (gn < G_MIN || gn > G_MAX || !isfinite(gn)) { valid = false; return false; }

    const float ux = gLP[0] / gn, uy = gLP[1] / gn, uz = gLP[2] / gn;  // 「上」单位向量
    // 线性加速度 = 本次比力 - 重力估计
    const float lx = ax - gLP[0], ly = ay - gLP[1], lz = az - gLP[2];

    // Gram-Schmidt：取与 up 最不平行的设备轴做参考，投影到水平面得到 e1，再用叉乘得 e2。
    // 方位角是任意的（设备无磁力计，也没有绝对航向），但一经选定就稳定；用户关心的是
    // 方向和大小的变化，不是「哪边是北」。
    float rx = 0, ry = 0, rz = 0;
    const float axu = fabsf(ux), ayu = fabsf(uy), azu = fabsf(uz);
    if (axu <= ayu && axu <= azu) rx = 1.0f;
    else if (ayu <= azu) ry = 1.0f;
    else rz = 1.0f;
    const float d = rx * ux + ry * uy + rz * uz;
    float e1x = rx - ux * d, e1y = ry - uy * d, e1z = rz - uz * d;
    const float e1n = sqrtf(e1x * e1x + e1y * e1y + e1z * e1z);
    if (e1n < 1e-4f) { valid = false; return false; }
    e1x /= e1n; e1y /= e1n; e1z /= e1n;
    // e2 = up × e1（up、e1 已单位正交，叉乘结果亦为单位向量）
    const float e2x = uy * e1z - uz * e1y;
    const float e2y = uz * e1x - ux * e1z;
    const float e2z = ux * e1y - uy * e1x;

    float lat = lx * e1x + ly * e1y + lz * e1z;
    float lon = lx * e2x + ly * e2y + lz * e2z;

    // 转成毫 g 并限幅（GForceMeter.feed 也做一次限幅，这里先夹住避免溢出）
    int la = (int)lroundf(lat * 1000.0f);
    int lo = (int)lroundf(lon * 1000.0f);
    if (la > LIMIT_MG) la = LIMIT_MG; else if (la < -LIMIT_MG) la = -LIMIT_MG;
    if (lo > LIMIT_MG) lo = LIMIT_MG; else if (lo < -LIMIT_MG) lo = -LIMIT_MG;
    latMg = la; lonMg = lo;
    valid = true;
    return true;
  }
};

// ----------------------------------------------------------------------------
// Wire 驱动层：只在 Arduino 固件里编译。单元测试（g++）拿到下面的恒失败 stub。
// ----------------------------------------------------------------------------
#if defined(ARDUINO)

#include <Arduino.h>
#include <Wire.h>

// 探测失败时是否允许「只置位、不清除」地补开 AXP2101 的 ALDO1 / BLDO1 电源轨。
// 默认开启；上电默认 0x90 = 0x3F 本来就全开，此路径通常是 no-op，不会误关别的电源。
#ifndef GFIMU_PMU_RESCUE
#define GFIMU_PMU_RESCUE 1
#endif

#define GFIMU_WHO       0x00
#define GFIMU_CTRL1     0x02
#define GFIMU_CTRL2     0x03
#define GFIMU_CTRL3     0x04
#define GFIMU_CTRL7     0x08
#define GFIMU_STATUS0   0x2E
#define GFIMU_AX_L      0x35
#define GFIMU_ID        0x05
#define GFIMU_PMU_ADDR  0x34
#define GFIMU_PMU_LDO0  0x90
#define GFIMU_SCALE     8192.0f          // ±4g → 4.0/32768 = 1/8192 g / LSB

static uint8_t gfImuAddr = 0;

static inline bool gfImuWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

static inline int gfImuRead8(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

static inline uint8_t gfImuProbe() {
  static const uint8_t cand[2] = {0x6B, 0x6A};
  for (int i = 0; i < 2; i++)
    if (gfImuRead8(cand[i], GFIMU_WHO) == GFIMU_ID) return cand[i];
  return 0;
}

// 在 setup() 里 Wire.begin() 之后调用。返回是否找到并配置好 QMI8658。
static inline bool gforceImuInit() {
  gfImuAddr = gfImuProbe();
#if GFIMU_PMU_RESCUE
  if (!gfImuAddr) {
    // 只置位 ALDO1(bit0) / BLDO1(bit4)，保留其它位，绝不清除任何已开启的电源轨。
    const int v = gfImuRead8(GFIMU_PMU_ADDR, GFIMU_PMU_LDO0);
    if (v >= 0 && (v & 0x11) != 0x11) {
      gfImuWrite(GFIMU_PMU_ADDR, GFIMU_PMU_LDO0, (uint8_t)(v | 0x11));
      delay(4);
      gfImuAddr = gfImuProbe();
    }
  }
#endif
  if (!gfImuAddr) {
    Serial.println("GF IMU: QMI8658 not found (tried 0x6B/0x6A)");
    return false;
  }

  gfImuWrite(gfImuAddr, GFIMU_CTRL7, 0x00);      // 先关传感器再改配置
  const int c1 = gfImuRead8(gfImuAddr, GFIMU_CTRL1);
  // CTRL1: ADDR_AI(bit6)=1 连续读地址自增；BE(bit5)=0 小端（AX_L 在前）；其余位保留
  gfImuWrite(gfImuAddr, GFIMU_CTRL1, (uint8_t)(((c1 < 0 ? 0 : c1) | 0x40) & ~0x20));
  gfImuWrite(gfImuAddr, GFIMU_CTRL2, 0x15);      // 加速度 ±4g, ODR 224.2Hz
  gfImuWrite(gfImuAddr, GFIMU_CTRL3, 0x55);      // 陀螺 ±512dps, ODR 224.2Hz（留作扩展）
  gfImuWrite(gfImuAddr, GFIMU_CTRL7, 0x03);      // aEN + gEN，非同步模式
  delay(20);
  Serial.printf("GF IMU: QMI8658 @0x%02X ready (CTRL2=0x%02X CTRL7=0x%02X)\n",
                gfImuAddr, gfImuRead8(gfImuAddr, GFIMU_CTRL2), gfImuRead8(gfImuAddr, GFIMU_CTRL7));
  return true;
}

// 读一次 12 字节（加速度 6 + 陀螺 6），只用加速度三轴。返回是否产出有效 G 值。
static inline bool gforceImuSample(GForceImu &imu, int &latMg, int &lonMg) {
  latMg = 0; lonMg = 0;
  if (!gfImuAddr) return false;
  Wire.beginTransmission(gfImuAddr);
  Wire.write(GFIMU_AX_L);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(gfImuAddr, (uint8_t)12) != 12) return false;
  uint8_t b[12];
  for (int i = 0; i < 12; i++) b[i] = (uint8_t)Wire.read();
  const int16_t rx = (int16_t)(((uint16_t)b[1] << 8) | b[0]);
  const int16_t ry = (int16_t)(((uint16_t)b[3] << 8) | b[2]);
  const int16_t rz = (int16_t)(((uint16_t)b[5] << 8) | b[4]);
  return imu.feed(rx / GFIMU_SCALE, ry / GFIMU_SCALE, rz / GFIMU_SCALE, latMg, lonMg);
}

#else  // 非 Arduino：供单元测试链接的恒失败 stub

static inline bool gforceImuInit() { return false; }
static inline bool gforceImuSample(GForceImu &, int &latMg, int &lonMg) {
  latMg = 0; lonMg = 0; return false;
}

#endif

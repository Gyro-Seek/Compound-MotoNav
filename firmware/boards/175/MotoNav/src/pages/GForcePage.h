#pragma once
// 主页「下滑」进入的 G 值互动页：抠图小狗随 G 值变表情/动作，右上角 G-G 雷达图。
// 小狗是主体（8 帧预烘焙精灵，DOG_CAN×DOG_CAN），雷达图放右上角、不压小狗，
// 红色实心小球随 (横向 G, 纵向 G) 的方向与大小移动，雷达下方显示 G 值数字。
// 由 MotoNav.ino 在 ClockPage.h 之后、PageNavigation.h 之前包含。
//
// 数据来源：板载 QMI8658 六轴 IMU（GForceImu.h），不依赖手机、不依赖 BLE。
// 手机息屏 / 后台 / 断连 / 飞行模式都能正常工作。
#include "../../GForceMeter.h"
#include "../../GForceImu.h"
#include "../../resources/DogSprites.h"

// 全局 G 值状态（与 HomeClock 一样由页面头提供实例，整个固件只编译一次）
GForceMeter gforce;
GForceImu gforceImu;
bool gforceImuOK = false;          // setup() 探测 QMI8658 的结果；false 时页面优雅降级

// ---- 布局：466 圆屏，圆心 (233,233) ----
#define GF_DOG_CX   186          // 小狗画布中心（雷达在右上，小狗略偏左下）
#define GF_DOG_CY   272
// 雷达整体右移：左边界 362-40=322 > 小狗画布右边界 317，两者完全不重叠。
// 圆心到屏幕中心距离约 182，加半径 40 仍小于圆屏半径 233，整块雷达都在圆内。
#define GF_RCX      362          // 雷达圆心（右上角，圆内）
#define GF_RCY      104
#define GF_RR       40           // 雷达半径
#define GF_BALL_R   8            // 红色实心小球半径
#define GF_NUM_Y    (GF_RCY + GF_RR + 10)
#define GF_GRID_C   0x39C7       // 雷达网格灰
#define GF_GRID_DIM 0x2104
#define GF_SPIN_MS  110          // 转圈帧间隔
#define GF_SAMPLE_MS 40          // 板载 IMU 采样节流（约 25Hz），只在 G 页进行
#define GF_DRAW_MS   66          // 整页重画节流（约 15fps）

// 把一帧 RLE 精灵画到 (x0,y0)。索引 0 = 透明；像素已预乘黑底，直接写 RGB565。
// 透明段 O(1) 跳过（大部分背景），非透明段逐像素写、无除法。
static void gfBlitDog(int frame, int x0, int y0) {
  if (frame < 0 || frame >= DOG_FRAMES_N) frame = 0;
  const uint8_t *p = DOG_FRAMES[frame];
  const uint32_t len = DOG_LEN[frame];
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  int x = x0, y = y0, col = 0;
  for (uint32_t i = 0; i + 1 < len; i += 2) {
    int cnt = p[i], idx = p[i + 1];
    if (idx) {
      uint16_t c = DOG_PAL[idx];
      for (int k = 0; k < cnt; k++) {
        if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT)
          fb[(size_t)y * LCD_WIDTH + x] = c;
        if (++col == DOG_CAN) { col = 0; ++y; x = x0; } else ++x;
      }
    } else {
      int total = col + cnt;
      y += total / DOG_CAN;
      col = total % DOG_CAN;
      x = x0 + col;
    }
  }
}

// 四角星（4 个尖三角 + 中心点）
static void gfStar(float cx, float cy, float r, uint16_t c) {
  gfx->fillTriangle(cx, cy - r, cx - r * 0.26f, cy - r * 0.26f, cx + r * 0.26f, cy - r * 0.26f, c);
  gfx->fillTriangle(cx, cy + r, cx - r * 0.26f, cy + r * 0.26f, cx + r * 0.26f, cy + r * 0.26f, c);
  gfx->fillTriangle(cx - r, cy, cx - r * 0.26f, cy - r * 0.26f, cx - r * 0.26f, cy + r * 0.26f, c);
  gfx->fillTriangle(cx + r, cy, cx + r * 0.26f, cy - r * 0.26f, cx + r * 0.26f, cy + r * 0.26f, c);
  gfx->fillCircle(cx, cy, r * 0.30f, c);
}

static void gfHeart(float cx, float cy, float r, uint16_t c) {
  gfx->fillCircle(cx - r * 0.45f, cy - r * 0.20f, r * 0.55f, c);
  gfx->fillCircle(cx + r * 0.45f, cy - r * 0.20f, r * 0.55f, c);
  gfx->fillTriangle(cx - r * 0.92f, cy + r * 0.05f, cx + r * 0.92f, cy + r * 0.05f, cx, cy + r * 1.25f, c);
}

static void gfDrop(float cx, float cy, float r, uint16_t c) {
  gfx->fillCircle(cx, cy + r * 0.35f, r * 0.75f, c);
  gfx->fillTriangle(cx, cy - r, cx - r * 0.62f, cy + r * 0.45f, cx + r * 0.62f, cy + r * 0.45f, c);
}

// 等级 → 精灵帧：0 微笑 / 1、2 张大嘴笑（2 的「蹦蹦跳跳」靠位移 + 地面影表现）/
// 3 兴奋 / 4 眩晕 / 5 转圈（spin0..spin3 四帧循环）。精灵索引 0..7。
static int gfFrameFor(int level, uint32_t now) {
  switch (level) {
    case 0: return 0;
    case 1: case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    default: return 4 + (int)((now / GF_SPIN_MS) % 4);
  }
}

void drawGForcePage() {
  gfx->fillScreen(C_BLACK);
  onWaitScreen = false;
  frameIsMap = false;
  const uint32_t now = millis();
  const int lv = gforce.level;

  // ---- 程序化动作：蹦跳 / 摇摆的位移 ----
  int dx = 0, dy = 0;
  const float ph = (now % 1000) / 1000.0f * 6.2831853f;
  if (lv == 1) { dx = (int)(sinf(ph) * 2); }
  else if (lv == 2) { dy = -(int)(fabsf(sinf(ph)) * 16); }
  else if (lv == 3) { dy = -(int)(fabsf(sinf(ph * 1.5f)) * 22); dx = (int)(sinf(ph) * 3); }
  else if (lv == 4) { dx = (int)(sinf(ph * 0.8f) * 6); }
  else if (lv == 5) { dy = -(int)(fabsf(sinf(ph)) * 6); }

  const int dogX = GF_DOG_CX - DOG_CAN / 2 + dx;
  const int dogY = GF_DOG_CY - DOG_CAN / 2 + dy;

  // ---- 特效（画在小狗下层，透明处露出）----
  if (lv == 2 || lv == 3) {                       // 地面影 + 弹跳线
    gfx->fillRect(GF_DOG_CX - 48, GF_DOG_CY + 108, 96, 8, 0x18C3);
    for (int k = -1; k <= 1; k++)
      gfx->fillRect(GF_DOG_CX - 72 + k * 28, GF_DOG_CY + 84, 3, 14, C_GREY);
  }
  if (lv == 3) {                                  // 兴奋：星星 + 爱心
    gfStar(46, 132, 9, C_YELLOW); gfStar(304, 156, 8, C_YELLOW);
    gfStar(70, 244, 7, C_YELLOW); gfStar(292, 262, 9, C_YELLOW);
    gfHeart(314, 206, 8, 0xF1A6);
  }
  if (lv == 4) {                                  // 眩晕：头顶转圈小星 + 汗滴
    for (int k = 0; k < 3; k++) {
      float a = -0.6f + k * 2.1f + ph * 0.35f;
      gfStar(GF_DOG_CX + cosf(a) * 74, GF_DOG_CY - 130 + sinf(a) * 20, 7, C_YELLOW);
    }
    gfDrop(296, 100, 8, C_CYAN);
  }
  if (lv == 5) {                                  // 转圈：环绕轨迹弧
    for (int a = 200; a <= 340; a += 12) {
      float t = a * 3.14159265f / 180.0f;
      gfx->fillCircle(GF_DOG_CX + cosf(t) * 120, GF_DOG_CY + sinf(t) * 120, 2, C_ORANGE);
    }
    for (int a = 20; a <= 150; a += 14) {
      float t = a * 3.14159265f / 180.0f;
      gfx->fillCircle(GF_DOG_CX + cosf(t) * 132, GF_DOG_CY + sinf(t) * 132, 2, C_ORANGE);
    }
  }

  // ---- 小狗主体 ----
  gfBlitDog(gfFrameFor(lv, now), dogX, dogY);

  // ---- 右上角 G-G 雷达图（同心圆 + 十字 + 红色实心球）----
  gfx->fillCircle(GF_RCX, GF_RCY, GF_RR, GF_GRID_C);
  gfx->fillCircle(GF_RCX, GF_RCY, GF_RR - 2, C_BLACK);
  gfx->fillCircle(GF_RCX, GF_RCY, (int)(GF_RR * 0.66f), GF_GRID_DIM);
  gfx->fillCircle(GF_RCX, GF_RCY, (int)(GF_RR * 0.66f) - 1, C_BLACK);
  gfx->fillCircle(GF_RCX, GF_RCY, (int)(GF_RR * 0.33f), GF_GRID_DIM);
  gfx->fillCircle(GF_RCX, GF_RCY, (int)(GF_RR * 0.33f) - 1, C_BLACK);
  gfx->fillRect(GF_RCX - GF_RR, GF_RCY, GF_RR * 2, 1, GF_GRID_DIM);
  gfx->fillRect(GF_RCX, GF_RCY - GF_RR, 1, GF_RR * 2, GF_GRID_DIM);
  float bx, by;
  gforce.ball(GF_RCX, GF_RCY, GF_RR, bx, by);
  gfx->fillCircle(bx, by, GF_BALL_R, C_RED);
  gfx->fillCircle(bx - 2, by - 2, 2, 0xFBC3);

  char num[8]; gforce.text(num, sizeof(num));
  // IMU 没探测到时数字用灰色，提示「当前是降级静态页」，布局与小字样完全不变。
  printCentered(String(num), GF_RCX, GF_NUM_Y, 2, gforceImuOK ? C_WHITE : C_GREY);

  needFlush = true;
}

// 每帧推进：板载 IMU 采样 → 平滑 / 等级 → 整页重画。
// 只在 G 值页且无遮挡时读 I2C：其它页面（尤其导航 / 拖动）完全不碰传感器总线，
// 保证 V3 已修好的「跟手」手感不受影响。
void gforceTick() {
  if (page != FixedPageLayout::GFORCE_PAGE || sliding || callOn || pairOn || fxOn) return;
  uint32_t now = millis();

  static uint32_t lastSample = 0;
  if (gforceImuOK && (uint32_t)(now - lastSample) >= GF_SAMPLE_MS) {
    lastSample = now;
    int lat = 0, lon = 0;
    if (gforceImuSample(gforceImu, lat, lon)) gforce.feed(lat, lon, now);
  }
  gforce.tick(now);

  static uint32_t lastDraw = 0;
  if ((uint32_t)(now - lastDraw) < GF_DRAW_MS) return;
  lastDraw = now;
  drawGForcePage();
}

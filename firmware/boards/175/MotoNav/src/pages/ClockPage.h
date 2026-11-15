#pragma once
// 主页「上滑」进入的 iOS 时钟页：黑底黑盘、白色刻度/数字/时分针、橙色秒针。
// 表盘半径 222，几乎铺满 466 圆屏（半径 233）；数字时间与日期移到数字 6
// 上方，白色、同字号。时间来自手机 #TIME 同步的 HomeClock（本地按 millis 递增），
// 统一快 1 秒以抵消手机到设备的传输延迟。不引入额外数据源。
// 由 MotoNav.ino 在 WeatherPage.h 之后、PageNavigation.h 之前包含。
#include "../../ClockFace.h"

#define CLOCK_R        222.0f        // 表盘半径：接近整块 466 圆屏（半径 233）
#define CLOCK_TICK_OUT (CLOCK_R - 8.0f)
#define CLOCK_NUM_R    (CLOCK_R - 46.0f)
#define CLOCK_FACE_C   C_BLACK       // 黑底黑盘
#define CLOCK_MARK_C   C_WHITE       // 刻度、数字、时针、分针

// 两端都带圆头的粗线：只用画布基础 API，不经过图标录制（recIcon）通道。
static void clockStroke(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) return;
  float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
  gfx->fillTriangle(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
  gfx->fillTriangle(x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
  gfx->fillCircle(x1, y1, w * 0.5f, c);
  gfx->fillCircle(x0, y0, w * 0.5f, c);
}

// deg：0 = 12 点，顺时针；fwd = 指针长度；back = 中心另一侧的配重长度。
static void clockHand(float deg, float fwd, float back, float w, uint16_t c) {
  float fx, fy, bx, by;
  ClockFace::tip(CX, CX, fwd, deg, fx, fy);
  ClockFace::tip(CX, CX, -back, deg, bx, by);   // 负半径 = 反向
  clockStroke(bx, by, fx, fy, w, c);
}

void drawClockPage() {
  // 黑底黑盘：整屏填黑即表盘底色，白色刻度 / 数字 / 指针直接在黑底上构成表盘。
  // 不再逐像素画半径 222 的黑色圆盘（黑上画黑视觉无变化，却需约 20 万次 sqrtf；
  // slidePrepare 会同步调用本函数，这一步是上滑换页起步「卡顿 / 滑不太动」的主因）。
  gfx->fillScreen(CLOCK_FACE_C);
  onWaitScreen = false;
  frameIsMap = false;

  // 60 根白色刻度：整点粗长，其余细短
  for (int i = 0; i < 60; i++) {
    float deg = (float)i * 6.0f;
    bool major = (i % 5 == 0);
    float rIn = CLOCK_TICK_OUT - (major ? 16.0f : 7.0f);
    float x0, y0, x1, y1;
    ClockFace::tip(CX, CX, rIn, deg, x0, y0);
    ClockFace::tip(CX, CX, CLOCK_TICK_OUT, deg, x1, y1);
    clockStroke(x0, y0, x1, y1, major ? 5.0f : 2.0f, CLOCK_MARK_C);
  }

  // 12 / 3 / 6 / 9 四个白色数字（内置点阵字体 size 3）
  const int sixY = (int)(CX + CLOCK_NUM_R) - 12;      // 数字 6 顶部
  printCentered("12", CX, (int)(CX - CLOCK_NUM_R) - 12, 3, CLOCK_MARK_C);
  printCentered("3",  (int)(CX + CLOCK_NUM_R), CX - 12, 3, CLOCK_MARK_C);
  printCentered("6",  CX, sixY, 3, CLOCK_MARK_C);
  printCentered("9",  (int)(CX - CLOCK_NUM_R), CX - 12, 3, CLOCK_MARK_C);

  const uint32_t now = millis();
  char hhmm[6] = "--:--", date[11] = "---- -- --";
  // 手机 → 设备有传输延迟：指针与数字时间统一使用 +1 秒读数，二者永不打架。
  const int64_t secs = homeClock.clockSecondKey(now);
  const bool valid = homeClock.formatSeconds(secs, hhmm, date);
  memcpy(homeClockTime, hhmm, sizeof(hhmm));
  memcpy(homeClockDate, date, sizeof(date));

  // 数字时间与日期：移到数字 6 上方，白色，两个都是 size 3（同字号）。
  // 先画文字再画指针，让指针像真表一样压在字上。
  const int timeY = sixY - 78;
  const int dateY = timeY + 34;
  printCentered(String(hhmm), CX, timeY, 3, CLOCK_MARK_C);
  if (valid) printCentered(String(date), CX, dateY, 3, CLOCK_MARK_C);
  else       printCentered("LINK PHONE FOR TIME", CX, dateY, 1, CLOCK_MARK_C);

  if (valid) {
    int s = (int)(secs % 60);
    int m = (int)((secs / 60) % 60);
    int h = (int)((secs / 3600) % 24);
    ClockFace::Hands hd = ClockFace::hands(h, m, s);
    clockHand(hd.hourDeg,   CLOCK_R * 0.52f, CLOCK_R * 0.10f, 11.0f, CLOCK_MARK_C);
    clockHand(hd.minuteDeg, CLOCK_R * 0.74f, CLOCK_R * 0.10f, 7.0f,  CLOCK_MARK_C);
    clockHand(hd.secondDeg, CLOCK_R * 0.84f, CLOCK_R * 0.20f, 3.0f,  C_ORANGE);
  }
  aaDisc(CX, CX, 6.0f, C_ORANGE);           // 秒针轴心橙点（未同步时也保留）

  // 不再画任何返回提示文字；返回仍为下滑，由手势直接完成。
  needFlush = true;
}

// 每秒刷新一次（秒针跳格）；滑入 / 来电 / 配对 / 到达特效期间不抢画面。
void clockTick() {
  if (page != FixedPageLayout::CLOCK_PAGE || sliding || callOn || pairOn || fxOn) return;
  static int64_t drawn = -2;
  int64_t key = homeClock.clockSecondKey(millis());
  if (key == drawn) return;
  drawn = key;
  drawClockPage();
}

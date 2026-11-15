#ifdef NDEBUG
#undef NDEBUG
#endif
// Renders the real ClockPage.h into an in-memory 466x466 canvas and checks the
// black-on-black dial, white marks/hands, orange second hand, label colors and
// the +1s display lead.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using std::min; using std::max;

using String = std::string;
constexpr int LCD_WIDTH = 466, LCD_HEIGHT = 466, CX = 233;
constexpr uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF, C_GREY = 0x7BEF;
constexpr uint16_t C_DIM = 0x6B4D, C_ORANGE = 0xFBC3;

struct Canvas {
  uint16_t px[LCD_WIDTH * LCD_HEIGHT]{};
  void fillScreen(uint16_t c) { std::fill(px, px + LCD_WIDTH * LCD_HEIGHT, c); }
  uint16_t *getFramebuffer() { return px; }
  void drawPixel(int x, int y, uint16_t c) {
    if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) px[y * LCD_WIDTH + x] = c;
  }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) drawPixel(i, j, c);
  }
  void fillCircle(int cx, int cy, float r, uint16_t c) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
      for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++)
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) drawPixel(x, y, c);
  }
  void fillTriangle(int ax, int ay, int bx, int by, int cx, int cy, uint16_t c) {
    for (int y = min({ay, by, cy}); y <= max({ay, by, cy}); y++)
      for (int x = min({ax, bx, cx}); x <= max({ax, bx, cx}); x++) {
        int a = (x - ax) * (by - ay) - (y - ay) * (bx - ax);
        int b = (x - bx) * (cy - by) - (y - by) * (cx - bx);
        int d = (x - cx) * (ay - cy) - (y - cy) * (ax - cx);
        if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) drawPixel(x, y, c);
      }
  }
  void setTextSize(int) {}
  void setTextColor(uint16_t) {}
  void setCursor(int, int) {}
  void print(const String &) {}
} canvas;
Canvas *gfx = &canvas;

std::vector<float> aaDiscRadii;
void aaDisc(float cx, float cy, float rad, uint16_t c) {
  aaDiscRadii.push_back(rad);
  gfx->fillCircle((int)lroundf(cx), (int)lroundf(cy), rad, c);
}
struct Label { std::string s; int x, y, size; uint16_t color; };
std::vector<Label> labels;
void printCentered(const String &s, int x, int y, int size, uint16_t color) {
  labels.push_back({s, x, y, size, color});
}

bool onWaitScreen = false, frameIsMap = false, needFlush = false, sliding = false;
bool callOn = false, pairOn = false, fxOn = false;
int page = 0;
char homeClockTime[6] = {}, homeClockDate[11] = {};
uint32_t fakeNow = 1000;
uint32_t millis() { return fakeNow; }

#include "../boards/175/MotoNav/FixedPageLayout.h"
#include "../boards/175/MotoNav/HomeClock.h"
HomeClock homeClock;
#include "../boards/175/MotoNav/src/pages/ClockPage.h"

static const Label *find(const std::string &s) {
  for (const auto &l : labels) if (l.s == s) return &l;
  return nullptr;
}
static bool has(const std::string &s) { return find(s) != nullptr; }
static uint16_t at(int x, int y) { return canvas.px[y * LCD_WIDTH + x]; }
static int countColor(int x0, int y0, int x1, int y1, uint16_t c) {
  int n = 0;
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (at(x, y) == c) n++;
  return n;
}

int main() {
  // 17:23:00 local on 2026-10-26 (verified by HomeClockTest's UTC+8 case).
  assert(homeClock.receive("#TIME,1793006580,28800", fakeNow));
  page = FixedPageLayout::CLOCK_PAGE;
  aaDiscRadii.clear();
  drawClockPage();

  // 换页起步不再逐像素画半径 222 的黑盘：黑底由 fillScreen 一次填成，
  // aaDisc 只剩半径 6 的橙色轴心，slidePrepare 不再被 20 万次 sqrtf 卡住。
  assert(aaDiscRadii.size() == 1 && aaDiscRadii[0] == 6.0f);

  // 黑底黑盘：盘内空白处是黑的，盘外也是黑的；没有白色圆盘 / 浅灰外环。
  assert(at(233, 233) == C_ORANGE);          // orange second-hand hub
  assert(at(173, 173) == C_BLACK);           // inside the dial, away from marks/hands
  assert(at(233, 8) == C_BLACK);             // outside the dial stays black
  assert(at(233, 27) == C_WHITE);            // 12 o'clock major tick, white
  assert(at(27, 233) == C_WHITE);            // 9 o'clock major tick near the screen edge
  assert(countColor(200, 40, 280, 200, C_ORANGE) > 20);   // orange second hand
  assert(countColor(255, 255, 340, 340, C_WHITE) > 20);   // white hour/minute hands

  const Label *l12 = find("12"), *l3 = find("3"), *l6 = find("6"), *l9 = find("9");
  const Label *lt = find("17:23"), *ld = find("2026-10-26");
  assert(l12 && l3 && l6 && l9 && lt && ld);
  assert(!has("SWIPE DOWN TO RETURN"));      // return hint removed
  assert(!has("--:--") && !has("LINK PHONE FOR TIME"));
  // 所有数字标签都是白色；时间与日期同字号，且都在数字 6 上方。
  assert(l12->color == C_WHITE && l3->color == C_WHITE && l6->color == C_WHITE && l9->color == C_WHITE);
  assert(lt->color == C_WHITE && ld->color == C_WHITE);
  assert(lt->size == ld->size && lt->size == 3);
  assert(lt->y < ld->y && ld->y < l6->y);
  assert(!onWaitScreen && !frameIsMap && needFlush);
  assert(strcmp(homeClockTime, "17:23") == 0 && strcmp(homeClockDate, "2026-10-26") == 0);

  // +1 秒：手机 17:23:59 → 设备显示 17:24:00（首页读数仍是 17:23）。
  HomeClock lead;
  assert(lead.receive("#TIME,1793006639,28800", fakeNow));
  char base[6], baseDate[11], ahead[6], aheadDate[11];
  assert(lead.format(fakeNow, base, baseDate));
  assert(strcmp(base, "17:23") == 0);
  assert(lead.clockSecondKey(fakeNow) == 1793035440LL);
  assert(lead.formatSeconds(lead.clockSecondKey(fakeNow), ahead, aheadDate));
  assert(strcmp(ahead, "17:24") == 0);
  homeClock = lead;
  labels.clear();
  drawClockPage();
  assert(has("17:24") && strcmp(homeClockTime, "17:24") == 0);

  // Once-per-second redraw: same second does nothing, next second repaints.
  labels.clear();
  clockTick();
  const size_t afterFirst = labels.size();
  assert(afterFirst > 0);
  clockTick();
  assert(labels.size() == afterFirst);
  fakeNow += 1000;
  clockTick();
  assert(labels.size() == afterFirst * 2);
  assert(has("17:24"));                      // minute unchanged, second hand moved

  // No phone time yet: black dial + white ticks + hub are drawn, no hands and no fake time.
  homeClock = HomeClock();
  labels.clear();
  fakeNow += 1000;
  drawClockPage();
  assert(at(233, 233) == C_ORANGE);
  assert(at(233, 27) == C_WHITE);
  assert(countColor(200, 40, 280, 200, C_ORANGE) == 0);
  assert(has("--:--") && has("LINK PHONE FOR TIME"));
  assert(!onWaitScreen && needFlush);

  std::puts("PASS: black dial, white ticks/numbers/hands, orange second hand, labels above 6, +1s lead, unsynced state");
}

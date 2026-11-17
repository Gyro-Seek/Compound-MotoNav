#ifdef NDEBUG
#undef NDEBUG
#endif
// 把真实的 GForcePage.h 渲染进内存 466x466 画布，验证：
// 8 帧精灵 RLE 完整性、等级→表情帧映射、黑底 + 小狗主体 + 右上角雷达 + 红球 + G 值数字、
// 红球随 G 方向/大小移动、特效分级、IMU 未就绪时的灰色降级数字、gforceTick 的页面/遮挡保护。
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using std::min; using std::max;

struct String : std::string {
  using std::string::string;
  String() = default;
  String(const std::string &s) : std::string(s) {}
  String &operator=(const char *s) { std::string::operator=(s); return *this; }
};
String operator+(const std::string &s, int n) { return String(s + std::to_string(n)); }

constexpr int LCD_WIDTH = 466, LCD_HEIGHT = 466;
constexpr uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF, C_GREEN = 0x07E0;
constexpr uint16_t C_GREY = 0x7BEF, C_CYAN = 0x07FF, C_RED = 0xF800;
constexpr uint16_t C_YELLOW = 0xF623, C_ORANGE = 0xFBC3;

uint32_t fakeNow = 0;
unsigned long millis() { return fakeNow; }

struct Canvas {
  uint16_t px[LCD_WIDTH * LCD_HEIGHT]{};
  void fillScreen(uint16_t c) { std::fill(px, px + LCD_WIDTH * LCD_HEIGHT, c); }
  uint16_t *getFramebuffer() { return px; }
  void drawPixel(int x, int y, uint16_t c) {
    if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT) px[(size_t)y * LCD_WIDTH + x] = c;
  }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) drawPixel(i, j, c);
  }
  void fillCircle(float cx, float cy, float r, uint16_t c) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
      for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++)
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) drawPixel(x, y, c);
  }
  void fillTriangle(float ax, float ay, float bx, float by, float cx, float cy, uint16_t c) {
    for (int y = (int)min({ay, by, cy}); y <= (int)max({ay, by, cy}); y++)
      for (int x = (int)min({ax, bx, cx}); x <= (int)max({ax, bx, cx}); x++) {
        float a = (x - ax) * (by - ay) - (y - ay) * (bx - ax);
        float b = (x - bx) * (cy - by) - (y - by) * (cx - bx);
        float d = (x - cx) * (ay - cy) - (y - cy) * (ax - cx);
        if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) drawPixel(x, y, c);
      }
  }
} canvas;
Canvas *gfx = &canvas;

struct TextRec { std::string value; int x, y, size; uint16_t color; };
std::vector<TextRec> texts;
void printCentered(const String &s, int x, int y, int size, uint16_t color) {
  texts.push_back({s, x, y, size, color});
}

bool onWaitScreen = false, frameIsMap = false, needFlush = false;
bool sliding = false, callOn = false, pairOn = false, fxOn = false;
int page = 0;
namespace FixedPageLayout { constexpr int CLOCK_PAGE = 3, GFORCE_PAGE = 4; }

#include "../boards/175/MotoNav/src/pages/GForcePage.h"

static int countColor(int x0, int y0, int x1, int y1, uint16_t c) {
  int n = 0;
  for (int y = max(0, y0); y < min(LCD_HEIGHT, y1); y++)
    for (int x = max(0, x0); x < min(LCD_WIDTH, x1); x++)
      if (canvas.px[(size_t)y * LCD_WIDTH + x] == c) n++;
  return n;
}
static int nonBlack(int x0, int y0, int x1, int y1) {
  int n = 0;
  for (int y = max(0, y0); y < min(LCD_HEIGHT, y1); y++)
    for (int x = max(0, x0); x < min(LCD_WIDTH, x1); x++)
      if (canvas.px[(size_t)y * LCD_WIDTH + x] != C_BLACK) n++;
  return n;
}
static bool hasText(const std::string &s) {
  for (const auto &t : texts) if (t.value == s) return true;
  return false;
}

int main() {
  // ---- 1) 精灵资源自检：8 帧 RLE 都能完整还原成 DOG_CAN×DOG_CAN 像素 ----
  assert(DOG_FRAMES_N == 8 && DOG_CAN == 262 && DOG_PAL_N == 25);
  for (int f = 0; f < DOG_FRAMES_N; f++) {
    const uint8_t *p = DOG_FRAMES[f];
    const uint32_t len = DOG_LEN[f];
    assert(len > 0 && len % 2 == 0);
    long total = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
      assert(p[i] >= 1);                 // count 必须 1..255
      assert(p[i + 1] < DOG_PAL_N);      // 索引 0..24，0 = 透明
      total += p[i];
    }
    assert(total == (long)DOG_CAN * DOG_CAN);
  }

  // ---- 2) 等级 → 表情帧映射（微笑→张大嘴笑→蹦跳→兴奋→眩晕→转圈）----
  assert(gfFrameFor(0, 0) == 0);
  assert(gfFrameFor(1, 0) == 1);
  assert(gfFrameFor(2, 0) == 1);
  assert(gfFrameFor(3, 0) == 2);
  assert(gfFrameFor(4, 0) == 3);
  const int s0 = gfFrameFor(5, 0);
  assert(s0 >= 4 && s0 <= 7);
  assert(gfFrameFor(5, GF_SPIN_MS) != s0);
  assert(gfFrameFor(5, GF_SPIN_MS * 4) == s0);       // 4 帧循环

  // ---- 3) 静止页面：黑底、小狗主体、透明背景、雷达、红球、0.00G ----
  gforceImuOK = true;                  // IMU 就绪：数字应为白色
  page = FixedPageLayout::GFORCE_PAGE;
  fakeNow = 1000;
  texts.clear();
  canvas.fillScreen(0x1234);            // 先填个非空底色，验证整屏被重画
  onWaitScreen = true; frameIsMap = true; needFlush = false;
  drawGForcePage();
  assert(!onWaitScreen && !frameIsMap && needFlush);
  assert(canvas.px[0] == C_BLACK);
  assert(canvas.px[LCD_WIDTH * LCD_HEIGHT - 1] == C_BLACK);
  const int dogX = GF_DOG_CX - DOG_CAN / 2, dogY = GF_DOG_CY - DOG_CAN / 2;
  assert(nonBlack(dogX + 40, dogY + 40, dogX + DOG_CAN - 40, dogY + DOG_CAN - 40) > 3000);
  assert(nonBlack(dogX, dogY, dogX + 24, dogY + 24) == 0);
  assert(countColor(GF_RCX - GF_RR, GF_RCY - GF_RR, GF_RCX + GF_RR, GF_RCY + GF_RR, GF_GRID_C) > 0);
  assert(countColor(GF_RCX - 9, GF_RCY - 9, GF_RCX + 10, GF_RCY + 10, C_RED) > 0);
  assert(GF_RCX - GF_RR > dogX + DOG_CAN);   // 雷达完全不压小狗
  assert(hasText("0.00G"));
  for (const auto &t : texts) if (t.value == "0.00G") {
    assert(t.color == C_WHITE && t.y == GF_NUM_Y && t.x == GF_RCX);
  }

  // ---- 4) 大 G：红球按方向偏离圆心、数字变大、等级特效分级 ----
  gforce.gx = 1.2f; gforce.gy = 0.9f; gforce.level = 5;
  gforce.everFed = true; gforce.lastFed = fakeNow;
  texts.clear(); fakeNow = 2000;
  drawGForcePage();
  assert(hasText("1.50G"));             // sqrt(1.44 + 0.81) = 1.5
  float bx = 0, by = 0;
  gforce.ball(GF_RCX, GF_RCY, GF_RR, bx, by);
  assert(bx > GF_RCX + 10 && by < GF_RCY - 5);
  assert(countColor((int)bx - 6, (int)by - 6, (int)bx + 7, (int)by + 7, C_RED) > 0);
  assert(countColor(GF_RCX - 6, GF_RCY - 6, GF_RCX + 7, GF_RCY + 7, C_RED) == 0);

  gforce.level = 3; texts.clear(); fakeNow = 3000;
  drawGForcePage();
  assert(countColor(36, 122, 58, 144, C_YELLOW) > 0);
  gforce.level = 4; texts.clear(); fakeNow = 4000;
  drawGForcePage();
  assert(countColor(287, 90, 305, 118, C_CYAN) > 0);

  // ---- 5) 板载 IMU 未就绪：数字变灰（降级静态页），布局不变 ----
  gforce.reset(); gforceImuOK = false;
  texts.clear(); fakeNow = 5000;
  drawGForcePage();
  assert(hasText("0.00G"));
  bool grey = false;
  for (const auto &t : texts) if (t.value == "0.00G") {
    assert(t.y == GF_NUM_Y && t.x == GF_RCX && t.size == 2);
    grey = (t.color == C_GREY);
  }
  assert(grey);

  // ---- 6) gforceTick：只在 G 页且无遮挡时重画；非 G 页完全不碰 IMU 总线 ----
  gforceImuOK = true;
  gforce.reset();
  page = 0; texts.clear(); fakeNow = 6000;
  gforceTick(); assert(texts.empty());
  page = FixedPageLayout::GFORCE_PAGE;
  sliding = true; texts.clear(); fakeNow = 6200; gforceTick(); assert(texts.empty());
  sliding = false; callOn = true; texts.clear(); fakeNow = 6400; gforceTick(); assert(texts.empty());
  callOn = false; pairOn = true; texts.clear(); fakeNow = 6600; gforceTick(); assert(texts.empty());
  pairOn = false; fxOn = true; texts.clear(); fakeNow = 6800; gforceTick(); assert(texts.empty());
  fxOn = false; texts.clear(); fakeNow = 7000;
  gforceTick(); assert(!texts.empty());   // 恢复后正常重画

  std::puts("PASS: dog sprite RLE, level->frame, black bg + dog + radar + red ball + G text, ball direction, tier effects, IMU-off grey fallback, gforceTick guards");
}

#pragma once

// Screen positions: music(-1,0), navigation(0,0), weather(1,0),
// rain(1,-1), alerts(1,1), iOS clock(0,1) reached by swiping up on home.
// sign is the corrected finger movement in canvas space (y grows downward):
//   sign < 0 = swipe up, sign > 0 = swipe down.
namespace FixedPageLayout {
constexpr int CLOCK_PAGE = 3;   // 主页上滑进入的 iOS 时钟页
struct Target { int page; int detail; bool allowed; };
inline Target move(int page, int detail, bool horizontal, int sign) {
  Target none{page, detail, false};
  if (sign != -1 && sign != 1) return none;
  if (horizontal) {
    if (page == 2 && sign < 0) return {0, 0, true};
    if (page == 0) return sign < 0 ? Target{1, 0, true} : Target{2, 0, true};
    if (page == 1 && detail == 0 && sign > 0) return {0, 0, true};
    return none;
  }
  // 主页上滑进 iOS 时钟页、下滑返回；时钟页不参与左右 / 继续上滑。
  // 上一版把方向写反了（实机变成下滑进时钟），这里纠正。
  if (page == 0) return sign < 0 ? Target{CLOCK_PAGE, 0, true} : none;
  if (page == CLOCK_PAGE) return sign > 0 ? Target{0, 0, true} : none;
  if (page != 1) return none;
  if (detail == 0) return {1, sign > 0 ? 1 : 2, true};
  if (detail == 1 && sign < 0) return {1, 0, true};
  if (detail == 2 && sign > 0) return {1, 0, true};
  return none;
}
inline int weatherRow(int detail) { return detail == 1 ? 0 : detail == 2 ? 2 : 1; }
}

#ifdef NDEBUG
#undef NDEBUG
#endif
// Pure geometry for the iOS-style analog clock face (no Arduino dependencies).
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../boards/175/MotoNav/ClockFace.h"

static bool near(float a, float b) { return fabsf(a - b) < 0.002f; }

int main() {
  ClockFace::Hands h = ClockFace::hands(12, 0, 0);
  assert(near(h.hourDeg, 0) && near(h.minuteDeg, 0) && near(h.secondDeg, 0));

  h = ClockFace::hands(3, 0, 0);   assert(near(h.hourDeg, 90));
  h = ClockFace::hands(6, 0, 0);   assert(near(h.hourDeg, 180));
  h = ClockFace::hands(9, 0, 0);   assert(near(h.hourDeg, 270));
  h = ClockFace::hands(0, 0, 0);   assert(near(h.hourDeg, 0));       // 12 AM == 12 PM
  h = ClockFace::hands(15, 0, 0);  assert(near(h.hourDeg, 90));       // 3 PM

  h = ClockFace::hands(23, 30, 30);
  assert(near(h.hourDeg, 345.25f));      // 11:30:30 → hour hand between 11 and 12
  assert(near(h.minuteDeg, 183.0f));     // minute hand moves continuously
  assert(near(h.secondDeg, 180.0f));

  h = ClockFace::hands(10, 9, 36);       // the classic iOS clock icon time
  assert(near(h.hourDeg, 304.8f));
  assert(near(h.minuteDeg, 57.6f));
  assert(near(h.secondDeg, 216.0f));

  h = ClockFace::hands(12, 0, 0, 0.5f); // sub-second sweep fraction
  assert(near(h.secondDeg, 3.0f));
  assert(near(h.minuteDeg, 0.05f));

  float x = 0, y = 0;
  ClockFace::tip(233, 233, 100, 0, x, y);     assert(near(x, 233) && near(y, 133));
  ClockFace::tip(233, 233, 100, 90, x, y);    assert(near(x, 333) && near(y, 233));
  ClockFace::tip(233, 233, 100, 180, x, y);   assert(near(x, 233) && near(y, 333));
  ClockFace::tip(233, 233, 100, 270, x, y);   assert(near(x, 133) && near(y, 233));
  ClockFace::tip(233, 233, -20, 90, x, y);    assert(near(x, 213) && near(y, 233)); // counterweight
  ClockFace::tip(233, 233, 0, 45, x, y);      assert(near(x, 233) && near(y, 233));

  std::puts("PASS: iOS clock hand angles, continuous minute/hour, sub-second sweep, tip/counterweight geometry");
}

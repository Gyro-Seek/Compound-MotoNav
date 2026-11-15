#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include "../boards/175/MotoNav/HomeClock.h"
void expect(HomeClock &c,uint32_t now,const char*t,const char*d){char time[6],date[11];assert(c.format(now,time,date));assert(strcmp(time,t)==0);assert(strcmp(date,d)==0);}
void check(const char*p,const char*t,const char*d){HomeClock c;assert(c.receive(p,100));expect(c,100,t,d);}
int main(){
  HomeClock c;char t[6],d[11];assert(!c.format(0,t,d));assert(strcmp(t,"--:--")==0);assert(strcmp(d,"---- -- --")==0);
  check("#TIME,1793006580,28800","17:23","2026-10-26");
  check("#TIME,1709135999,28800","23:59","2024-02-28");
  check("#TIME,1735660799,28800","23:59","2024-12-31");
  check("#TIME,1792984200,-18000","22:10","2026-10-25");
  check("#TIME,1793016000,19800","17:30","2026-10-26");
  assert(c.receive("#TIME,1709135999,28800",100));expect(c,1100,"00:00","2024-02-29");
  assert(c.receive("#TIME,1677599999,28800",100));expect(c,1100,"00:00","2023-03-01");
  assert(c.receive("#TIME,1735660799,28800",100));expect(c,1100,"00:00","2025-01-01");
  assert(c.receive("#TIME,1793030399,28800",0xfffffe0bU));expect(c,499U,"00:00","2026-10-27");
  // No further phone messages: independently advance for a whole day.
  expect(c,86400499U,"00:00","2026-10-28");
  for(const char*bad:{"#TIME,0,0","#TIME,4102444800,0","#TIME,1793000000,50401","#TIME,1793000000,-50401","#TIME,1793000000,","#TIME,1793000000,28800x","#TIME,9999999999999999999,0","#TIME,,0","#TIME,1793000000,+28800","#TIME,1793000000,0,1"}){assert(!c.receive(bad,86400499U));expect(c,86400499U,"00:00","2026-10-28");}
  // Explicit re-sync also follows a change of timezone or manual phone time.
  assert(c.receive("#TIME,1793016000,19800",86400499U));expect(c,86400499U,"17:30","2026-10-26");
  // iOS clock page: second-level local time and once-per-second redraw key.
  HomeClock s;
  assert(s.secondKey(0)==-1);
  assert(s.receive("#TIME,1793006580,28800",100));
  assert(s.secondKey(100)==1793035380LL);
  assert(s.secondKey(1100)==1793035381LL);
  assert(s.receive("#TIME,1793006580,0",1100));   // timezone correction moves seconds back
  assert(s.secondKey(1100)==1793006580LL);
  // iOS 时钟页 +1 秒提前量：clockSecondKey / formatSeconds 与未提前读数一致偏移 1 秒。
  HomeClock lead;
  char lt[6],ld[11],at[6],ad[11];
  assert(lead.clockSecondKey(0)==-1);
  assert(lead.receive("#TIME,1793006580,28800",100));
  assert(lead.secondKey(100)==1793035380LL);
  assert(lead.clockSecondKey(100)==1793035381LL);
  assert(lead.format(100,lt,ld));assert(strcmp(lt,"17:23")==0);
  assert(lead.formatSeconds(lead.clockSecondKey(100),at,ad));
  assert(strcmp(at,"17:23")==0&&strcmp(ad,"2026-10-26")==0);
  // 跨分钟：手机 17:23:59 → 设备显示 17:24:00，未提前的 format 仍是 17:23。
  assert(lead.receive("#TIME,1793006639,28800",100));
  assert(lead.format(100,lt,ld));
  assert(strcmp(lt,"17:23")==0);
  assert(lead.clockSecondKey(100)==1793035440LL);
  assert(lead.formatSeconds(lead.clockSecondKey(100),at,ad));
  assert(strcmp(at,"17:24")==0&&strcmp(ad,"2026-10-26")==0);
  // 无效 / 未同步：仍是占位符，不产生假时间；read() 与 minuteKey() 行为不变。
  assert(!lead.formatSeconds(-1,at,ad));
  assert(strcmp(at,"--:--")==0&&strcmp(ad,"---- -- --")==0);
  HomeClock::Reading r;
  assert(lead.read(100,r) && r.year==2026 && r.month==10 && r.day==26 && r.hour==17 && r.minute==23);
  assert(lead.minuteKey(100)==29883923LL);
  puts("PASS: 24h/date, offsets, midnight, leap year, year boundary, millis rollover, offline advance, malformed packets, re-sync, secondKey, +1s clock lead");
}

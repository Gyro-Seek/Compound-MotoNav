#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Phone UTC seconds plus its current UTC offset. No GPS/ETA/weather time reuse.
// Advancing on every loop handles millis() rollover without needing an RTC.
class HomeClock {
public:
  // 手机 #TIME 经蓝牙到设备有传输延迟；iOS 时钟页统一按 +1 秒显示。
  // 首页等待态的小数字时钟仍用未提前的读数，不受影响。
  static constexpr int64_t CLOCK_LEAD_MS = 1000;
  struct Reading { int year,month,day,hour,minute; };
  bool synced() const { return valid; }
  void tick(uint32_t now) {
    if(!valid)return;
    localMillis += static_cast<uint32_t>(now-lastTick);
    lastTick=now;
  }
  // Cheap face revision. It includes the date and follows backward phone corrections.
  int64_t minuteKey(uint32_t now) {
    tick(now);return valid?localMillis/60000:-1;
  }
  // Local Unix seconds (phone UTC + offset), -1 before the first #TIME packet.
  int64_t secondKey(uint32_t now) {
    tick(now);return valid?localMillis/1000:-1;
  }
  // The iOS clock page renders phone time one second ahead to absorb BLE lag.
  // Hands and the digital label both derive from this one key, so they never disagree.
  int64_t clockSecondKey(uint32_t now) {
    int64_t s=secondKey(now);
    return s<0?-1:s+CLOCK_LEAD_MS/1000;
  }
  bool receive(const char *packet,uint32_t now) {
    if(!packet||strncmp(packet,"#TIME,",6)!=0)return false;
    const char *p=packet+6;
    uint64_t seconds=0;
    unsigned digits=0;
    while(*p>='0'&&*p<='9') {
      if(++digits>10)return false;
      seconds=seconds*10+(*p++-'0');
    }
    if(!digits||*p++!=','||seconds<1577836800ULL||seconds>=4102444800ULL)return false;
    bool negative=*p=='-';if(negative)++p;
    int offset=0;digits=0;
    while(*p>='0'&&*p<='9') {
      if(++digits>5)return false;
      offset=offset*10+(*p++-'0');
    }
    if(!digits||*p||offset>14*3600)return false;
    if(negative)offset=-offset;
    localMillis=(static_cast<int64_t>(seconds)+offset)*1000;
    lastTick=now;valid=true;
    return true;
  }
  bool read(uint32_t now,Reading &out) {
    int64_t seconds=secondKey(now);
    if(seconds<0)return false;
    civil(seconds,out);
    return true;
  }
  // Format an explicit local Unix-seconds value; the clock page passes its +1s key.
  bool formatSeconds(int64_t seconds,char (&time)[6],char (&date)[11]) const {
    if(seconds<0){memcpy(time,"--:--",6);memcpy(date,"---- -- --",11);return false;}
    Reading out;civil(seconds,out);
    snprintf(time,sizeof(time),"%02d:%02d",out.hour,out.minute);
    snprintf(date,sizeof(date),"%04d-%02d-%02d",out.year,out.month,out.day);
    return true;
  }
  bool format(uint32_t now,char (&time)[6],char (&date)[11]) {
    return formatSeconds(secondKey(now),time,date);
  }
private:
  static void civil(int64_t seconds,Reading &out) {
    out.hour=static_cast<int>((seconds/3600)%24);out.minute=static_cast<int>((seconds/60)%60);
    int days=static_cast<int>(seconds/86400);
    out.year=1970;
    for(;;){int n=leap(out.year)?366:365;if(days<n)break;days-=n;++out.year;}
    static const int months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    out.month=1;
    for(int i=0;i<12;++i){int n=months[i]+(i==1&&leap(out.year)?1:0);if(days<n)break;days-=n;++out.month;}
    out.day=days+1;
  }
  bool valid=false;
  uint32_t lastTick=0;
  int64_t localMillis=0;
  static bool leap(int year){return year%4==0&&(year%100!=0||year%400==0);}
};

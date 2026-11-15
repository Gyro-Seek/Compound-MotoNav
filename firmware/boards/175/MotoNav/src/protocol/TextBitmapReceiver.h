#pragma once
// 手机文字位图分包接收。由 MotoNav.ino 按依赖顺序包含，仅编译一次。
// 'B' slot id part parts wLo wHi h offLo offHi + 数据
void handleBitmap(const uint8_t *d, int n) {
  if (n < 10) return;
  int slot = d[1], id = d[2], part = d[3], parts = d[4];
  int w = d[5] | (d[6] << 8), h = d[7];
  int off = d[8] | (d[9] << 8);
  if (slot >= IMG_SLOTS) return;
  TextImg &im = imgs[slot];
  if (w == 0 || h == 0 || parts == 0) {          // 空图：清掉这一栏
    im.showW = 0; im.showH = 0; im.recvId = -1;
    navDirty = true;
    if (slot == 3 || slot == 4 || (slot >= 7 && slot <= 11)) wxDirty = true;
    if (slot == 5 || slot == 6 || slot == 12 || slot == 13 || slot == 14) musicDirty = true;
    return;
  }
  // 歌词位图只受"逐句歌词"开关控制。旧逻辑还要求 hidMusicMode、appleMedia.available
  // 以及 generation/titleRev/artistRev 与 AMS 完全一致，任一刷新时序差都会把整帧歌词
  // 静默丢弃（歌名 slot 5/6 无此门槛，因此出现"有歌名、无歌词"）。曲目切换由 #LY 负责清理。
  // 12/13/14 分别是当前句 / 上一句 / 下一句，三者同受该开关控制。
  if ((slot == 12 || slot == 13 || slot == 14) && !musicLyrics) return;
  int total = ((w + 1) / 2) * h;
  if (total > IMG_MAX || parts > 32) return;
  if (id != im.recvId) {                          // 新的一张图：重新开始接收
    im.recvId = id; im.recvW = w; im.recvH = h; im.recvParts = parts; im.recvMask = 0;
  }
  int len = n - 10;
  if (off + len > total) len = total - off;
  if (len > 0) memcpy(im.recv + off, d + 10, len);
  im.recvMask |= (1UL << part);
  uint32_t full = (parts >= 32) ? 0xFFFFFFFFUL : ((1UL << parts) - 1);
  if ((im.recvMask & full) == full) {             // 收齐了：换成显示用的图
    uint8_t *t = im.show; im.show = im.recv; im.recv = t;
    im.showW = im.recvW; im.showH = im.recvH;
    im.recvId = -1;
    navDirty = true;
    if (slot == 3 || slot == 4 || (slot >= 7 && slot <= 11)) wxDirty = true;
    if (slot == 5 || slot == 6 || slot == 12 || slot == 13 || slot == 14) musicDirty = true;                // 音乐文字图片收齐后刷新
  }
}

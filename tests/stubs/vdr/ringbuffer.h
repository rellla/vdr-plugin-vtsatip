#pragma once
#include "tools.h"
#include <vector>
#include <algorithm>
// Simplified, wrap-free ring buffer for tests: data is only reused when the buffer runs empty.
class cRingBufferLinear {
  std::vector<uchar> buf; size_t head, tail; int margin; int getTimeout;
  pthread_mutex_t m; pthread_cond_t c;
public:
  cRingBufferLinear(int Size, int Margin = 0, bool = false, const char * = NULL) : buf(Size), head(0), tail(0), margin(Margin ? Margin : 1), getTimeout(0) {
    pthread_mutex_init(&m, NULL); pthread_cond_init(&c, NULL); }
  void SetTimeouts(int, int Get) { getTimeout = Get; }
  void SetIoThrottle() {}
  int Put(const uchar *Data, int Count) {
    pthread_mutex_lock(&m);
    if (head == tail) head = tail = 0;
    int n = (int)std::min((size_t)Count, buf.size() - head);
    n -= n % 1;
    memcpy(&buf[head], Data, n); head += n;
    pthread_cond_broadcast(&c); pthread_mutex_unlock(&m); return n;
  }
  uchar *Get(int &Count) {
    pthread_mutex_lock(&m);
    if ((int)(head - tail) < margin && getTimeout) {
      struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += getTimeout / 1000; t.tv_nsec += (long)(getTimeout % 1000) * 1000000L;
      if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&c, &m, &t);
    }
    uchar *p = NULL;
    if ((int)(head - tail) >= margin) { Count = (int)(head - tail); p = &buf[tail]; }
    pthread_mutex_unlock(&m); return p;
  }
  void Del(int Count) { pthread_mutex_lock(&m); tail += std::min((size_t)Count, head - tail); pthread_mutex_unlock(&m); }
  void Clear() { pthread_mutex_lock(&m); head = tail = 0; pthread_mutex_unlock(&m); }
  int Available() { pthread_mutex_lock(&m); int a = (int)(head - tail); pthread_mutex_unlock(&m); return a; }
  int Free() { return (int)buf.size() - Available() - 1; }
  void ReportOverflow(int) {}
};

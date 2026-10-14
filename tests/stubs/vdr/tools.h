// Minimal functional stand-in for VDR's tools.h (test environment only)
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
typedef unsigned char uchar;
#define KILOBYTE(n) ((n) * 1024)
#define MEGABYTE(n) ((n) * 1024 * 1024)
#define TS_SIZE 188
#define TS_SYNC_BYTE 0x47
inline bool isempty(const char *s) { return !s || !*s; }
extern int StubLogLevel;
inline void StubLog(const char *p, const char *fmt, ...) {
  if (!StubLogLevel) return;
  va_list ap; va_start(ap, fmt); fprintf(stderr, "[%s] ", p); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
}
#define esyslog(...) StubLog("E", __VA_ARGS__)
#define isyslog(...) StubLog("I", __VA_ARGS__)
#define dsyslog(...) StubLog("D", __VA_ARGS__)
class cString {
  char *s;
public:
  cString(const char *S = NULL) : s(S ? strdup(S) : NULL) {}
  cString(const cString &o) : s(o.s ? strdup(o.s) : NULL) {}
  cString &operator=(const cString &o) { if (this != &o) { free(s); s = o.s ? strdup(o.s) : NULL; } return *this; }
  cString &operator=(const char *S) { free(s); s = S ? strdup(S) : NULL; return *this; }
  ~cString() { free(s); }
  operator const char *() const { return s; }
  const char *operator*() const { return s; }
  static cString sprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); char *b = NULL; if (vasprintf(&b, fmt, ap) < 0) b = NULL; va_end(ap);
    cString r; r.s = b; return r;
  }
};
class cTimeMs {
  uint64_t begin;
  uint64_t end;
public:
  static uint64_t Now() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
  cTimeMs(int Ms = 0) { Set(Ms); }
  void Set(int Ms = 0) { begin = Now(); end = begin + Ms; }
  bool TimedOut() const { return Now() >= end; }
  uint64_t Elapsed() const { return Now() - begin; }
};
class cMutex {
  friend class cCondVar;
  pthread_mutex_t m;
public:
  cMutex() { pthread_mutex_init(&m, NULL); }
  ~cMutex() { pthread_mutex_destroy(&m); }
  void Lock() { pthread_mutex_lock(&m); }
  void Unlock() { pthread_mutex_unlock(&m); }
};
class cMutexLock {
  cMutex *m;
public:
  cMutexLock(cMutex *M) : m(M) { m->Lock(); }
  ~cMutexLock() { m->Unlock(); }
};
class cCondVar {
  pthread_cond_t c;
public:
  cCondVar() { pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&c, &a); }
  ~cCondVar() { pthread_cond_destroy(&c); }
  void Wait(cMutex &M) { pthread_cond_wait(&c, &M.m); }
  bool TimedWait(cMutex &M, int Ms) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += Ms / 1000; t.tv_nsec += (long)(Ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c, &M.m, &t) == 0;
  }
  void Broadcast() { pthread_cond_broadcast(&c); }
};
class cCondWait {
public:
  static void SleepMs(int Ms) { usleep(Ms * 1000); }
};
#define DELETENULL(p) (delete (p), p = NULL)

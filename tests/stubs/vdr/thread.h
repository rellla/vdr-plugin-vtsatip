#pragma once
#include "tools.h"
class cThread {
  pthread_t t;
  bool started;
  volatile bool running;
  volatile bool active;
  static void *Entry(void *p) { cThread *c = (cThread *)p; c->active = true; c->Action(); c->active = false; return NULL; }
protected:
  virtual void Action(void) = 0;
public:
  cThread(const char *Description = NULL, bool = false) : started(false), running(false), active(false) { (void)Description; }
  virtual ~cThread() { Cancel(2); }
  bool Start() { if (started) return true; running = true; started = true; return pthread_create(&t, NULL, Entry, this) == 0; }
  bool Running() { return running; }
  bool Active() { return active; }
  void Cancel(int WaitSeconds = 0) {
    running = false;
    if (started) {
      for (int i = 0; i < WaitSeconds * 100 && active; ++i) usleep(10000);
      if (active) pthread_cancel(t);
      pthread_join(t, NULL);
      started = false;
    }
  }
};

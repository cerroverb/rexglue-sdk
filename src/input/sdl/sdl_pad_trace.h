/**
 * Pad-age trace: a diagnostic for how old the controller state is when the
 * game reads it.
 *
 * Off unless the environment variable DOA4_PADAGE_DIR names an existing
 * folder; then every read, refresh request and controller event of the SDL
 * driver writes one row to <folder>/padage_<process id>.csv. It answers "how
 * long does a press wait before the game sees it" on a player's own pad and
 * PC, without a special build.
 *
 * Off (the normal case): no file is opened, no clock is read, no lock is
 * taken and nothing is allocated; each site costs one branch on a flag that
 * is read from the environment once.
 *
 * Rows (times are microseconds on std::chrono::steady_clock):
 *   site "state"  a GetState call. t_enter_us = entry; t_copy_us = just
 *                 before the cached state is drained and copied; the pump
 *                 and round columns are sampled at t_copy_us.
 *   site "key"    a GetKeystroke call, as "state".
 *   site "queue"  a request for the UI thread's background refresh
 *                 (QueueControllerUpdate); caller = state, caps, raw, set or
 *                 key; queued_before = 1 means one was already queued.
 *   site "event"  a controller event reaching the driver. user = the SDL
 *                 event type, t_copy_us = SDL's own event time, active = it
 *                 arrived inside a marked UI-thread pump, queued_before = on
 *                 the pump's thread, in_round = inside a pad-thread round.
 * wait_us and wait_result are what the read's wait for a fresh round gave
 * ("none" where no wait was asked). sync_us is always 0 (a column of the
 * first traces, kept so old and new files read alike).
 * Rows are flushed every 256; a killed process loses what was not yet
 * written out, and its last line can be cut short (readers drop it).
 */
#pragma once

#include <cstdint>

namespace rex::input::sdl::padtrace {

// True when the trace is on. The first call reads the environment.
bool On();

// Around the UI thread's SDL_PumpEvents.
void PumpBegin();
void PumpEnd();

// Around one round of the pad refresh thread.
void RoundBegin();
void RoundEnd();

// A controller event reached the driver (any thread).
void Event(uint32_t sdl_event_type, uint64_t sdl_timestamp_ns);

// One row. Built on the stack by the call site; written when it goes out of
// scope, so declare it before the locks it must not be written under.
class Sample {
 public:
  Sample(const char* site, uint32_t user, const char* caller);
  ~Sample();
  Sample(const Sample&) = delete;
  Sample& operator=(const Sample&) = delete;

  // The pump and round columns and t_copy_us, as of now.
  void Snapshot();
  void SetWait(int64_t wait_us, const char* result);
  // This row is a refresh request that was queued: its time becomes queue_us.
  void MarkQueued();

  bool active = false;
  bool queued_before = false;
  uint32_t result = 0;

 private:
  friend void Event(uint32_t, uint64_t);
  const char* site_;
  const char* caller_;
  uint32_t user_;
  bool armed_;
  bool in_round_ = false;
  int64_t t_enter_ = 0, t_copy_ = 0;
  int64_t wait_us_ = 0;
  const char* wait_result_ = "none";
  uint64_t pumps_ = 0, events_ = 0, events_outside_ = 0, rounds_ = 0;
  int64_t queue_us_ = 0, pump_begin_ = 0, pump_end_ = 0, last_event_ = 0;
  int64_t round_begin_ = 0, round_end_ = 0;
  uint32_t pump_tid_ = 0;
};

}  // namespace rex::input::sdl::padtrace

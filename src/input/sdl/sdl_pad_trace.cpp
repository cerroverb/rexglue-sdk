/**
 * Pad-age trace: see sdl_pad_trace.h.
 */

#include "sdl_pad_trace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <rex/logging.h>

namespace rex::input::sdl::padtrace {

namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t Tid() {
  return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

long long ProcessId() {
#if defined(_WIN32)
  return static_cast<long long>(_getpid());
#else
  return static_cast<long long>(getpid());
#endif
}

struct Trace {
  bool on = false;
  std::FILE* file = nullptr;
  std::mutex mu;  // the file and `rows`
  uint64_t rows = 0;
  std::atomic<int64_t> queue_us{0}, pump_begin_us{0}, pump_end_us{0}, last_event_us{0};
  std::atomic<int64_t> round_begin_us{0}, round_end_us{0};
  std::atomic<uint64_t> pumps{0}, rounds{0}, events{0}, events_outside{0};
  std::atomic<uint32_t> pump_tid{0}, round_tid{0};
  std::atomic<bool> in_pump{false}, in_round{false};

  Trace() {
    const char* dir = std::getenv("DOA4_PADAGE_DIR");
    if (!dir || !*dir) {
      return;
    }
    const std::string path = std::string(dir) + "/padage_" + std::to_string(ProcessId()) + ".csv";
    file = std::fopen(path.c_str(), "w");
    if (!file) {
      REXLOG_ERROR("padage: cannot open {}", path);
      return;
    }
    std::fputs("seq,site,user,tid,active,queued_before,t_enter_us,t_copy_us,sync_us,pumps,queue_us,"
               "pump_begin_us,pump_end_us,pump_tid,events,events_outside,last_event_us,result,caller,"
               "wait_us,wait_result,rounds,round_begin_us,round_end_us,in_round\n",
               file);
    std::fflush(file);
    on = true;
    REXLOG_INFO("padage: trace on, file {}", path);
  }
};

Trace& T() {
  static Trace t;
  return t;
}

}  // namespace

bool On() {
  static const bool on = T().on;
  return on;
}

void PumpBegin() {
  if (!On()) {
    return;
  }
  Trace& t = T();
  t.pump_tid = Tid();
  t.pump_begin_us = NowUs();
  t.in_pump = true;
}

void PumpEnd() {
  if (!On()) {
    return;
  }
  Trace& t = T();
  t.in_pump = false;
  t.pump_end_us = NowUs();
  ++t.pumps;
}

void RoundBegin() {
  if (!On()) {
    return;
  }
  Trace& t = T();
  t.round_tid = Tid();
  t.round_begin_us = NowUs();
  t.in_round = true;
}

void RoundEnd() {
  if (!On()) {
    return;
  }
  Trace& t = T();
  t.in_round = false;
  t.round_end_us = NowUs();
  ++t.rounds;
}

void Event(uint32_t sdl_event_type, uint64_t sdl_timestamp_ns) {
  if (!On()) {
    return;
  }
  Trace& t = T();
  const uint32_t tid = Tid();
  const bool in_pump = t.in_pump.load();
  const bool pump_thread = tid == t.pump_tid.load();
  const bool in_round = t.in_round.load() && tid == t.round_tid.load();
  ++t.events;
  if (!(in_pump && pump_thread) && !in_round) {
    ++t.events_outside;
  }
  Sample row("event", sdl_event_type, "");
  t.last_event_us = row.t_enter_;
  row.active = in_pump;
  row.queued_before = pump_thread;
  row.in_round_ = in_round;
  row.Snapshot();
  row.t_copy_ = static_cast<int64_t>(sdl_timestamp_ns / 1000);
}

Sample::Sample(const char* site, uint32_t user, const char* caller)
    : site_(site), caller_(caller), user_(user), armed_(On()) {
  if (armed_) {
    t_enter_ = NowUs();
  }
}

void Sample::Snapshot() {
  if (!armed_) {
    return;
  }
  Trace& t = T();
  t_copy_ = NowUs();
  pumps_ = t.pumps;
  queue_us_ = t.queue_us;
  pump_begin_ = t.pump_begin_us;
  pump_end_ = t.pump_end_us;
  pump_tid_ = t.pump_tid;
  events_ = t.events;
  events_outside_ = t.events_outside;
  last_event_ = t.last_event_us;
  rounds_ = t.rounds;
  round_begin_ = t.round_begin_us;
  round_end_ = t.round_end_us;
}

void Sample::SetWait(int64_t wait_us, const char* result) {
  wait_us_ = wait_us;
  wait_result_ = result;
}

void Sample::MarkQueued() {
  if (armed_) {
    T().queue_us = t_enter_;
  }
}

Sample::~Sample() {
  if (!armed_) {
    return;
  }
  Trace& t = T();
  std::lock_guard<std::mutex> lock(t.mu);
  std::fprintf(t.file,
               "%llu,%s,%u,%u,%d,%d,%lld,%lld,0,%llu,%lld,%lld,%lld,%u,%llu,%llu,%lld,%u,%s,%lld,%s,%llu,"
               "%lld,%lld,%d\n",
               static_cast<unsigned long long>(t.rows), site_, user_, Tid(), active ? 1 : 0,
               queued_before ? 1 : 0, static_cast<long long>(t_enter_), static_cast<long long>(t_copy_),
               static_cast<unsigned long long>(pumps_), static_cast<long long>(queue_us_),
               static_cast<long long>(pump_begin_), static_cast<long long>(pump_end_), pump_tid_,
               static_cast<unsigned long long>(events_), static_cast<unsigned long long>(events_outside_),
               static_cast<long long>(last_event_), result, caller_, static_cast<long long>(wait_us_),
               wait_result_, static_cast<unsigned long long>(rounds_), static_cast<long long>(round_begin_),
               static_cast<long long>(round_end_), in_round_ ? 1 : 0);
  if ((++t.rows & 255) == 0) {
    std::fflush(t.file);
  }
}

}  // namespace rex::input::sdl::padtrace

/**
 * Pad-age trace: see sdl_pad_trace.h.
 */

#include "sdl_pad_trace.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include <rex/logging.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

// One CSV line, built on the writer's stack: no lock, no allocation, no C
// library stream or locale. A guest thread can be killed in the middle of a
// row (at a window close), and it must leave nothing held.
class Line {
 public:
  void Text(const char* s) {
    while (*s) {
      Char(*s++);
    }
  }
  void Char(char c) {
    if (n_ < sizeof(buf_) - 1) {  // the last byte is kept for the line's end
      buf_[n_++] = c;
    }
  }
  void Unsigned(uint64_t v) {
    char digits[20];
    int k = 0;
    do {
      digits[k++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0);
    while (k > 0) {
      Char(digits[--k]);
    }
  }
  void Signed(int64_t v) {
    if (v < 0) {
      Char('-');
      Unsigned(0 - static_cast<uint64_t>(v));
    } else {
      Unsigned(static_cast<uint64_t>(v));
    }
  }
  // Ends the line; returns its length with the line's end.
  size_t End() {
    buf_[n_++] = '\n';
    return n_;
  }
  const char* data() const { return buf_; }

 private:
  char buf_[1024];
  size_t n_ = 0;
};

constexpr char kHeader[] =
    "seq,site,user,tid,active,queued_before,t_enter_us,t_copy_us,sync_us,pumps,queue_us,"
    "pump_begin_us,pump_end_us,pump_tid,events,events_outside,last_event_us,result,caller,"
    "wait_us,wait_result,rounds,round_begin_us,round_end_us,in_round\n";

struct Trace {
  bool on = false;
  // Opened for append: each write lands whole at the file's end, so rows of
  // different threads never mix inside a line, with no lock of ours.
#if defined(_WIN32)
  HANDLE file = INVALID_HANDLE_VALUE;
#else
  int file = -1;
#endif
  std::atomic<uint64_t> rows{0};
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
#if defined(_WIN32)
    // Created (or emptied) first, then opened again with append access only.
    HANDLE created = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created != INVALID_HANDLE_VALUE) {
      CloseHandle(created);
      file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    const bool opened = file != INVALID_HANDLE_VALUE;
#else
    file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    const bool opened = file >= 0;
#endif
    if (!opened) {
      REXLOG_ERROR("padage: cannot open {}", path);
      return;
    }
    Write(kHeader, sizeof(kHeader) - 1);
    on = true;
    REXLOG_INFO("padage: trace on, file {}", path);
  }

  // One write call for the whole line.
  void Write(const char* data, size_t size) {
#if defined(_WIN32)
    DWORD written = 0;
    WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
#else
    (void)!::write(file, data, size);
#endif
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
  // The columns of kHeader, in its order.
  Line line;
  line.Unsigned(t.rows.fetch_add(1));
  line.Char(',');
  line.Text(site_);
  line.Char(',');
  line.Unsigned(user_);
  line.Char(',');
  line.Unsigned(Tid());
  line.Char(',');
  line.Unsigned(active ? 1 : 0);
  line.Char(',');
  line.Unsigned(queued_before ? 1 : 0);
  line.Char(',');
  line.Signed(t_enter_);
  line.Char(',');
  line.Signed(t_copy_);
  line.Text(",0,");  // sync_us
  line.Unsigned(pumps_);
  line.Char(',');
  line.Signed(queue_us_);
  line.Char(',');
  line.Signed(pump_begin_);
  line.Char(',');
  line.Signed(pump_end_);
  line.Char(',');
  line.Unsigned(pump_tid_);
  line.Char(',');
  line.Unsigned(events_);
  line.Char(',');
  line.Unsigned(events_outside_);
  line.Char(',');
  line.Signed(last_event_);
  line.Char(',');
  line.Unsigned(result);
  line.Char(',');
  line.Text(caller_);
  line.Char(',');
  line.Signed(wait_us_);
  line.Char(',');
  line.Text(wait_result_);
  line.Char(',');
  line.Unsigned(rounds_);
  line.Char(',');
  line.Signed(round_begin_);
  line.Char(',');
  line.Signed(round_end_);
  line.Char(',');
  line.Unsigned(in_round_ ? 1 : 0);
  const size_t size = line.End();
  t.Write(line.data(), size);
}

}  // namespace rex::input::sdl::padtrace

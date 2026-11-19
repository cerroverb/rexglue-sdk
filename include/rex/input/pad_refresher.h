/**
 * A thread that refreshes the controllers on request.
 *
 * The SDL driver reads a controller state that a background refresh filled
 * in; with one refresh a frame that state is a frame old when the game reads
 * it. PadRefresher runs the refresh ("a round") on its own thread when a
 * reader asks, and the reader waits for that round, up to a limit.
 *
 * Standard library only (no SDL, no other rex header), so that it builds
 * into a host test.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace rex::input {

class PadRefresher {
 public:
  enum class Result : uint8_t {
    kFresh,     // a round that began after this call has ended
    kRecent,    // a round ended less than recent_us ago: no new round
    kTimedOut,  // the round did not end within the limit
    kSkipped,   // rounds are slow (a caller's wait ran out): a round was asked for, no wait
    kStopped,   // no thread (start_thread false, no refresh function, or it could not start), or Stop() was called
  };

  struct Counts {
    uint64_t rounds;     // rounds ended
    uint64_t fresh;      // results returned, each call counted once
    uint64_t recent;
    uint64_t timed_out;
    uint64_t skipped;
    uint64_t stopped;
    // What rounds cost, on steady_clock in microseconds (0 before the first round).
    int64_t longest_round_us;        // the longest refresh call
    int64_t round_us_sum;            // all refresh calls together (mean = this / rounds)
    int64_t longest_start_delay_us;  // the longest time from a request to the start of the
                                     // round that served it (the newest request it served)
  };

  // What a session's closing line says about the cost of its rounds. A session whose
  // MEAN round is longer than kLongMeanRoundUs gets one WARN at close, once at least
  // kLongMeanMinRounds rounds ran: every frame's first read waits for its round. A
  // single long round is normal (SDL's battery query takes about 4 ms now and then,
  // device detection after an unplug about 200 ms), so the longest round is reported
  // and never warned about.
  static constexpr int64_t kLongMeanRoundUs = 500;
  static constexpr uint64_t kLongMeanMinRounds = 1000;
  struct RoundCost {
    int64_t mean_us;  // round_us_sum / rounds, rounded down; 0 when no round ran
    bool warn;        // rounds >= kLongMeanMinRounds and mean_us > kLongMeanRoundUs
  };
  static RoundCost RoundCostOf(const Counts& counts);

  struct Options {
    std::function<void()> refresh;       // one round; runs on the refresher's thread only
    std::function<void()> thread_setup;  // optional; runs once on that thread before the first wait
    std::function<int64_t()> now_us;     // optional; default steady_clock in microseconds; stamps rounds
    int64_t recent_us = 1000;
    // A waiting caller keeps its core this long before it starts to yield it
    // between looks. A round takes tens of microseconds; a yield on a busy PC
    // can cost a whole scheduler slice (measured: 13 ms).
    int64_t spin_us = 300;
    std::function<void()> yield;          // optional; default std::this_thread::yield (tests count it)
    std::function<void()> after_request;  // optional, for tests: runs on the caller's thread right
                                          // after its request is made, before it waits
    std::function<void()> after_noted;    // optional, for tests: runs on the refresher's thread right
                                          // after it read the request count, before it sleeps or
                                          // runs a round
    std::function<void()> at_deadline;    // optional, for tests: runs on the caller's thread when
                                          // its wait has reached the limit, before its last look
                                          // at whether its round has ended
    bool start_thread = true;           // false: behaves as a refresher whose thread could not start
  };

  explicit PadRefresher(Options options);
  ~PadRefresher();  // Stop()

  PadRefresher(const PadRefresher&) = delete;
  PadRefresher& operator=(const PadRefresher&) = delete;

  // The thread started and Stop() was not called.
  bool running() const;

  // Asks for a round and waits for it, `limit` at most. Never runs `refresh`
  // on the calling thread. From any thread but the refresher's own.
  //
  // When a wait runs out, rounds count as slow: that caller gets kTimedOut,
  // callers waiting beside it and later callers get kSkipped without a wait
  // (each still asks for a round), until a round has ended less than half
  // that limit after the request of a skipped call it served. So slow rounds,
  // and rounds that start late, cost one limit, not one a call.
  //
  // The object must outlive every call: destroy it only when no thread can
  // still be inside RefreshAndWait (Stop() makes calls return, it does not
  // wait for them to have returned).
  Result RefreshAndWait(std::chrono::microseconds limit);

  // Waiters leave at once and later calls return kStopped; then the thread
  // is joined (a round in flight is waited for). Idempotent; from any thread
  // but the refresher's own.
  //
  // No call takes a lock of this class or of the C++ library that another
  // thread could need: RefreshAndWait and counts() take none, and Stop() takes
  // only the one that orders two Stop() calls. So a caller's thread that is
  // killed inside RefreshAndWait (the guest threads are, at a window close)
  // leaves no lock of ours held. Not covered: whatever the OS's own wake call
  // holds for an instant, and any lock the refresh function takes (Stop()
  // waits for a round in flight).
  void Stop();

  Counts counts() const;
  // Counts::longest_round_us alone (one load). No caller in the driver: the driver reports
  // the round costs once, at close, from counts().
  int64_t longest_round_us() const;
  // 0 before the first round; on the now_us clock. Two separate reads: while
  // rounds run, a begin and an end read one after the other can belong to
  // different rounds.
  int64_t last_round_begin_us() const;
  int64_t last_round_end_us() const;

 private:
  void ThreadMain();
  int64_t NowUs() const;
  static int64_t SteadyUs();  // steady_clock in microseconds, whatever now_us is

  Options options_;
  bool started_ = false;  // set in the constructor only
  std::atomic<bool> stopped_{false};

  // Request numbers start at 1. A request is done when a round that began
  // after it was made has ended. A round's end is published in this order:
  // the two stamps, rounds_, then done_.
  std::atomic<uint64_t> requested_{0};
  std::atomic<uint64_t> done_{0};
  // The thread sleeps on this (std::atomic wait); every request and Stop()
  // change it and notify. 32 bits: the size the OS waits on directly.
  std::atomic<uint32_t> wake_{0};
  // Set when a caller's wait runs out, with that caller's limit; while it is
  // set nobody waits. A call that finds it set stores its request time in
  // slow_request_us_ before it makes the request; the thread clears slow_
  // after a round that ended less than half that limit after the stamp it
  // read with the round's request count. The caller's clock, not the round's
  // own duration: the thread cannot see how late it was woken.
  std::atomic<bool> slow_{false};
  std::atomic<int64_t> slow_limit_us_{0};
  std::atomic<int64_t> slow_request_us_{0};  // steady_clock us; 0 = none yet

  // Every request's time (steady_clock us), stored before the request is
  // made; the thread reads it with the request count, for the start delay.
  std::atomic<int64_t> request_us_{0};

  std::atomic<uint64_t> rounds_{0};
  std::atomic<int64_t> last_begin_us_{0};
  std::atomic<int64_t> last_end_us_{0};
  // Round costs; written by the thread only.
  std::atomic<int64_t> longest_round_us_{0};
  std::atomic<int64_t> round_us_sum_{0};
  std::atomic<int64_t> longest_start_delay_us_{0};

  std::atomic<uint64_t> fresh_{0};
  std::atomic<uint64_t> recent_{0};
  std::atomic<uint64_t> timed_out_{0};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<uint64_t> stopped_count_{0};

  std::mutex join_mutex_;  // two Stop() calls must not join at once
  std::thread thread_;
};

}  // namespace rex::input

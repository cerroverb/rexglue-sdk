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
#include <condition_variable>
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
  };

  struct Options {
    std::function<void()> refresh;       // one round; runs on the refresher's thread only
    std::function<void()> thread_setup;  // optional; runs once on that thread before the first wait
    std::function<int64_t()> now_us;     // optional; default steady_clock in microseconds; stamps rounds
    int64_t recent_us = 1000;
    bool start_thread = true;            // false: behaves as a refresher whose thread could not start
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
  // (each still asks for a round), until a round has taken less than half
  // that limit. So slow rounds cost one limit, not one a call.
  //
  // The object must outlive every call: destroy it only when no thread can
  // still be inside RefreshAndWait (Stop() makes calls return, it does not
  // wait for them to have returned).
  Result RefreshAndWait(std::chrono::microseconds limit);

  // Waiters leave at once and later calls return kStopped; then the thread
  // is joined (a round in flight is waited for). Idempotent; from any thread
  // but the refresher's own.
  void Stop();

  Counts counts() const;
  // 0 before the first round; on the now_us clock. Two separate reads: while
  // rounds run, a begin and an end read one after the other can belong to
  // different rounds.
  int64_t last_round_begin_us() const;
  int64_t last_round_end_us() const;

 private:
  void ThreadMain();
  int64_t NowUs() const;

  Options options_;
  bool started_ = false;  // set in the constructor only
  std::atomic<bool> stopped_{false};

  // Request numbers start at 1. A request is done when a round that began
  // after it was made has ended.
  mutable std::mutex mutex_;  // requested_, and the publication of a round's end
  std::condition_variable wake_;
  std::atomic<uint64_t> requested_{0};
  std::atomic<uint64_t> done_{0};
  // Set when a caller's wait runs out, with that caller's limit; while it is
  // set nobody waits. The thread clears it after a round that took less than
  // half that limit.
  std::atomic<bool> slow_{false};
  std::atomic<int64_t> slow_limit_us_{0};

  std::atomic<uint64_t> rounds_{0};
  std::atomic<int64_t> last_begin_us_{0};
  std::atomic<int64_t> last_end_us_{0};

  std::atomic<uint64_t> fresh_{0};
  std::atomic<uint64_t> recent_{0};
  std::atomic<uint64_t> timed_out_{0};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<uint64_t> stopped_count_{0};

  std::mutex join_mutex_;  // two Stop() calls must not join at once
  std::thread thread_;
};

}  // namespace rex::input

/**
 * A thread that refreshes the controllers on request.
 */

#include <rex/input/pad_refresher.h>

#include <system_error>
#include <utility>

namespace rex::input {

PadRefresher::PadRefresher(Options options) : options_(std::move(options)) {
  if (!options_.start_thread || !options_.refresh) {
    return;
  }
  try {
    thread_ = std::thread([this] { ThreadMain(); });
    started_ = true;
  } catch (const std::system_error&) {
    // No thread: running() stays false and every call returns kStopped.
  }
}

PadRefresher::~PadRefresher() { Stop(); }

bool PadRefresher::running() const {
  return started_ && !stopped_.load(std::memory_order_acquire);
}

int64_t PadRefresher::NowUs() const {
  if (options_.now_us) {
    return options_.now_us();
  }
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

PadRefresher::Result PadRefresher::RefreshAndWait(std::chrono::microseconds limit) {
  if (!running()) {
    ++stopped_count_;
    return Result::kStopped;
  }
  // A burst of reads (the game reads four users in a row) shares one round.
  if (rounds_.load(std::memory_order_acquire) != 0 &&
      NowUs() - last_end_us_.load(std::memory_order_acquire) < options_.recent_us) {
    ++recent_;
    return Result::kRecent;
  }
  uint64_t mine;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mine = requested_.load(std::memory_order_relaxed) + 1;
    requested_.store(mine, std::memory_order_release);
  }
  wake_.notify_one();

  // Rounds are slow (a device being plugged in can hold the refresh for a
  // long time, or every round takes long on this PC): one caller has paid
  // the limit, nobody waits again until a round has been quick. The request
  // above still stands, so the state goes on being refreshed in the
  // background and a quick round can be seen.
  if (slow_.load(std::memory_order_acquire)) {
    ++skipped_;
    return Result::kSkipped;
  }

  // Not a timed OS wait: on Windows one can take a whole timer tick (15.6 ms)
  // where a round takes tens of microseconds.
  const auto deadline = std::chrono::steady_clock::now() + limit;
  for (;;) {
    if (done_.load(std::memory_order_acquire) >= mine) {
      ++fresh_;
      return Result::kFresh;
    }
    if (stopped_.load(std::memory_order_acquire)) {
      ++stopped_count_;
      return Result::kStopped;
    }
    if (slow_.load(std::memory_order_acquire)) {
      // Another caller gave up on the round this one is waiting for.
      ++skipped_;
      return Result::kSkipped;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      slow_limit_us_.store(limit.count(), std::memory_order_release);
      slow_.store(true, std::memory_order_release);
      ++timed_out_;
      return Result::kTimedOut;
    }
    std::this_thread::yield();
  }
}

void PadRefresher::ThreadMain() {
  if (options_.thread_setup) {
    options_.thread_setup();
  }
  for (;;) {
    uint64_t noted;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] {
        return stopped_.load(std::memory_order_acquire) ||
               requested_.load(std::memory_order_relaxed) > done_.load(std::memory_order_relaxed);
      });
      if (stopped_.load(std::memory_order_acquire)) {
        return;
      }
      // Requests made from here on are not served by this round: it may
      // already have read the device when they arrive.
      noted = requested_.load(std::memory_order_relaxed);
    }
    int64_t begin = NowUs();
    const auto t0 = std::chrono::steady_clock::now();
    options_.refresh();
    const int64_t took_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    int64_t end = NowUs();
    // Waits come back after a round that took less than half the limit that
    // a caller gave up on.
    if (slow_.load(std::memory_order_acquire) &&
        took_us * 2 < slow_limit_us_.load(std::memory_order_acquire)) {
      slow_.store(false, std::memory_order_release);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_begin_us_.store(begin, std::memory_order_release);
      last_end_us_.store(end, std::memory_order_release);
      rounds_.fetch_add(1, std::memory_order_release);
      done_.store(noted, std::memory_order_release);
    }
  }
}

void PadRefresher::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_.store(true, std::memory_order_release);
  }
  wake_.notify_all();
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

PadRefresher::Counts PadRefresher::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Counts c;
  c.rounds = rounds_.load(std::memory_order_acquire);
  c.fresh = fresh_.load(std::memory_order_acquire);
  c.recent = recent_.load(std::memory_order_acquire);
  c.timed_out = timed_out_.load(std::memory_order_acquire);
  c.skipped = skipped_.load(std::memory_order_acquire);
  c.stopped = stopped_count_.load(std::memory_order_acquire);
  return c;
}

int64_t PadRefresher::last_round_begin_us() const {
  return last_begin_us_.load(std::memory_order_acquire);
}

int64_t PadRefresher::last_round_end_us() const {
  return last_end_us_.load(std::memory_order_acquire);
}

}  // namespace rex::input

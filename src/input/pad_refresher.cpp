/**
 * A thread that refreshes the controllers on request.
 */

#include <rex/input/pad_refresher.h>

#include <system_error>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define REX_PAD_REFRESHER_RELAX() _mm_pause()
#else
#define REX_PAD_REFRESHER_RELAX() ((void)0)
#endif

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
  // No lock here or anywhere below: this thread may be killed at any point
  // (see Stop() in the header).
  // Whether rounds are slow is read before the request is made: the round
  // this call asks for must not decide whether this call waits for it.
  const bool slow = slow_.load(std::memory_order_acquire);
  const uint64_t mine = requested_.fetch_add(1) + 1;
  wake_.fetch_add(1);
  wake_.notify_one();
  if (options_.after_request) {
    options_.after_request();
  }

  // Rounds are slow (a device being plugged in can hold the refresh for a
  // long time, or every round takes long on this PC): one caller has paid
  // the limit, nobody waits again until a round has been quick. The request
  // above still stands, so the state goes on being refreshed in the
  // background and a quick round can be seen.
  if (slow) {
    ++skipped_;
    return Result::kSkipped;
  }

  // Not a timed OS wait: on Windows one can take a whole timer tick (15.6 ms)
  // where a round takes tens of microseconds. And no yield while the spin
  // lasts: on a busy PC a yield hands the core to another thread for up to a
  // scheduler slice, and the round this call waits for is over long before.
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + limit;
  const auto spin_end = start + std::chrono::microseconds(options_.spin_us);
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
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      slow_limit_us_.store(limit.count(), std::memory_order_release);
      slow_.store(true, std::memory_order_release);
      ++timed_out_;
      return Result::kTimedOut;
    }
    if (now < spin_end) {
      REX_PAD_REFRESHER_RELAX();
    } else if (options_.yield) {
      options_.yield();
    } else {
      std::this_thread::yield();
    }
  }
}

void PadRefresher::ThreadMain() {
  if (options_.thread_setup) {
    options_.thread_setup();
  }
  uint64_t served = 0;  // done_, as this thread last stored it
  for (;;) {
    // Read wake_ first: a request made after this read changes it, so the
    // wait below returns at once and the request is not slept through.
    const uint32_t woken = wake_.load();
    if (stopped_.load(std::memory_order_acquire)) {
      return;
    }
    // Requests made from here on are not served by this round: it may
    // already have read the device when they arrive.
    const uint64_t noted = requested_.load();
    if (options_.after_noted) {
      options_.after_noted();
    }
    if (noted == served) {
      wake_.wait(woken);
      continue;
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
    last_begin_us_.store(begin);
    last_end_us_.store(end);
    rounds_.fetch_add(1);
    done_.store(noted);
    served = noted;
  }
}

void PadRefresher::Stop() {
  stopped_.store(true);
  wake_.fetch_add(1);
  wake_.notify_all();
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

PadRefresher::Counts PadRefresher::counts() const {
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

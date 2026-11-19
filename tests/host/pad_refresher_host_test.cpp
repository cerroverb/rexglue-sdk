// Host test for rex::input::PadRefresher (src/input/pad_refresher.cpp).
// Built and run by run_pad_refresher_test.bat; standard library only.
//
// The refresh function is a fake that records the thread of each round, counts rounds begun and
// ended, and can hold one chosen round at a gate until the test opens it. The clock is an atomic
// the test sets. Every CHECK runs on the main thread, and the number of CHECKs a run executes is
// fixed: a call that may block runs on a helper thread and the main thread waits a bounded time
// for it ("at once" = ready within 1 s).

#include <rex/input/pad_refresher.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using rex::input::PadRefresher;
using Result = PadRefresher::Result;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

static int g_checks = 0;
static int g_failures = 0;
static const char* g_test = "";
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s: %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond);    \
      std::fflush(stdout);                                                       \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

namespace {

constexpr Result kNoAnswer = static_cast<Result>(255);  // a helper call that did not return in time

struct Fake {
  std::atomic<int64_t> clock{5000};  // not 0: "0 before the first round" must differ from a stamp
  std::atomic<int> begun{0};
  std::atomic<int> ended{0};
  std::atomic<bool> inside{false};
  std::atomic<int> hold_round{0};    // the round (1 = first) that waits at the gate; 0 = none
  std::atomic<bool> gate_open{false};
  std::atomic<bool> gave_up{false};  // a held round left by its 30 s limit (the test forgot the gate)
  std::atomic<int64_t> advance_in_round{0};  // the clock moves by this much inside each round
  std::atomic<int> round_ms{0};              // each round takes this long (real time)
  std::atomic<int> setups{0};
  std::mutex mutex;
  std::vector<std::thread::id> round_threads;
  std::thread::id setup_thread;

  void Refresh() {
    int n = ++begun;
    {
      std::lock_guard<std::mutex> lock(mutex);
      round_threads.push_back(std::this_thread::get_id());
    }
    inside = true;
    if (n == hold_round.load()) {
      auto give_up = std::chrono::steady_clock::now() + seconds(30);
      while (!gate_open.load()) {
        if (std::chrono::steady_clock::now() > give_up) {
          gave_up = true;
          break;
        }
        std::this_thread::sleep_for(milliseconds(1));
      }
    }
    if (round_ms.load() > 0) {
      std::this_thread::sleep_for(milliseconds(round_ms.load()));
    }
    clock.fetch_add(advance_in_round.load());
    inside = false;
    ++ended;
  }

  void Setup() {
    std::lock_guard<std::mutex> lock(mutex);
    setup_thread = std::this_thread::get_id();
    ++setups;
  }

  PadRefresher::Options Options() {
    PadRefresher::Options o;
    o.refresh = [this] { Refresh(); };
    o.thread_setup = [this] { Setup(); };
    o.now_us = [this] { return clock.load(); };
    return o;
  }

  // Every round ran on one thread, and that thread is not `caller`.
  bool OneThreadNot(std::thread::id caller) {
    std::lock_guard<std::mutex> lock(mutex);
    if (round_threads.empty()) return false;
    for (std::thread::id id : round_threads) {
      if (id != round_threads[0] || id == caller) return false;
    }
    return true;
  }

  bool SetupOnRoundThread(std::thread::id caller) {
    std::lock_guard<std::mutex> lock(mutex);
    return !round_threads.empty() && setup_thread == round_threads[0] && setup_thread != caller;
  }
};

template <class Pred>
bool WaitUntil(Pred pred, int ms = 5000) {
  auto end = std::chrono::steady_clock::now() + milliseconds(ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(milliseconds(1));
  }
  return true;
}

std::future<Result> Call(PadRefresher& r, microseconds limit) {
  return std::async(std::launch::async, [&r, limit] { return r.RefreshAndWait(limit); });
}

template <class T>
bool AtOnce(std::future<T>& f, int ms = 1000) {
  return f.wait_for(milliseconds(ms)) == std::future_status::ready;
}

// The answer of a helper call, or kNoAnswer when it has not returned.
Result Answer(std::future<Result>& f, bool ready) { return ready ? f.get() : kNoAnswer; }

// Ends a test: no round stays held, no waiter stays waiting. Futures declared after the refresher
// are destroyed (and so joined) before it.
void Release(Fake& fake, PadRefresher& r) {
  fake.gate_open = true;
  r.Stop();
}

const microseconds kLong = seconds(10);  // a wait the test ends itself
const microseconds kPlain = seconds(2);  // a call made with no round held: a round takes microseconds

void NeverOnCallerThread() {
  g_test = "NeverOnCallerThread";
  Fake fake;
  PadRefresher r(fake.Options());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 2000;
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 2000;
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(fake.begun == 3 && fake.ended == 3);
  CHECK(r.counts().rounds == 3);
  CHECK(fake.OneThreadNot(std::this_thread::get_id()));
  Release(fake, r);
}

void ThreadSetupRunsOnceOnPadThread() {
  g_test = "ThreadSetupRunsOnceOnPadThread";
  Fake fake;
  PadRefresher r(fake.Options());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 2000;
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(fake.setups == 1);
  CHECK(fake.SetupOnRoundThread(std::this_thread::get_id()));
  Release(fake, r);
}

void BurstGetsOneRound() {
  g_test = "BurstGetsOneRound";
  Fake fake;
  PadRefresher r(fake.Options());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  fake.clock += 999;
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  // A recent answer asks for no round: none starts in the next 100 ms either.
  std::this_thread::sleep_for(milliseconds(100));
  CHECK(fake.begun == 1 && r.counts().rounds == 1);
  Release(fake, r);
}

// The first call is never "recent": no round has ended, whatever the clock says (here it is
// below recent_us, so "now - 0 < recent_us" would hold).
void FirstCallIsNotRecent() {
  g_test = "FirstCallIsNotRecent";
  Fake fake;
  fake.clock = 500;
  PadRefresher r(fake.Options());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(fake.begun == 1);
  Release(fake, r);
}

// The clock moves 7 inside each round, so a round's begin and end stamps differ and "recent" is
// seen to count from the END of the round (from its begin, 6006 would already be 1006 later).
void RecentBoundary() {
  g_test = "RecentBoundary";
  Fake fake;
  fake.advance_in_round = 7;
  PadRefresher r(fake.Options());
  CHECK(r.last_round_begin_us() == 0 && r.last_round_end_us() == 0);
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(r.last_round_begin_us() == 5000 && r.last_round_end_us() == 5007);
  fake.clock = 6006;  // 999 after the end
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  fake.clock = 6007;  // exactly 1000 after the end: not recent
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(r.counts().rounds == 2);
  CHECK(r.last_round_begin_us() == 6007 && r.last_round_end_us() == 6014);
  Release(fake, r);
}

// Y asks while round 1 runs: round 1 may hold events from before Y's request only, so Y must get
// round 2. Y's request has been made when the gate opens (the after_request hook counts requests:
// X's is the first, Y's the second). Y's own thread counts the rounds ended at the moment its call
// returns, and every round takes 100 ms from the gate on, so a Y that was answered by round 1 is
// seen with one round ended.
void RoundStartsAfterRequest() {
  g_test = "RoundStartsAfterRequest";
  Fake fake;
  fake.hold_round = 1;
  std::atomic<int> requests{0};
  PadRefresher::Options o = fake.Options();
  o.after_request = [&] { ++requests; };
  PadRefresher r(std::move(o));
  std::future<Result> x = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load() && fake.begun == 1; }));
  fake.clock += 2000;
  std::atomic<int> ended_when_y_returned{-1};
  std::future<Result> y = std::async(std::launch::async, [&] {
    Result result = r.RefreshAndWait(kLong);
    ended_when_y_returned = fake.ended.load();
    return result;
  });
  CHECK(WaitUntil([&] { return requests.load() == 2; }));
  CHECK(fake.inside.load() && fake.ended == 0);  // round 1 is still held with both waiting
  fake.round_ms = 100;
  fake.gate_open = true;
  bool y_ready = AtOnce(y, 5000);
  uint64_t rounds_after_y = r.counts().rounds;
  CHECK(Answer(y, y_ready) == Result::kFresh);
  CHECK(ended_when_y_returned == 2 && rounds_after_y == 2);
  bool x_ready = AtOnce(x, 5000);
  CHECK(Answer(x, x_ready) == Result::kFresh);
  Release(fake, r);
}

// Review focus 1: a round that does not come back. One caller waits its limit once; nobody waits
// again until that round has ended; then waits resume.
void StuckRound() {
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.Options());

  g_test = "WaitIsBounded";
  std::future<std::pair<Result, int64_t>> timed =
      std::async(std::launch::async, [&]() -> std::pair<Result, int64_t> {
        auto t0 = std::chrono::steady_clock::now();
        Result res = r.RefreshAndWait(milliseconds(50));
        return {res, std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - t0)
                         .count()};
      });
  bool timed_ready = AtOnce(timed);
  std::pair<Result, int64_t> timed_answer =
      timed_ready ? timed.get() : std::pair<Result, int64_t>{kNoAnswer, 0};
  CHECK(timed_ready);
  CHECK(timed_answer.first == Result::kTimedOut);
  CHECK(timed_answer.second >= 50000);
  CHECK(timed_answer.second < 300000);  // the limit from above: 50 ms, not a multiple of it
  CHECK(fake.inside.load() && fake.begun == 1 && fake.ended == 0);

  g_test = "NoSecondWaitWhileRoundOut";
  fake.clock += 2000;
  std::future<Result> second = Call(r, kLong);
  bool second_ready = AtOnce(second);
  CHECK(second_ready);
  CHECK(Answer(second, second_ready) == Result::kSkipped);
  CHECK(fake.begun == 1 && fake.ended == 0);

  // The stuck round ends (it was slow: waits stay off), then the round the skipped call asked
  // for runs and is quick: waits are back on.
  g_test = "WaitsResumeAfterRoundEnds";
  fake.gate_open = true;
  CHECK(WaitUntil([&] { return r.counts().rounds >= 2; }));
  CHECK(fake.begun == 2);
  fake.clock += 2000;
  std::future<Result> third = Call(r, kLong);
  bool third_ready = AtOnce(third);
  CHECK(Answer(third, third_ready) == Result::kFresh);
  CHECK(fake.ended == 3 && r.counts().rounds == 3);
  Release(fake, r);
}

// Two callers wait on one stuck round: when the first gives up, the other leaves too, so the
// round costs one limit, not one limit a caller.
void WaitersLeaveWhenOneTimesOut() {
  g_test = "WaitersLeaveWhenOneTimesOut";
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.Options());
  std::future<Result> patient = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load(); }));
  std::future<Result> brief = Call(r, milliseconds(50));
  bool brief_ready = AtOnce(brief);
  CHECK(Answer(brief, brief_ready) == Result::kTimedOut);
  bool patient_ready = AtOnce(patient);
  bool held = fake.inside.load() && fake.ended == 0;
  CHECK(patient_ready);
  CHECK(Answer(patient, patient_ready) == Result::kSkipped);
  CHECK(held);
  Release(fake, r);
}

// Rounds that are slow every time (100 ms each, limit 40 ms): one call pays the limit, the calls
// after it do not wait but still ask for a round, so the state keeps being refreshed; when
// rounds are quick again the waits come back. (Spec: "a stuck round costs 2 ms once, not every
// frame".)
void SlowRoundsCostOneLimit() {
  g_test = "SlowRoundsCostOneLimit";
  Fake fake;
  fake.round_ms = 100;
  PadRefresher r(fake.Options());
  Result first = r.RefreshAndWait(milliseconds(40));
  bool rounds_kept_coming = true;
  int skipped = 0;
  for (int i = 1; i <= 8; ++i) {
    // Each call below is made after the round the call before it asked for has ended.
    rounds_kept_coming = WaitUntil([&] { return r.counts().rounds >= uint64_t(i); }) && rounds_kept_coming;
    fake.clock += 2000;
    if (r.RefreshAndWait(milliseconds(40)) == Result::kSkipped) ++skipped;
  }
  CHECK(first == Result::kTimedOut);
  CHECK(rounds_kept_coming);
  CHECK(skipped == 8);
  CHECK(r.counts().timed_out == 1);
  // Quick rounds again: the next call still does not wait, its round is quick, the one after waits.
  bool ninth = WaitUntil([&] { return r.counts().rounds >= 9; });
  fake.round_ms = 0;
  fake.clock += 2000;
  Result still_off = r.RefreshAndWait(milliseconds(40));
  bool tenth = WaitUntil([&] { return r.counts().rounds >= 10; });
  fake.clock += 2000;
  Result back_on = r.RefreshAndWait(kPlain);
  CHECK(ninth && tenth);
  CHECK(still_off == Result::kSkipped);
  CHECK(back_on == Result::kFresh);
  CHECK(r.counts().rounds == 11);
  Release(fake, r);
}

// Review focus 2: exit while a round is in flight and a caller waits.
void StopReleasesWaiter() {
  g_test = "StopReleasesWaiter";
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.Options());
  std::future<Result> a = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load(); }));
  std::future<void> b = std::async(std::launch::async, [&] { r.Stop(); });
  bool a_ready = AtOnce(a);
  bool held_when_a_returned = fake.inside.load() && fake.ended == 0;
  CHECK(a_ready);
  CHECK(Answer(a, a_ready) == Result::kStopped);
  CHECK(held_when_a_returned);
  fake.gate_open = true;
  CHECK(AtOnce(b));
  CHECK(!fake.gave_up);
  Release(fake, r);
}

// Stop() waits for the round in flight: it has not returned while the round is held, and when it
// returns the refresh function is not running and does not run again.
void StopJoins() {
  g_test = "StopJoins";
  Fake fake;
  fake.hold_round = 1;
  PadRefresher r(fake.Options());
  std::future<Result> a = Call(r, kLong);
  CHECK(WaitUntil([&] { return fake.inside.load(); }));
  std::atomic<bool> stop_entered{false};
  std::future<bool> b = std::async(std::launch::async, [&] {
    stop_entered = true;
    r.Stop();
    return fake.inside.load();
  });
  CHECK(WaitUntil([&] { return stop_entered.load(); }));
  CHECK(!AtOnce(b, 200));  // Stop() is waiting for the held round
  fake.gate_open = true;
  bool b_ready = AtOnce(b);
  bool inside_when_stop_returned = b_ready ? b.get() : true;
  CHECK(b_ready);
  CHECK(!inside_when_stop_returned);
  int begun = fake.begun.load();
  int ended = fake.ended.load();
  std::this_thread::sleep_for(milliseconds(50));
  CHECK(begun == 1 && ended == 1 && fake.begun == 1 && fake.ended == 1 && !fake.inside);
  Release(fake, r);
}

// The clock does not move after the round, so a call after Stop() would be "recent" if the
// stopped rule did not come first.
void AfterStopReturnsAtOnce() {
  g_test = "AfterStopReturnsAtOnce";
  Fake fake;
  PadRefresher r(fake.Options());
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(r.running());
  r.Stop();
  CHECK(!r.running());
  std::future<Result> late = Call(r, kLong);
  bool late_ready = AtOnce(late);
  CHECK(late_ready);
  CHECK(Answer(late, late_ready) == Result::kStopped);
  fake.clock += 2000;
  std::future<Result> later = Call(r, kLong);
  bool later_ready = AtOnce(later);
  CHECK(Answer(later, later_ready) == Result::kStopped);
  CHECK(fake.begun == 1 && r.counts().rounds == 1);
  std::future<void> again = std::async(std::launch::async, [&] { r.Stop(); });
  CHECK(AtOnce(again));
  Release(fake, r);
}

void NotStartedReturnsAtOnce() {
  g_test = "NotStartedReturnsAtOnce";
  Fake fake;
  std::future<int> whole = std::async(std::launch::async, [&] {
    PadRefresher::Options o = fake.Options();
    o.start_thread = false;
    PadRefresher r(std::move(o));
    int bits = 0;
    if (!r.running()) bits |= 1;
    if (r.RefreshAndWait(kLong) == Result::kStopped) bits |= 2;
    if (r.counts().rounds == 0 && r.counts().stopped == 1) bits |= 4;
    return bits;  // the destructor runs here
  });
  bool ready = AtOnce(whole);
  int bits = ready ? whole.get() : 0;
  CHECK(ready);
  CHECK((bits & 1) != 0);
  CHECK((bits & 2) != 0);
  CHECK((bits & 4) != 0);
  CHECK(fake.begun == 0 && fake.setups == 0);
}

// The driver gives no clock: the default one stamps rounds (recent_us is a minute here, so the
// second call is recent whatever the PC's load).
void DefaultClockStampsRounds() {
  g_test = "DefaultClockStampsRounds";
  Fake fake;
  PadRefresher::Options o = fake.Options();
  o.now_us = nullptr;
  o.recent_us = 60'000'000;
  PadRefresher r(std::move(o));
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(r.last_round_begin_us() > 0 && r.last_round_end_us() >= r.last_round_begin_us());
  CHECK(r.RefreshAndWait(kPlain) == Result::kRecent);
  Release(fake, r);
}

void CountsMatch() {
  g_test = "CountsMatch";
  Fake fake;
  PadRefresher r(fake.Options());
  Result seq[8];
  seq[0] = r.RefreshAndWait(kPlain);  // fresh
  seq[1] = r.RefreshAndWait(kPlain);  // recent (the clock has not moved)
  seq[2] = r.RefreshAndWait(kPlain);  // recent
  seq[3] = r.RefreshAndWait(kPlain);  // recent
  fake.clock += 2000;
  seq[4] = r.RefreshAndWait(kPlain);  // fresh
  fake.clock += 2000;
  fake.hold_round = 3;
  std::future<Result> t = Call(r, milliseconds(50));
  bool t_ready = AtOnce(t);
  seq[5] = Answer(t, t_ready);        // timed out, round 3 held
  std::future<Result> s1 = Call(r, kLong);
  bool s1_ready = AtOnce(s1);
  seq[6] = Answer(s1, s1_ready);      // skipped
  std::future<Result> s2 = Call(r, kLong);
  bool s2_ready = AtOnce(s2);
  seq[7] = Answer(s2, s2_ready);      // skipped
  fake.gate_open = true;
  // Round 3 was the held one; round 4 is the one the skipped calls asked for.
  bool round4_ended = WaitUntil([&] { return r.counts().rounds >= 4; });
  r.Stop();
  Result last = r.RefreshAndWait(kPlain);  // stopped
  CHECK(seq[0] == Result::kFresh && seq[4] == Result::kFresh);
  CHECK(seq[1] == Result::kRecent && seq[2] == Result::kRecent && seq[3] == Result::kRecent);
  CHECK(seq[5] == Result::kTimedOut);
  CHECK(seq[6] == Result::kSkipped && seq[7] == Result::kSkipped);
  CHECK(round4_ended);
  CHECK(last == Result::kStopped);
  PadRefresher::Counts c = r.counts();
  CHECK(c.rounds == 4);
  CHECK(c.fresh == 2);
  CHECK(c.recent == 3);
  CHECK(c.timed_out == 1);
  CHECK(c.skipped == 2);
  CHECK(c.stopped == 1);
  Release(fake, r);
}

// A waiting caller keeps its core while the spin lasts: a yield on a busy PC can cost a whole
// scheduler slice (measured: 13 ms for a round that took 20 us). The spin is 2 s here and the
// round 20 ms, so the call never reaches a yield whatever the PC's load.
void SpinKeepsTheCore() {
  g_test = "SpinKeepsTheCore";
  Fake fake;
  fake.round_ms = 20;
  std::atomic<int> yields{0};
  PadRefresher::Options o = fake.Options();
  o.spin_us = 2'000'000;
  o.yield = [&] {
    ++yields;
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  CHECK(r.RefreshAndWait(kLong) == Result::kFresh);
  CHECK(yields == 0);
  CHECK(fake.ended == 1);
  Release(fake, r);
}

// After the spin the caller gives the core away each turn (a long round must not burn a core for
// the whole limit), and not before: the first yield comes no earlier than the spin's length.
void YieldsAfterTheSpin() {
  g_test = "YieldsAfterTheSpin";
  Fake fake;
  fake.round_ms = 100;
  std::atomic<int> yields{0};
  std::atomic<int64_t> first_yield_us{-1};
  std::chrono::steady_clock::time_point start;
  PadRefresher::Options o = fake.Options();
  o.spin_us = 5000;
  o.yield = [&] {
    if (yields.fetch_add(1) == 0) {
      first_yield_us = std::chrono::duration_cast<microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    }
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  start = std::chrono::steady_clock::now();
  CHECK(r.RefreshAndWait(kLong) == Result::kFresh);
  CHECK(yields > 0);
  CHECK(first_yield_us >= 5000);
  Release(fake, r);
}

// A spin of 0 is the plain yield loop: the first turn that finds the round unfinished yields.
// Twenty calls, each with a 5 ms round; the earliest first yield of the twenty is taken, so a busy
// PC must delay every one of them to fail this, while a spin of any length (0 read as a default,
// say) keeps every first yield at or above that length.
void NoSpinYieldsAtOnce() {
  g_test = "NoSpinYieldsAtOnce";
  Fake fake;
  fake.round_ms = 5;
  int yields = 0;           // the hook runs on the calling thread, which is this one
  int64_t first_yield_us = -1;
  std::chrono::steady_clock::time_point start;
  PadRefresher::Options o = fake.Options();
  o.spin_us = 0;
  o.yield = [&] {
    ++yields;
    if (first_yield_us < 0) {
      first_yield_us = std::chrono::duration_cast<microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    }
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  int fresh = 0;
  int64_t earliest = -1;
  for (int i = 0; i < 20; ++i) {
    fake.clock += 2000;
    first_yield_us = -1;
    start = std::chrono::steady_clock::now();
    if (r.RefreshAndWait(kLong) == Result::kFresh) ++fresh;
    if (first_yield_us >= 0 && (earliest < 0 || first_yield_us < earliest)) earliest = first_yield_us;
  }
  std::printf("note NoSpinYieldsAtOnce: earliest first yield %lld us, yields %d\n",
              static_cast<long long>(earliest), yields);
  CHECK(fresh == 20);
  CHECK(yields > 0);
  CHECK(earliest >= 0 && earliest < 150);
  Release(fake, r);
}

// The spin is a spin: nothing in it gives the core away or sleeps. The yield hook cannot see a
// sleep, so this one measures: 51 calls, the refresher's thread asleep before each (1 ms pause),
// the spin 2 s. A call then lasts as long as the thread's wake and a round (tens of
// microseconds); a sleep in the spin makes every call that finds its round unfinished at the
// first look last the sleep's length. The median is judged, so a busy PC must delay half the calls
// to fail this. Not seen here: a bare yield in the spin on a quiet PC (it returns at once); that
// is what the runs under made load are for.
void SpinDoesNotSleep() {
  g_test = "SpinDoesNotSleep";
  Fake fake;
  std::atomic<int> yields{0};
  PadRefresher::Options o = fake.Options();
  o.recent_us = 0;
  o.spin_us = 2'000'000;
  o.yield = [&] {
    ++yields;
    std::this_thread::yield();
  };
  PadRefresher r(std::move(o));
  std::vector<int64_t> us;
  int fresh = 0;
  for (int i = 0; i < 51; ++i) {
    std::this_thread::sleep_for(milliseconds(1));
    auto t0 = std::chrono::steady_clock::now();
    if (r.RefreshAndWait(kPlain) == Result::kFresh) ++fresh;
    us.push_back(
        std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(us.begin(), us.end());
  std::printf("note SpinDoesNotSleep: call us min %lld, median %lld, max %lld\n",
              static_cast<long long>(us[0]), static_cast<long long>(us[25]),
              static_cast<long long>(us[50]));
  CHECK(fresh == 51);
  CHECK(yields == 0);
  CHECK(us[25] < 500);
  Release(fake, r);
}

// Whether rounds are slow is read BEFORE a call makes its request. Rounds are slow (one call ran
// out); then rounds are quick, and the call's own round has ended, and cleared "slow", before the
// call goes on (the after_request hook waits for round 2). The call must not wait for a round
// because that round was quick: it answers kSkipped. Read after the request, "slow" is already
// clear and the answer is kFresh.
void SlowIsReadBeforeTheRequest() {
  g_test = "SlowIsReadBeforeTheRequest";
  Fake fake;
  fake.round_ms = 100;
  std::atomic<bool> armed{false};
  std::atomic<bool> saw_round_2{false};
  std::atomic<PadRefresher*> rp{nullptr};
  PadRefresher::Options o = fake.Options();
  // Round 2 is seen by its end stamp (7000 on the test's clock; round 1's is 5000), which the
  // thread stores after it cleared "slow". Not by counts(): the hook must use nothing a caller
  // could be holding.
  o.after_request = [&] {
    if (!armed.load()) return;
    saw_round_2 = WaitUntil([&] { return rp.load()->last_round_end_us() == 7000; });
  };
  PadRefresher r(std::move(o));
  rp = &r;
  CHECK(r.RefreshAndWait(milliseconds(40)) == Result::kTimedOut);
  CHECK(WaitUntil([&] { return r.counts().rounds >= 1; }));
  fake.round_ms = 0;
  fake.clock += 2000;
  armed = true;
  Result result = r.RefreshAndWait(kPlain);
  armed = false;
  CHECK(saw_round_2);
  CHECK(result == Result::kSkipped);
  Release(fake, r);
}

// A request made while the refresher's thread is between "nothing to do" and going to sleep is
// not slept through. The after_noted hook holds the thread right after its first read of the
// request count (0: nothing to do) until the caller's request has been made. The thread read its
// wake word before that, so its sleep returns at once and the round runs; had it read the wake
// word after the request count, it would sleep on the new value and the caller would pay its
// whole limit.
void RequestBeforeTheSleepIsSeen() {
  g_test = "RequestBeforeTheSleepIsSeen";
  Fake fake;
  std::atomic<int> noted_calls{0};
  std::atomic<bool> at_hook{false};
  std::atomic<bool> requested{false};
  std::atomic<bool> hook_gave_up{false};
  PadRefresher::Options o = fake.Options();
  o.after_noted = [&] {
    if (noted_calls.fetch_add(1) != 0) return;  // the first look only
    at_hook = true;
    auto give_up = std::chrono::steady_clock::now() + seconds(30);
    while (!requested.load()) {
      if (std::chrono::steady_clock::now() > give_up) {
        hook_gave_up = true;
        break;
      }
      std::this_thread::sleep_for(milliseconds(1));
    }
  };
  o.after_request = [&] { requested = true; };
  PadRefresher r(std::move(o));
  CHECK(WaitUntil([&] { return at_hook.load(); }));
  CHECK(fake.begun == 0);
  CHECK(r.RefreshAndWait(kPlain) == Result::kFresh);
  CHECK(!hook_gave_up && fake.ended == 1);
  Release(fake, r);
}

// A caller that stops for ever right after it made its request (at a window close the guest
// threads are killed wherever they are) holds nothing the others need: counts() answers, the
// round it asked for runs and Stop() returns. A guard, not a proof: only the one place where
// `after_request` runs is covered; the mutant that puts a lock around the request shows the test
// can fail.
void StuckCallerBlocksNobody() {
  g_test = "StuckCallerBlocksNobody";
  Fake fake;
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  PadRefresher::Options o = fake.Options();
  o.after_request = [&] {
    entered = true;
    auto give_up = std::chrono::steady_clock::now() + seconds(30);
    while (!release.load() && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(milliseconds(1));
    }
  };
  PadRefresher r(std::move(o));
  std::future<Result> a = Call(r, kLong);
  bool a_entered = WaitUntil([&] { return entered.load(); });
  CHECK(a_entered);
  std::future<uint64_t> c = std::async(std::launch::async, [&] { return r.counts().fresh; });
  bool c_ready = AtOnce(c);
  CHECK(c_ready);
  CHECK(WaitUntil([&] { return fake.ended.load() >= 1; }));
  std::future<void> s = std::async(std::launch::async, [&] { r.Stop(); });
  bool s_ready = AtOnce(s);
  CHECK(s_ready);
  bool stuck_until_now = a.wait_for(milliseconds(0)) != std::future_status::ready;
  CHECK(stuck_until_now);
  release = true;
  bool a_ready = AtOnce(a);
  CHECK(a_ready);
  CHECK(Answer(a, a_ready) == Result::kFresh);  // its round had ended: done comes before stopped
  Release(fake, r);
}

}  // namespace

int main() {
  NeverOnCallerThread();
  ThreadSetupRunsOnceOnPadThread();
  BurstGetsOneRound();
  FirstCallIsNotRecent();
  RecentBoundary();
  RoundStartsAfterRequest();
  StuckRound();
  WaitersLeaveWhenOneTimesOut();
  SlowRoundsCostOneLimit();
  StopReleasesWaiter();
  StopJoins();
  AfterStopReturnsAtOnce();
  NotStartedReturnsAtOnce();
  DefaultClockStampsRounds();
  CountsMatch();
  SpinKeepsTheCore();
  YieldsAfterTheSpin();
  NoSpinYieldsAtOnce();
  SpinDoesNotSleep();
  SlowIsReadBeforeTheRequest();
  RequestBeforeTheSleepIsSeen();
  StuckCallerBlocksNobody();
  if (g_failures) {
    std::printf("pad_refresher: FAILED\n");
  } else {
    std::printf("pad_refresher: all checks passed\n");
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

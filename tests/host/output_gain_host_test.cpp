// Host test for the output stage's volume and limiter (rex/audio/output_gain.h).
// Built and run by run_output_gain_test.bat.

#include <rex/audio/output_gain.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::fflush(stdout);                                        \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace rex::audio;

static bool Near(float a, float b) {
  return std::fabs(a - b) < 1e-5f;
}

static float Peak(const std::vector<float>& v) {
  float p = 0;
  for (float s : v) p = std::fmax(p, std::fabs(s));
  return p;
}

// The gain for a volume setting and a stream layout.
static void TestGainFor() {
  CHECK(OutputGainFor(100, 6) == 1.0f);
  // Stereo: the downmix (rex/audio/conversion.h) divides its sum by 2.5; the gain gives that back.
  CHECK(OutputGainFor(100, 2) == 2.5f);
  CHECK(OutputGainFor(200, 2) == 5.0f);
  CHECK(OutputGainFor(50, 6) == 0.5f);
  CHECK(OutputGainFor(0, 2) == 0.0f);
  CHECK(OutputGainFor(0, 6) == 0.0f);
  // Both edges of the range (0 and 200 are the cvar's documented limits).
  CHECK(Near(OutputGainFor(200, 6), 2.0f));
  CHECK(Near(OutputGainFor(201, 6), 2.0f));  // not 2.01: clamped
  CHECK(Near(OutputGainFor(300, 6), 2.0f));  // not 3.0: the old top
  CHECK(Near(OutputGainFor(199, 6), 1.99f));
  CHECK(OutputGainFor(-1, 6) == 0.0f);  // not -0.01: clamped
  CHECK(Near(OutputGainFor(1, 6), 0.01f));
  CHECK(kVolumePercentMin == 0 && kVolumePercentMax == 200 && kVolumePercentDefault == 100);
}

// At gain 1 with nothing over the ceiling the samples are untouched, bit for bit.
static void TestUnity() {
  const std::vector<float> in = {0.0f, 0.1f, -0.9f, 0.97f, -0.333333f, 1e-7f};
  std::vector<float> out = in;
  OutputGain stage;
  CHECK(!stage.Process(out.data(), out.size(), 1.0f));
  CHECK(std::memcmp(out.data(), in.data(), in.size() * sizeof(float)) == 0);
  CHECK(stage.reduction() == 1.0f);
}

static void TestScales() {
  std::vector<float> v = {0.1f, -0.2f, 0.3f, 0.0f};
  OutputGain stage;
  CHECK(!stage.Process(v.data(), v.size(), 2.5f));
  CHECK(Near(v[0], 0.25f) && Near(v[1], -0.5f) && Near(v[2], 0.75f) && v[3] == 0.0f);
  CHECK(!Near(v[0], 0.1f));  // not the input: the gain was applied
  CHECK(stage.reduction() == 1.0f);

  std::vector<float> z = {0.5f, -0.9f, 0.25f};
  CHECK(!stage.Process(z.data(), z.size(), 0.0f));
  CHECK(z[0] == 0.0f && z[1] == 0.0f && z[2] == 0.0f);
}

// A chunk whose peak times gain passes the ceiling is scaled as a whole, in that same chunk.
static void TestAttack() {
  std::vector<float> v = {0.4f, -0.2f, 0.8f, 0.1f};
  OutputGain stage;
  CHECK(stage.Process(v.data(), v.size(), 2.0f));
  // 0.8 * 2 = 1.6 -> the whole chunk times 0.98 / 1.6 = 0.6125.
  CHECK(Near(stage.reduction(), 0.6125f));
  CHECK(Near(v[2], 0.98f));
  CHECK(Near(v[0], 0.49f));    // not 0.8: a hard clip would leave 0.4 * 2 alone
  CHECK(Near(v[1], -0.245f));
  CHECK(Near(v[3], 0.1225f));
  CHECK(Peak(v) <= OutputGain::kCeiling + 1e-6f);

  // A peak exactly at the ceiling is not limited; one just over it is.
  std::vector<float> at = {0.49f};
  OutputGain a;
  CHECK(!a.Process(at.data(), at.size(), 2.0f));
  CHECK(Near(at[0], 0.98f) && a.reduction() == 1.0f);
  std::vector<float> over = {0.5f};
  OutputGain o;
  CHECK(o.Process(over.data(), over.size(), 2.0f));
  CHECK(Near(over[0], 0.98f) && o.reduction() < 1.0f);  // not 1.0: limited
}

// After a loud chunk the reduction relaxes by a fixed step per chunk, ramped inside the chunk.
static void TestRelease() {
  OutputGain stage;
  std::vector<float> loud = {0.8f, 0.8f};
  stage.Process(loud.data(), loud.size(), 2.0f);
  CHECK(Near(stage.reduction(), 0.6125f));

  std::vector<float> quiet(4, 0.1f);
  CHECK(stage.Process(quiet.data(), quiet.size(), 2.0f));  // still reducing
  CHECK(Near(stage.reduction(), 0.6225f));                 // not 1.0: no instant release
  // The ramp runs from the old value towards the new one and ends on it.
  CHECK(Near(quiet[3], 0.2f * 0.6225f));
  CHECK(Near(quiet[0], 0.2f * (0.6125f + 0.01f * 0.25f)));
  CHECK(quiet[0] < quiet[1] && quiet[1] < quiet[2] && quiet[2] < quiet[3]);
  CHECK(!Near(quiet[3], 0.2f));

  // It reaches 1 and stops there: (1 - 0.6225) / 0.01 = 37.75, so 38 more chunks.
  for (int i = 0; i < 37; ++i) {
    std::vector<float> q(4, 0.1f);
    stage.Process(q.data(), q.size(), 2.0f);
  }
  CHECK(stage.reduction() < 1.0f);  // 37 chunks are one too few
  std::vector<float> q(4, 0.1f);
  stage.Process(q.data(), q.size(), 2.0f);
  CHECK(stage.reduction() == 1.0f);
  CHECK(Near(q[3], 0.2f));
  std::vector<float> after(4, 0.1f);
  CHECK(!stage.Process(after.data(), after.size(), 2.0f));
  CHECK(Near(after[0], 0.2f) && Near(after[3], 0.2f));
  CHECK(stage.reduction() == 1.0f);  // not above 1
}

// While it relaxes, a chunk that is itself nearly too loud holds the release back.
static void TestReleaseBoundedByChunk() {
  OutputGain stage;
  std::vector<float> loud = {1.0f};
  stage.Process(loud.data(), loud.size(), 2.0f);
  CHECK(Near(stage.reduction(), 0.49f));
  // 0.99 * 2 = 1.98 needs 0.98 / 1.98 = 0.49495; the plain step would give 0.50 and 0.99 out.
  std::vector<float> next = {0.99f, 0.99f, 0.99f, 0.99f};
  CHECK(stage.Process(next.data(), next.size(), 2.0f));
  CHECK(Near(stage.reduction(), 0.98f / 1.98f));
  CHECK(Peak(next) <= OutputGain::kCeiling + 1e-6f);
  CHECK(Near(next[3], 0.98f));

  // A louder chunk during the release pulls the reduction down again at once.
  std::vector<float> louder = {1.0f, 0.5f};
  CHECK(stage.Process(louder.data(), louder.size(), 4.0f));
  CHECK(Near(stage.reduction(), 0.245f));
  CHECK(Near(louder[0], 0.98f) && Near(louder[1], 0.49f));
}

// The peak is taken on the magnitude: a negative sample over the ceiling is limited like a positive one.
static void TestNegativePeak() {
  std::vector<float> v = {0.2f, -0.8f};
  OutputGain stage;
  CHECK(stage.Process(v.data(), v.size(), 2.0f));
  CHECK(Near(stage.reduction(), 0.6125f));
  CHECK(Near(v[1], -0.98f));  // not -1.6: the negative half is bounded too
  CHECK(Near(v[0], 0.245f));
}

// A sample that is not a number or not finite becomes silence; it neither reaches the device nor
// drives the limiter (an infinite peak would otherwise mute everything and then fade back in).
static void TestNonFinite() {
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> v = {0.5f, inf, -inf, nan, 0.25f};
  OutputGain stage;
  CHECK(!stage.Process(v.data(), v.size(), 1.0f));
  CHECK(v[0] == 0.5f && v[4] == 0.25f);
  CHECK(v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f);
  CHECK(stage.reduction() == 1.0f);  // not 0: the infinity did not count as a peak

  std::vector<float> w = {0.25f, inf, nan};
  CHECK(!stage.Process(w.data(), w.size(), 2.0f));
  CHECK(Near(w[0], 0.5f) && w[1] == 0.0f && w[2] == 0.0f);
  CHECK(stage.reduction() == 1.0f);
}

int main() {
  TestNegativePeak();
  TestNonFinite();
  TestGainFor();
  TestUnity();
  TestScales();
  TestAttack();
  TestRelease();
  TestReleaseBoundedByChunk();
  if (g_failures) {
    std::printf("%d FAILED of %d checks\n", g_failures, g_checks);
    return 1;
  }
  std::printf("all output gain checks passed (%d checks)\n", g_checks);
  return 0;
}

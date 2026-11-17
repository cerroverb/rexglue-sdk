// The output stage's volume and limiter: the last step before samples go to the audio device.
// Pure (no SDL, no cvars), so tests/host/output_gain_host_test.cpp can drive it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rex::audio {

// audio_volume's range, in percent.
constexpr int32_t kVolumePercentMin = 0;
constexpr int32_t kVolumePercentMax = 300;
constexpr int32_t kVolumePercentDefault = 100;

// The stereo downmix (conversion.h) divides its sum of front, back and half the centre by 2.5 so that
// five full-scale channels cannot clip. Games rarely fill them all: DOA4's loudest moment in a measured
// match summed to 0.43 before that division. The stereo path gets the division back here, and the
// limiter below takes care of the rare sum that does pass full scale.
constexpr float kStereoDownmixMakeup = 2.5f;

// The linear gain for an audio_volume setting on a stream of `stream_channels` channels (2 or 6).
inline float OutputGainFor(int32_t volume_percent, uint32_t stream_channels) {
  const int32_t percent = std::clamp(volume_percent, kVolumePercentMin, kVolumePercentMax);
  const float volume = float(percent) / 100.0f;
  return stream_channels == 2 ? volume * kStereoDownmixMakeup : volume;
}

// Applies a gain to interleaved float samples, chunk by chunk, and keeps the result inside the ceiling.
// A chunk whose peak times gain passes the ceiling is scaled down as a whole at once (no sample of it
// clips); afterwards the reduction relaxes by kReleasePerChunk per chunk, ramped across the chunk, and
// never faster than the chunk in hand allows. One instance per output stream; not thread safe.
class OutputGain {
 public:
  static constexpr float kCeiling = 0.98f;
  // A chunk is 256 frames at 48 kHz (5.3 ms): a full recovery from silence-level reduction takes
  // about half a second.
  static constexpr float kReleasePerChunk = 0.01f;

  // Returns true when the limiter reduced any sample of this chunk.
  bool Process(float* samples, size_t count, float gain) {
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      // A sample that is not finite becomes silence: it must not reach the device, and as a peak it
      // would mute the whole stream.
      if (!std::isfinite(samples[i])) samples[i] = 0.0f;
      peak = std::max(peak, std::fabs(samples[i]));
    }
    const float scaled = peak * gain;
    const float target = scaled > kCeiling ? kCeiling / scaled : 1.0f;
    const float start = reduction_;
    if (target <= start) {
      reduction_ = target;
      if (gain == 1.0f && target == 1.0f) return false;  // nothing to do: leave the samples as they are
      const float factor = gain * target;
      for (size_t i = 0; i < count; ++i) samples[i] *= factor;
      return target < 1.0f;
    }
    reduction_ = std::min(target, start + kReleasePerChunk);
    const float step = (reduction_ - start) / float(count);
    for (size_t i = 0; i < count; ++i) samples[i] *= gain * (start + step * float(i + 1));
    return true;
  }

  // The limiter's factor after the last chunk: 1 when it is not reducing.
  float reduction() const { return reduction_; }

 private:
  float reduction_ = 1.0f;
};

}  // namespace rex::audio

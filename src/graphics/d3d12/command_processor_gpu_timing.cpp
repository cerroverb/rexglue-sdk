/**
 * TEMP DIAGNOSTIC (d3d12_gpu_timing): where the host GPU's time goes, by kind
 * of work. A timestamp is written into the submission wherever the kind of
 * work recorded changes (and only if commands were recorded since the last
 * one), and at each submission's start and end, so the time between two
 * submissions (the GPU idle, or running the presenter's own work) shows too.
 * Read back once the submission's fence has passed; logged every 10 seconds.
 *
 * Ported from lmaogoodcodenotreally/rexglue-sdk@59e13bba without its frame
 * interpolation and added-effects kinds. Added for the gx trial: the window's
 * figures per frame (swaps seen by IssueSwap), the CPU time of the GPU command
 * thread and of the submit worker (GetThreadTimes), the prefetched pages, and
 * the DOA4_GX test seam that sets the trial's cvars.
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_gpu_timing, false, "GPU/D3D12",
                    "Diagnostics: log the host GPU time spent by kind of work (draws, render "
                    "target transfers, texture loads, uploads, resolves, presenting) per frame "
                    "every 10 seconds, from timestamp queries, with the CPU time of the GPU "
                    "command thread and of the submit worker.");

namespace rex::graphics::d3d12 {

namespace {

// CPU time (kernel + user) of a thread in 100 ns units, 0 if unknown.
uint64_t ThreadCpuTime(HANDLE thread) {
  FILETIME creation, exit, kernel, user;
  if (!thread || !GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
    return 0;
  }
  return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
         ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}

}  // namespace

void D3D12CommandProcessor::ApplyGxSeam() {
  static bool applied = false;
  if (applied) {
    return;
  }
  applied = true;
  const char* env = std::getenv("DOA4_GX");
  const std::string text = env ? env : "";
  bool prefetch = false, async = false, timing = false;
  std::string unknown;
  if (!text.empty()) {
    size_t start = 0;
    while (true) {
      size_t end = text.find(',', start);
      if (end == std::string::npos) {
        end = text.size();
      }
      const std::string_view word(text.data() + start, end - start);
      if (word == "prefetch") {
        prefetch = true;
      } else if (word == "async") {
        async = true;
      } else if (word == "timing") {
        timing = true;
      } else {
        unknown += unknown.empty() ? "'" : ", '";
        unknown += word;
        unknown += "'";
      }
      if (end == text.size()) {
        break;
      }
      start = end + 1;
    }
  }
  if (!unknown.empty()) {
    REXGPU_ERROR("gx: DOA4_GX='{}' has an unknown word ({}), so all are off", text, unknown);
    prefetch = async = timing = false;
  }
  REXCVAR_SET(d3d12_gpu_timing, timing);
  REXGPU_INFO("gx: prefetch={} async={} timing={} (DOA4_GX='{}')", prefetch ? "on" : "off",
              async ? "on" : "off", REXCVAR_GET(d3d12_gpu_timing) ? "on" : "off", text);
}

void D3D12CommandProcessor::GpuTimingBeginSubmission() {
  gpu_timing_slot_ = nullptr;
  if (!REXCVAR_GET(d3d12_gpu_timing)) {
    return;
  }
  if (!gpu_timing_heap_) {
    if (gpu_timing_failed_) {
      return;
    }
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    constexpr uint32_t kCount = kGpuTimingSlots * kGpuTimingPerSlot;
    D3D12_QUERY_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = kCount;
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kCount * sizeof(uint64_t),
                                            D3D12_RESOURCE_FLAG_NONE);
    void* mapping = nullptr;
    const D3D12_RANGE read_range = {0, kCount * sizeof(uint64_t)};
    if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&gpu_timing_heap_))) ||
        FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu_timing_readback_))) ||
        FAILED(gpu_timing_readback_->Map(0, &read_range, &mapping)) ||
        FAILED(provider.GetDirectQueue()->GetTimestampFrequency(&gpu_timing_frequency_)) ||
        !gpu_timing_frequency_) {
      REXGPU_ERROR("d3d12_gpu_timing: failed to create the timestamp resources");
      gpu_timing_heap_.Reset();
      gpu_timing_readback_.Reset();
      gpu_timing_failed_ = true;
      return;
    }
    gpu_timing_mapping_ = static_cast<const uint64_t*>(mapping);
    gpu_timing_slots_.resize(kGpuTimingSlots);
    // Submissions begin on the GPU command thread: keep a real handle to it.
    if (!gpu_timing_gpu_thread_ &&
        !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &gpu_timing_gpu_thread_, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
      gpu_timing_gpu_thread_ = nullptr;
    }
    gpu_timing_window_start_ = rex::chrono::Clock::QueryHostTickCount();
    gpu_timing_frames_ = 0;
    gpu_timing_upload_calls_ = gpu_timing_upload_copies_ = 0;
    gpu_timing_upload_bytes_ = gpu_timing_prefetch_pages_ = 0;
    gpu_timing_gpu_thread_cpu_start_ = ThreadCpuTime(gpu_timing_gpu_thread_);
    gpu_timing_submit_cpu_start_ = GpuTimingSubmitThreadCpu();
  }
  GpuTimingHarvest(false);
  GpuTimingSlot& slot = gpu_timing_slots_[submission_current_ % kGpuTimingSlots];
  if (slot.pending) {
    return;  // still in flight: this submission goes untimed
  }
  slot.submission = submission_current_;
  slot.count = 0;
  gpu_timing_slot_ = &slot;
  GpuTimingStamp(gpu_zone_);
}

void D3D12CommandProcessor::GpuTimingStamp(GpuZone zone_after) {
  GpuTimingSlot& slot = *gpu_timing_slot_;
  // The last entry is kept for the submission's end.
  if (slot.count + 1 >= kGpuTimingPerSlot) {
    ++gpu_timing_overflows_;
    return;
  }
  const uint32_t index =
      uint32_t(slot.submission % kGpuTimingSlots) * kGpuTimingPerSlot + slot.count;
  deferred_command_list_.D3DEndQuery(gpu_timing_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
  slot.zone[slot.count++] = uint8_t(zone_after);
  gpu_timing_mark_ = deferred_command_list_.size();
}

void D3D12CommandProcessor::SetGpuZoneSlow(GpuZone zone) {
  GpuTimingSlot& slot = *gpu_timing_slot_;
  if (deferred_command_list_.size() == gpu_timing_mark_ && slot.count) {
    slot.zone[slot.count - 1] = uint8_t(zone);  // nothing recorded in between: relabel
    return;
  }
  GpuTimingStamp(zone);
}

void D3D12CommandProcessor::GpuTimingEndSubmission() {
  if (!gpu_timing_slot_) {
    return;
  }
  GpuTimingSlot& slot = *gpu_timing_slot_;
  gpu_timing_slot_ = nullptr;
  const uint32_t index = uint32_t(slot.submission % kGpuTimingSlots) * kGpuTimingPerSlot;
  deferred_command_list_.D3DEndQuery(gpu_timing_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     index + slot.count);
  slot.zone[slot.count++] = uint8_t(GpuZone::kCount);
  deferred_command_list_.D3DResolveQueryData(gpu_timing_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                             index, slot.count, gpu_timing_readback_.Get(),
                                             uint64_t(index) * sizeof(uint64_t));
  slot.pending = true;
}

uint64_t D3D12CommandProcessor::GpuTimingSubmitThreadCpu() {
  return 0;  // no submit worker in this build
}

void D3D12CommandProcessor::GpuTimingHarvest(bool force) {
  if (!gpu_timing_heap_) {
    return;
  }
  std::vector<GpuTimingSlot*> done;
  for (GpuTimingSlot& slot : gpu_timing_slots_) {
    if (slot.pending && slot.submission <= submission_completed_) {
      done.push_back(&slot);
    }
  }
  std::sort(done.begin(), done.end(), [](const GpuTimingSlot* a, const GpuTimingSlot* b) {
    return a->submission < b->submission;
  });
  for (GpuTimingSlot* slot : done) {
    slot->pending = false;
    const uint64_t* t =
        gpu_timing_mapping_ + uint32_t(slot->submission % kGpuTimingSlots) * kGpuTimingPerSlot;
    for (uint32_t i = 0; i + 1 < slot->count; ++i) {
      if (t[i + 1] > t[i]) {
        gpu_timing_ticks_[slot->zone[i]] += t[i + 1] - t[i];
      }
    }
    if (slot->submission == gpu_timing_last_submission_ + 1 && gpu_timing_last_end_ &&
        t[0] > gpu_timing_last_end_) {
      gpu_timing_ticks_[size_t(GpuZone::kCount)] += t[0] - gpu_timing_last_end_;
    }
    gpu_timing_last_submission_ = slot->submission;
    gpu_timing_last_end_ = t[slot->count - 1];
    ++gpu_timing_submissions_;
  }

  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t host_frequency = rex::chrono::Clock::QueryHostTickFrequency();
  if (!force && now - gpu_timing_window_start_ < host_frequency * 10) {
    return;
  }
  const double seconds = double(now - gpu_timing_window_start_) / double(host_frequency);
  const uint64_t frames = gpu_timing_frames_;
  const double per_frame = frames ? 1.0 / double(frames) : 0.0;
  auto ms = [&](size_t zone) {
    return double(gpu_timing_ticks_[zone]) * 1000.0 / double(gpu_timing_frequency_) * per_frame;
  };
  double busy = 0.0;
  for (size_t i = 0; i < size_t(GpuZone::kCount); ++i) {
    busy += ms(i);
  }
  const uint64_t gpu_thread_cpu = ThreadCpuTime(gpu_timing_gpu_thread_);
  const uint64_t submit_cpu = GpuTimingSubmitThreadCpu();
  // 100 ns units to ms.
  const double gpu_thread_ms =
      double(gpu_thread_cpu - std::min(gpu_thread_cpu, gpu_timing_gpu_thread_cpu_start_)) *
      1.0e-4 * per_frame;
  const double submit_ms =
      double(submit_cpu - std::min(submit_cpu, gpu_timing_submit_cpu_start_)) * 1.0e-4 *
      per_frame;
  REXGPU_INFO(
      "gx timing: secs={:.1f} frames={} gpu_ms_per_frame={:.3f} setup={:.3f} draw={:.3f} "
      "rt={:.3f} tex={:.3f} upload={:.3f} resolve={:.3f} swap={:.3f} idle={:.3f} "
      "submissions={} overflows={} | gpu_thread_cpu_ms_per_frame={:.3f} "
      "submit_cpu_ms_per_frame={:.3f} | upload_calls={:.1f} upload_copies={:.1f} "
      "upload_kb={:.1f} per frame | prefetch_pages={:.1f} per frame{}",
      seconds, frames, busy, ms(size_t(GpuZone::kSetup)), ms(size_t(GpuZone::kDraw)),
      ms(size_t(GpuZone::kRenderTargets)), ms(size_t(GpuZone::kTextures)),
      ms(size_t(GpuZone::kUploads)), ms(size_t(GpuZone::kResolves)), ms(size_t(GpuZone::kSwap)),
      ms(size_t(GpuZone::kCount)), gpu_timing_submissions_, gpu_timing_overflows_, gpu_thread_ms,
      submit_ms, double(gpu_timing_upload_calls_) * per_frame,
      double(gpu_timing_upload_copies_) * per_frame,
      double(gpu_timing_upload_bytes_) / 1024.0 * per_frame,
      double(gpu_timing_prefetch_pages_) * per_frame, force ? " (at shutdown)" : "");
  gpu_timing_upload_calls_ = gpu_timing_upload_copies_ = 0;
  gpu_timing_upload_bytes_ = gpu_timing_prefetch_pages_ = 0;
  std::fill(std::begin(gpu_timing_ticks_), std::end(gpu_timing_ticks_), 0);
  gpu_timing_submissions_ = 0;
  gpu_timing_overflows_ = 0;
  gpu_timing_frames_ = 0;
  gpu_timing_window_start_ = now;
  gpu_timing_gpu_thread_cpu_start_ = gpu_thread_cpu;
  gpu_timing_submit_cpu_start_ = submit_cpu;
}

}  // namespace rex::graphics::d3d12

/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#define LOG_TAG "GCH_AidlProfiler"
#include "aidl_profiler.h"

#include <log/log.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "profiler.h"
#include "profiler_util.h"
#include "tracked_profiler.h"

namespace android {
namespace google_camera_hal {
namespace {

using ::google::camera_common::Profiler;

// setprop key for profiling open/close camera
constexpr char kPropKeyProfileOpenClose[] =
    "persist.vendor.camera.profiler.open_close";
// setprop key for profiling camera fps
constexpr char kPropKeyProfileFps[] = "persist.vendor.camera.profiler.fps";
constexpr char kPropKeyProfileReprocess[] =
    "persist.vendor.camera.profiler.reprocess";

constexpr char kReprocess[] = "Reprocess Frame ";

class AidlProfilerImpl : public AidlProfiler {
 public:
  AidlProfilerImpl(uint32_t camera_id, int32_t latency_flag, int32_t fps_flag,
                   int32_t reprocess_latency_flag)
      : camera_id_string_("Cam" + std::to_string(camera_id)),
        camera_id_(camera_id),
        latency_flag_(latency_flag),
        fps_flag_(fps_flag),
        reprocess_latency_flag_(reprocess_latency_flag) {
  }

  std::unique_ptr<AidlScopedProfiler> MakeScopedProfiler(
      EventType type,
      std::unique_ptr<google::camera_common::Profiler> custom_latency_profiler,
      std::unique_ptr<google::camera_common::Profiler> custom_fps_profiler)
      override {
    /*
     * Makes a ScopedProfiler to help profile the corresponding event type. If
     * profiling an open, close, or reconfiguration operation, function checks
     * to see if the event can be used as a continuation of a previous operation
     */
    std::lock_guard lock(api_mutex_);
    if (type == EventType::kConfigureStream && fps_profiler_ == nullptr) {
      if (SetFpsProfiler(std::move(custom_fps_profiler)) == false) {
        fps_profiler_ = CreateFpsProfiler();
      }
    }

    latency_profilers_.erase(
        std::remove_if(latency_profilers_.begin(), latency_profilers_.end(),
                       [type](const auto& profiler) {
                         return profiler->ShouldDelete(type);
                       }),
        latency_profilers_.end());

    for (auto rprofiler = latency_profilers_.rbegin();
         rprofiler != latency_profilers_.rend(); ++rprofiler) {
      if (std::unique_ptr<AidlScopedProfiler> ret =
              (*rprofiler)->AcceptNextState(type);
          ret != nullptr) {
        return ret;
      }
    }

    if (int size = latency_profilers_.size(); size > 2) {
      ALOGW("%s: Too many overlapping operations (have: %d). Will not profile.",
            __FUNCTION__, size);
      return nullptr;
    }

    if (type == EventType::kOpen || type == EventType::kFlush) {
      if (SetOrCreateTrackedProfiler(std::move(custom_latency_profiler),
                                     camera_id_string_)) {
        return latency_profilers_.back()->AcceptNextState(type);
      } else {
        return nullptr;
      }
    }
    ALOGW("%s: Could not find an operation for incoming event: %s",
          __FUNCTION__, EventTypeToString(type).c_str());
    return nullptr;
  }

  void FirstFrameStart() override {
    std::lock_guard lock(api_mutex_);
    for (auto rprofiler = latency_profilers_.rbegin();
         rprofiler != latency_profilers_.rend(); ++rprofiler) {
      if ((*rprofiler)->AcceptFirstFrameStart()) {
        return;
      }
    }
    ALOGW("%s: Error: no profiler accepted First Frame Start", __FUNCTION__);
  }

  void FirstFrameEnd() override {
    std::lock_guard lock(api_mutex_);
    for (auto rprofiler = latency_profilers_.rbegin();
         rprofiler != latency_profilers_.rend(); ++rprofiler) {
      if ((*rprofiler)->AcceptFirstFrameEnd()) {
        latency_profilers_.erase(std::next(rprofiler).base());
        return;
      }
    }
    ALOGW("%s: Error: no profiler accepted First Frame End", __FUNCTION__);
  }

  void ReprocessingRequestStart(
      std::unique_ptr<Profiler> custom_reprocessing_profiler,
      int32_t id) override {
    std::lock_guard lock(api_mutex_);
    if (reprocessing_profiler_ == nullptr) {
      if (!SetReprocessingProfiler(std::move(custom_reprocessing_profiler))) {
        reprocessing_profiler_ = CreateReprocessingProfiler();
      }
      reprocessing_profiler_->SetUseCase(camera_id_string_ + "-Reprocess");
      open_reprocessing_frames_count_ = 0;
    }
    if (reprocessing_profiler_ != nullptr) {
      reprocessing_profiler_->Start(kReprocess + std::to_string(id),
                                    Profiler::kInvalidRequestId);
      open_reprocessing_frames_count_++;
    }
  }

  void ReprocessingResultEnd(int32_t id) override {
    std::lock_guard lock(api_mutex_);
    if (reprocessing_profiler_ != nullptr) {
      reprocessing_profiler_->End(kReprocess + std::to_string(id),
                                  Profiler::kInvalidRequestId);
      if (--open_reprocessing_frames_count_ <= 0) {
        reprocessing_profiler_ = nullptr;
      }
    }
  }

  void ProfileFrameRate(const std::string& name) override {
    std::lock_guard lock(api_mutex_);
    if (fps_profiler_ != nullptr) {
      fps_profiler_->ProfileFrameRate(name);
    }
  }

 private:
  std::shared_ptr<Profiler> CreateLatencyProfiler() {
    if (latency_flag_ == Profiler::SetPropFlag::kDisable) {
      return nullptr;
    }
    std::shared_ptr<Profiler> profiler = Profiler::Create(latency_flag_);
    if (profiler == nullptr) {
      ALOGW("%s: Failed to create profiler", __FUNCTION__);
      return nullptr;
    }
    profiler->SetDumpFilePrefix(
        "/data/vendor/camera/profiler/aidl_open_close_");
    profiler->Start(kOverall, Profiler::kInvalidRequestId);
    return profiler;
  }

  bool SetOrCreateTrackedProfiler(std::unique_ptr<Profiler> profiler,
                                  std::string camera_id_string) {
    if (profiler == nullptr) {
      std::shared_ptr<Profiler> latency_profiler_ = CreateLatencyProfiler();
      if (latency_profiler_ == nullptr) {
        return false;
      }
      latency_profilers_.emplace_back(std::make_shared<TrackedProfiler>(
          latency_profiler_, camera_id_string, EventType::kNone));
    } else {
      profiler->SetDumpFilePrefix(
          "/data/vendor/camera/profiler/aidl_open_close_");
      profiler->Start(kOverall, Profiler::kInvalidRequestId);
      latency_profilers_.emplace_back(std::make_shared<TrackedProfiler>(
          std::move(profiler), camera_id_string, EventType::kNone));
    }
    return true;
  }

  std::shared_ptr<Profiler> CreateFpsProfiler() {
    if (fps_flag_ == Profiler::SetPropFlag::kDisable) {
      return nullptr;
    }
    std::shared_ptr<Profiler> profiler = Profiler::Create(fps_flag_);
    if (profiler == nullptr) {
      ALOGW("%s: Failed to create profiler", __FUNCTION__);
      return nullptr;
    }
    profiler->SetDumpFilePrefix("/data/vendor/camera/profiler/aidl_fps_");
    return profiler;
  }

  std::shared_ptr<Profiler> CreateReprocessingProfiler() {
    if (reprocess_latency_flag_ == Profiler::SetPropFlag::kDisable) {
      return nullptr;
    }
    std::shared_ptr<Profiler> profiler = Profiler::Create(latency_flag_);
    if (profiler == nullptr) {
      ALOGW("%s: Failed to create profiler", __FUNCTION__);
      return nullptr;
    }
    profiler->SetDumpFilePrefix("/data/vendor/camera/profiler/aidl_reprocess_");
    return profiler;
  }

  uint32_t GetCameraId() const {
    return camera_id_;
  }
  int32_t GetLatencyFlag() const {
    return latency_flag_;
  }
  int32_t GetFpsFlag() const {
    return fps_flag_;
  }
  int32_t GetReprocessLatencyFlag() const {
    return reprocess_latency_flag_;
  }

  bool SetFpsProfiler(std::unique_ptr<Profiler> profiler) {
    if (profiler == nullptr) {
      return false;
    }
    fps_profiler_ = std::move(profiler);
    if (fps_profiler_ != nullptr) {
      fps_profiler_->SetDumpFilePrefix(
          "/data/vendor/camera/profiler/aidl_fps_");
      return true;
    }
    return false;
  }

  bool SetReprocessingProfiler(std::unique_ptr<Profiler> profiler) {
    if (profiler == nullptr) {
      return false;
    }
    reprocessing_profiler_ = std::move(profiler);
    open_reprocessing_frames_count_ = 0;
    if (reprocessing_profiler_ != nullptr) {
      reprocessing_profiler_->SetDumpFilePrefix(
          "data/vendor/camera/profiler/aidl_reprocess_");
      return true;
    }
    return false;
  }

  // Protect all API functions mutually exclusive, all member variables should
  // also be protected by this mutex.
  std::mutex api_mutex_;
  std::vector<std::shared_ptr<TrackedProfiler>> latency_profilers_;
  std::shared_ptr<Profiler> fps_profiler_;
  std::shared_ptr<Profiler> reprocessing_profiler_;

  const std::string camera_id_string_;
  const uint32_t camera_id_;
  const int32_t latency_flag_;
  const int32_t fps_flag_;
  const int32_t reprocess_latency_flag_;

  int32_t open_reprocessing_frames_count_;
};

class AidlProfilerMock : public AidlProfiler {
  std::unique_ptr<AidlScopedProfiler> MakeScopedProfiler(
      EventType, std::unique_ptr<google::camera_common::Profiler>,
      std::unique_ptr<google::camera_common::Profiler>) override {
    return nullptr;
  }

  void FirstFrameStart() override{};
  void FirstFrameEnd() override{};
  void ProfileFrameRate(const std::string&) override{};
  void ReprocessingRequestStart(std::unique_ptr<Profiler>, int32_t) override{};
  void ReprocessingResultEnd(int32_t) override{};

  uint32_t GetCameraId() const override {
    return 0;
  }
  int32_t GetLatencyFlag() const override {
    return 0;
  }
  int32_t GetFpsFlag() const override {
    return 0;
  }
  int32_t GetReprocessLatencyFlag() const override {
    return 0;
  }
};

}  // anonymous namespace

std::shared_ptr<AidlProfiler> AidlProfiler::Create(uint32_t camera_id) {
  int32_t latency_flag = property_get_int32(
      kPropKeyProfileOpenClose, Profiler::SetPropFlag::kCustomProfiler);
  int32_t fps_flag = property_get_int32(kPropKeyProfileFps,
                                        Profiler::SetPropFlag::kCustomProfiler);
  int32_t reprocess_latency_flag = property_get_int32(
      kPropKeyProfileReprocess, Profiler::SetPropFlag::kCustomProfiler);

  if (latency_flag == Profiler::SetPropFlag::kDisable &&
      fps_flag == Profiler::SetPropFlag::kDisable &&
      reprocess_latency_flag == Profiler::SetPropFlag::kDisable) {
    return std::make_shared<AidlProfilerMock>();
  }
  // Use stopwatch flag to print result.
  if ((latency_flag & Profiler::SetPropFlag::kPrintBit) != 0) {
    latency_flag |= Profiler::SetPropFlag::kStopWatch;
  }
  if ((reprocess_latency_flag & Profiler::SetPropFlag::kPrintBit) != 0) {
    reprocess_latency_flag |= Profiler::SetPropFlag::kStopWatch;
  }
  // Use interval flag to print fps instead of print on end.
  if ((fps_flag & Profiler::SetPropFlag::kPrintBit) != 0) {
    fps_flag |= Profiler::SetPropFlag::kPrintFpsPerIntervalBit;
    fps_flag &= ~Profiler::SetPropFlag::kPrintBit;
  }
  return std::make_shared<AidlProfilerImpl>(camera_id, latency_flag, fps_flag,
                                            reprocess_latency_flag);
}

AidlScopedProfiler::AidlScopedProfiler(std::shared_ptr<Profiler> profiler,
                                       const std::string name, int id,
                                       std::function<void()> end_callback)
    : profiler_(profiler),
      name_(std::move(name)),
      id_(id),
      end_callback_(end_callback) {
  profiler_->Start(name_, id_);
  profiler_->Start(kHalTotal, Profiler::kInvalidRequestId);
}

AidlScopedProfiler::~AidlScopedProfiler() {
  profiler_->End(kHalTotal, Profiler::kInvalidRequestId);
  profiler_->End(name_, id_);
  if (end_callback_) {
    end_callback_();
  }
}

}  // namespace google_camera_hal
}  // namespace android

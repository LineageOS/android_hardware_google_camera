/*
 * Copyright (C) 2019 The Android Open Source Project
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
#define LOG_TAG "GCH_CameraDevice"
#define ATRACE_TAG ATRACE_TAG_CAMERA
#include "camera_device.h"

#include <dlfcn.h>
#include <errno.h>
#include <log/log.h>
#include <meminfo/procmeminfo.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utils/Trace.h>

#include <thread>

#include "hwl_types.h"
#include "log/log_main.h"
#include "utils.h"
#include "vendor_tags.h"

using android::meminfo::ProcMemInfo;
using namespace android::meminfo;

namespace android {
namespace {
enum class PreloadMode {
  kMadvise = 0,
  kMlockMadvise = 1,
};
}  // namespace

void MadviseFileForRange(size_t madvise_size_limit_bytes, size_t map_size_bytes,
                         const uint8_t* map_begin, const uint8_t* map_end,
                         const std::string& file_name,
                         PreloadMode preload_mode) {
  // Ideal blockTransferSize for madvising files (128KiB)
  static const size_t kIdealIoTransferSizeBytes = 128 * 1024;
  size_t target_size_bytes =
      std::min<size_t>(map_size_bytes, madvise_size_limit_bytes);
  if (target_size_bytes == 0) {
    return;
  }
  std::string trace_tag =
      file_name + " size=" + std::to_string(target_size_bytes);
  if (preload_mode == PreloadMode::kMadvise) {
    trace_tag = "madvising " + trace_tag;
  } else if (preload_mode == PreloadMode::kMlockMadvise) {
    trace_tag = "madvising and mlocking " + trace_tag;
  } else {
    trace_tag = "Unknown preload mode " + trace_tag;
    ALOGE("%s: Unknown preload mode %d", __FUNCTION__, preload_mode);
  }
  ATRACE_NAME(trace_tag.c_str());
  // Based on requested size (target_size_bytes)
  const uint8_t* target_pos = map_begin + target_size_bytes;

  // Clamp endOfFile if its past map_end
  if (target_pos > map_end) {
    target_pos = map_end;
  }

  // Madvise the whole file up to target_pos in chunks of
  // kIdealIoTransferSizeBytes (to MADV_WILLNEED)
  // Note:
  // madvise(MADV_WILLNEED) will prefetch max(fd readahead size, optimal
  // block size for device) per call, hence the need for chunks. (128KB is a
  // good default.)
  for (const uint8_t* madvise_start = map_begin; madvise_start < target_pos;
       madvise_start += kIdealIoTransferSizeBytes) {
    void* madvise_addr =
        const_cast<void*>(reinterpret_cast<const void*>(madvise_start));
    size_t madvise_length =
        std::min(kIdealIoTransferSizeBytes,
                 static_cast<size_t>(target_pos - madvise_start));
    if (preload_mode == PreloadMode::kMlockMadvise) {
      int status_mlock = mlock(madvise_addr, madvise_length);
      // In case of error we stop mlocking rest of the file
      if (status_mlock < 0) {
        ALOGW(
            "%s: Pinning memory by mlock failed! status=%i, errno=%i, "
            "trace_tag=%s",
            __FUNCTION__, status_mlock, errno, trace_tag.c_str());
        break;
      }
    }
    int status_madvise = madvise(madvise_addr, madvise_length, MADV_WILLNEED);
    // In case of error we stop madvising rest of the file
    if (status_madvise < 0) {
      break;
    }
  }
}

static void ReadAheadVma(const Vma& vma, const size_t madvise_size_limit_bytes,
                         PreloadMode preload_mode) {
  const uint8_t* map_begin = reinterpret_cast<uint8_t*>(vma.start);
  const uint8_t* map_end = reinterpret_cast<uint8_t*>(vma.end);
  MadviseFileForRange(madvise_size_limit_bytes,
                      static_cast<size_t>(map_end - map_begin), map_begin,
                      map_end, vma.name, preload_mode);
}

static void UnpinVma(const Vma& vma) {
  std::string trace_tag =
      "munlocking " + vma.name + " size=" + std::to_string(vma.end - vma.start);
  ATRACE_NAME(trace_tag.c_str());
  int status_munlock =
      munlock(reinterpret_cast<uint8_t*>(vma.start), vma.end - vma.start);
  if (status_munlock < 0) {
    ALOGW(
        "%s: Unlocking memory failed! status=%i, errno=%i, "
        "trace_tag=%s",
        __FUNCTION__, status_munlock, errno, trace_tag.c_str());
  }
}

// Update memory configuration to match the new configuration. This includes
// pinning new libraries, unpinning libraries that were pinned in the old
// config but aren't any longer, and madvising anonymous VMAs.
static void LoadLibraries(google_camera_hal::HwlMemoryConfig memory_config,
                          google_camera_hal::HwlMemoryConfig old_memory_config) {
  auto vmaCollectorCb = [&memory_config, &old_memory_config](const Vma& vma) {
    // Read ahead for anonymous VMAs and for specific files.
    // vma.flags represents a VMAs rwx bits.
    if (vma.inode == 0 && !vma.is_shared && vma.flags) {
      if (memory_config.madvise_map_size_limit_bytes == 0) {
        return true;
      }
      // Madvise anonymous memory, do not pin.
      ReadAheadVma(vma, memory_config.madvise_map_size_limit_bytes,
                   PreloadMode::kMadvise);
      return true;
    }
    if (memory_config.pinned_libraries.contains(vma.name) &&
        !old_memory_config.pinned_libraries.contains(vma.name)) {
      // File-backed VMAs do not have a madvise limit
      ReadAheadVma(vma, std::numeric_limits<size_t>::max(),
                   PreloadMode::kMlockMadvise);
    } else if (!memory_config.pinned_libraries.contains(vma.name) &&
               old_memory_config.pinned_libraries.contains(vma.name)) {
      // Unpin libraries that were previously pinned but are no longer needed.
      ALOGI("%s: Unpinning %s", __FUNCTION__, vma.name.c_str());
      UnpinVma(vma);
    }
    return true;
  };
  ProcMemInfo meminfo(getpid());
  meminfo.ForEachVmaFromMaps(vmaCollectorCb);
}

namespace google_camera_hal {

// HAL external capture session library path
#if GCH_HWL_USE_DLOPEN
#if defined(_LP64)
constexpr char kExternalCaptureSessionDir[] =
    "/vendor/lib64/camera/capture_sessions/";
#else  // defined(_LP64)
constexpr char kExternalCaptureSessionDir[] =
    "/vendor/lib/camera/capture_sessions/";
#endif
#endif

HwlMemoryConfig CameraDevice::applied_memory_config_;
std::mutex CameraDevice::applied_memory_config_mutex_;

std::unique_ptr<CameraDevice> CameraDevice::Create(
    std::unique_ptr<CameraDeviceHwl> camera_device_hwl,
    CameraBufferAllocatorHwl* camera_allocator_hwl) {
  ATRACE_CALL();
  auto device = std::unique_ptr<CameraDevice>(new CameraDevice());

  if (device == nullptr) {
    ALOGE("%s: Creating CameraDevice failed.", __FUNCTION__);
    return nullptr;
  }

  status_t res =
      device->Initialize(std::move(camera_device_hwl), camera_allocator_hwl);
  if (res != OK) {
    ALOGE("%s: Initializing CameraDevice failed: %s (%d).", __FUNCTION__,
          strerror(-res), res);
    return nullptr;
  }

  ALOGI("%s: Created a camera device for public(%u)", __FUNCTION__,
        device->GetPublicCameraId());

  android::google_camera_hal::HwlMemoryConfig memory_config =
      device->camera_device_hwl_->GetMemoryConfig();
  memory_config.madvise_map_size_limit_bytes = 0;
  ALOGI("Pinning memory for %zu shared libraries.",
        memory_config.pinned_libraries.size());

  std::lock_guard<std::mutex> lock(applied_memory_config_mutex_);
  std::thread t(LoadLibraries, memory_config, device->GetAppliedMemoryConfig());
  t.detach();
  device->SetAppliedMemoryConfig(memory_config);

  return device;
}

status_t CameraDevice::Initialize(
    std::unique_ptr<CameraDeviceHwl> camera_device_hwl,
    CameraBufferAllocatorHwl* camera_allocator_hwl) {
  ATRACE_CALL();
  if (camera_device_hwl == nullptr) {
    ALOGE("%s: camera_device_hwl cannot be nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  public_camera_id_ = camera_device_hwl->GetCameraId();
  camera_device_hwl_ = std::move(camera_device_hwl);
  camera_allocator_hwl_ = camera_allocator_hwl;
  status_t res = LoadExternalCaptureSession();
  if (res != OK) {
    ALOGE("%s: Loading external capture sessions failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  std::unique_ptr<HalCameraMetadata> static_metadata;
  res = camera_device_hwl_->GetCameraCharacteristics(&static_metadata);
  if (res != OK) {
    ALOGE("%s: Getting camera characteristics failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  res = utils::GetStreamUseCases(
      static_metadata.get(),
      &camera_id_to_stream_use_cases_[camera_device_hwl_->GetCameraId()]);
  if (res != OK) {
    ALOGE(
        "%s: Initializing logical stream use case for camera id %u failed: "
        "%s(%d)",
        __FUNCTION__, camera_device_hwl_->GetCameraId(), strerror(-res), res);
    return res;
  }
  res = utils::GetPhysicalCameraStreamUseCases(camera_device_hwl_.get(),
                                               &camera_id_to_stream_use_cases_);

  if (res != OK) {
    ALOGE(
        "%s: Initializing physical stream use case for camera id %u failed: "
        "%s(%d)",
        __FUNCTION__, camera_device_hwl_->GetCameraId(), strerror(-res), res);
  }
  return res;
}

status_t CameraDevice::GetResourceCost(CameraResourceCost* cost) {
  ATRACE_CALL();
  return camera_device_hwl_->GetResourceCost(cost);
}

status_t CameraDevice::GetCameraCharacteristics(
    std::unique_ptr<HalCameraMetadata>* characteristics) {
  ATRACE_CALL();
  status_t res = camera_device_hwl_->GetCameraCharacteristics(characteristics);
  if (res != OK) {
    ALOGE("%s: GetCameraCharacteristics() failed: %s (%d).", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  return hal_vendor_tag_utils::ModifyCharacteristicsKeys(characteristics->get());
}

// Populates the required session characteristics keys from a camera
// characteristics object.
status_t generateSessionCharacteristics(
    const HalCameraMetadata* camera_characteristics,
    HalCameraMetadata* session_characteristics) {
  if (camera_characteristics == nullptr) {
    ALOGE("%s: camera characteristics is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  if (session_characteristics == nullptr) {
    ALOGE("%s: session characteristics is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  camera_metadata_ro_entry entry;
  status_t res;

  // Get the zoom ratio key
  res = camera_characteristics->Get(ANDROID_CONTROL_ZOOM_RATIO_RANGE, &entry);
  if (res == OK && entry.count == 2) {
    std::vector<float> zoom_ratio_key(entry.data.f, entry.data.f + entry.count);
    if (session_characteristics->Set(ANDROID_CONTROL_ZOOM_RATIO_RANGE,
                                     zoom_ratio_key.data(),
                                     zoom_ratio_key.size()) != OK) {
      ALOGE("%s Updating static metadata with zoom ratio range failed",
            __FUNCTION__);
      return UNKNOWN_ERROR;
    }
  }

  // Get the max digital zoom key
  res = camera_characteristics->Get(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
                                    &entry);
  if (res == OK) {
    std::vector<float> max_digital_zoom_key(entry.data.f,
                                            entry.data.f + entry.count);
    if (session_characteristics->Set(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
                                     max_digital_zoom_key.data(),
                                     max_digital_zoom_key.size()) != OK) {
      ALOGE("%s Updating static metadata with max digital zoom failed",
            __FUNCTION__);
      return UNKNOWN_ERROR;
    }
  }

  return OK;
}

status_t CameraDevice::GetSessionCharacteristics(
    const StreamConfiguration& stream_config,
    std::unique_ptr<HalCameraMetadata>& session_characteristics) {
  ATRACE_CALL();
  std::unique_ptr<HalCameraMetadata> camera_characteristics;
  status_t res = camera_device_hwl_->GetSessionCharacteristics(
      stream_config, camera_characteristics);
  if (res != OK) {
    ALOGE("%s: GetCameraCharacteristics() failed: %s (%d).", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  // Allocating space for 10 entries and 256 bytes.
  session_characteristics = HalCameraMetadata::Create(10, 256);

  return generateSessionCharacteristics(camera_characteristics.get(),
                                        session_characteristics.get());
}

status_t CameraDevice::GetPhysicalCameraCharacteristics(
    uint32_t physical_camera_id,
    std::unique_ptr<HalCameraMetadata>* characteristics) {
  ATRACE_CALL();
  status_t res = camera_device_hwl_->GetPhysicalCameraCharacteristics(
      physical_camera_id, characteristics);
  if (res != OK) {
    ALOGE("%s: GetPhysicalCameraCharacteristics() failed: %s (%d).",
          __FUNCTION__, strerror(-res), res);
    return res;
  }

  return hal_vendor_tag_utils::ModifyCharacteristicsKeys(characteristics->get());
}

status_t CameraDevice::SetTorchMode(TorchMode mode) {
  ATRACE_CALL();
  return camera_device_hwl_->SetTorchMode(mode);
}

status_t CameraDevice::TurnOnTorchWithStrengthLevel(int32_t torch_strength) {
  ATRACE_CALL();
  return camera_device_hwl_->TurnOnTorchWithStrengthLevel(torch_strength);
}

status_t CameraDevice::GetTorchStrengthLevel(int32_t& torch_strength) const {
  ATRACE_CALL();
  status_t res = camera_device_hwl_->GetTorchStrengthLevel(torch_strength);
  if (res != OK) {
    ALOGE("%s: GetTorchStrengthLevel() failed: %s (%d).", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  return res;
}

status_t CameraDevice::ConstructDefaultRequestSettings(
    RequestTemplate type, std::unique_ptr<HalCameraMetadata>* request_settings) {
  ATRACE_CALL();
  return camera_device_hwl_->ConstructDefaultRequestSettings(type,
                                                             request_settings);
}

status_t CameraDevice::DumpState(int fd) {
  ATRACE_CALL();
  return camera_device_hwl_->DumpState(fd);
}

status_t CameraDevice::CreateCameraDeviceSession(
    std::unique_ptr<CameraDeviceSession>* session) {
  ATRACE_CALL();
  if (session == nullptr) {
    ALOGE("%s: session is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  std::unique_ptr<CameraDeviceSessionHwl> session_hwl;
  status_t res = camera_device_hwl_->CreateCameraDeviceSessionHwl(
      camera_allocator_hwl_, &session_hwl);
  if (res != OK) {
    ALOGE("%s: Creating a CameraDeviceSessionHwl failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  *session = CameraDeviceSession::Create(std::move(session_hwl),
                                         external_session_factory_entries_,
                                         camera_allocator_hwl_);
  if (*session == nullptr) {
    ALOGE("%s: Creating a CameraDeviceSession failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return UNKNOWN_ERROR;
  }

  std::lock_guard<std::mutex> lock(applied_memory_config_mutex_);
  HwlMemoryConfig memory_config = camera_device_hwl_->GetMemoryConfig();
  std::thread t(LoadLibraries, memory_config, GetAppliedMemoryConfig());
  SetAppliedMemoryConfig(memory_config);
  t.detach();

  return OK;
}

bool CameraDevice::IsStreamCombinationSupported(
    const StreamConfiguration& stream_config, bool check_settings) {
  if (!utils::IsStreamUseCaseSupported(stream_config, public_camera_id_,
                                       camera_id_to_stream_use_cases_)) {
    return false;
  }

  bool supported = camera_device_hwl_->IsStreamCombinationSupported(
      stream_config, check_settings);
  if (!supported) {
    ALOGD("%s: stream config is not supported.", __FUNCTION__);
  }

  return supported;
}

status_t CameraDevice::LoadExternalCaptureSession() {
  ATRACE_CALL();

  if (external_session_factory_entries_.size() > 0) {
    ALOGI("%s: External capture session libraries already loaded; skip.",
          __FUNCTION__);
    return OK;
  }

#if GCH_HWL_USE_DLOPEN
  for (const auto& lib_path :
       utils::FindLibraryPaths(kExternalCaptureSessionDir)) {
    ALOGI("%s: Loading %s", __FUNCTION__, lib_path.c_str());
    void* lib_handle = nullptr;
    // load shared library and never unload
    // TODO(b/...): Switch to using build-system based HWL
    //   loading and remove dlopen here?
    lib_handle = dlopen(lib_path.c_str(), RTLD_NOW);
    if (lib_handle == nullptr) {
      ALOGW("Failed loading %s.", lib_path.c_str());
      continue;
    }

    GetCaptureSessionFactoryFunc external_session_factory_t =
        reinterpret_cast<GetCaptureSessionFactoryFunc>(
            dlsym(lib_handle, "GetCaptureSessionFactory"));
    if (external_session_factory_t == nullptr) {
      ALOGE("%s: dlsym failed (%s) when loading %s.", __FUNCTION__,
            "GetCaptureSessionFactory", lib_path.c_str());
      dlclose(lib_handle);
      lib_handle = nullptr;
      continue;
    }

    external_session_factory_entries_.push_back(external_session_factory_t);
    external_capture_session_lib_handles_.push_back(lib_handle);
  }
#else
  if (GetCaptureSessionFactory) {
    external_session_factory_entries_.push_back(GetCaptureSessionFactory);
  }
#endif

  return OK;
}

CameraDevice::~CameraDevice() {
}

std::unique_ptr<google::camera_common::Profiler> CameraDevice::GetProfiler(
    uint32_t camera_id, int option) {
  return camera_device_hwl_->GetProfiler(camera_id, option);
}

}  // namespace google_camera_hal
}  // namespace android

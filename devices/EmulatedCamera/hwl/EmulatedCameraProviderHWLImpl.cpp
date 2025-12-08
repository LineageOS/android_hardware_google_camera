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

//#define LOG_NDEBUG 0
#define LOG_TAG "EmulatedCameraProviderHwlImpl"
#include "EmulatedCameraProviderHWLImpl.h"

#include <android-base/file.h>
#include <android-base/strings.h>
#include <cutils/properties.h>
#include <hardware/camera_common.h>
#include <log/log.h>

#include "ConfigUtils.h"
#include "EmulatedCameraDeviceHWLImpl.h"
#include "EmulatedCameraDeviceSessionHWLImpl.h"
#include "EmulatedLogicalRequestState.h"
#include "EmulatedSensor.h"
#include "EmulatedTorchState.h"
#include "utils/HWLUtils.h"
#include "vendor_tag_defs.h"

namespace android {

constexpr StreamSize s240pStreamSize = std::pair(240, 180);
constexpr StreamSize s720pStreamSize = std::pair(1280, 720);
constexpr StreamSize s1440pStreamSize = std::pair(1920, 1440);

std::unique_ptr<EmulatedCameraProviderHwlImpl>
EmulatedCameraProviderHwlImpl::Create() {
  auto provider = std::unique_ptr<EmulatedCameraProviderHwlImpl>(
      new EmulatedCameraProviderHwlImpl());

  if (provider == nullptr) {
    ALOGE("%s: Creating EmulatedCameraProviderHwlImpl failed.", __FUNCTION__);
    return nullptr;
  }

  status_t res = provider->Initialize();
  if (res != OK) {
    ALOGE("%s: Initializing EmulatedCameraProviderHwlImpl failed: %s (%d).",
          __FUNCTION__, strerror(-res), res);
    return nullptr;
  }

  ALOGI("%s: Created EmulatedCameraProviderHwlImpl", __FUNCTION__);

  return provider;
}

static bool IsMaxSupportedSizeGreaterThanOrEqual(
    const std::set<StreamSize>& stream_sizes, StreamSize compare_size) {
  for (const auto& stream_size : stream_sizes) {
    if (stream_size.first * stream_size.second >=
        compare_size.first * compare_size.second) {
      return true;
    }
  }
  return false;
}

static bool SupportsCapability(const uint32_t camera_id,
                               const HalCameraMetadata& static_metadata,
                               uint8_t cap) {
  camera_metadata_ro_entry_t entry;
  auto ret = static_metadata.Get(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
  if (ret != OK || (entry.count == 0)) {
    ALOGE("Error getting capabilities for camera id %u", camera_id);
    return false;
  }
  for (size_t i = 0; i < entry.count; i++) {
    if (entry.data.u8[i] == cap) {
      return true;
    }
  }
  return false;
}

bool EmulatedCameraProviderHwlImpl::SupportsMandatoryConcurrentStreams(
    uint32_t camera_id) {
  HalCameraMetadata& static_metadata = *(static_metadata_.at(camera_id));
  auto map = std::make_unique<StreamConfigurationMap>(static_metadata);
  auto yuv_output_sizes = map->GetOutputSizes(HAL_PIXEL_FORMAT_YCBCR_420_888);
  auto blob_output_sizes = map->GetOutputSizes(HAL_PIXEL_FORMAT_BLOB);
  auto depth16_output_sizes = map->GetOutputSizes(HAL_PIXEL_FORMAT_Y16);
  auto priv_output_sizes =
      map->GetOutputSizes(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);

  if (!SupportsCapability(
          camera_id, static_metadata,
          ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE) &&
      IsMaxSupportedSizeGreaterThanOrEqual(depth16_output_sizes,
                                           s240pStreamSize)) {
    ALOGI("%s: Depth only output supported by camera id %u", __FUNCTION__,
          camera_id);
    return true;
  }
  if (yuv_output_sizes.empty()) {
    ALOGW("%s: No YUV output supported by camera id %u", __FUNCTION__,
          camera_id);
    return false;
  }

  if (priv_output_sizes.empty()) {
    ALOGW("No PRIV output supported by camera id %u", camera_id);
    return false;
  }

  if (blob_output_sizes.empty()) {
    ALOGW("No BLOB output supported by camera id %u", camera_id);
    return false;
  }

  // According to the HAL spec, if a device supports format sizes > 1440p and
  // 720p, it must support both 1440p and 720p streams for PRIV, JPEG and YUV
  // formats. Otherwise it must support 2 streams (YUV / PRIV + JPEG) of the max
  // supported size.

  // Check for YUV output sizes
  if (IsMaxSupportedSizeGreaterThanOrEqual(yuv_output_sizes, s1440pStreamSize) &&
      (yuv_output_sizes.find(s1440pStreamSize) == yuv_output_sizes.end() ||
       yuv_output_sizes.find(s720pStreamSize) == yuv_output_sizes.end())) {
    ALOGW("%s: 1440p+720p YUV outputs not found for camera id %u", __FUNCTION__,
          camera_id);
    return false;
  } else if (IsMaxSupportedSizeGreaterThanOrEqual(yuv_output_sizes,
                                                  s720pStreamSize) &&
             yuv_output_sizes.find(s720pStreamSize) == yuv_output_sizes.end()) {
    ALOGW("%s: 720p YUV output not found for camera id %u", __FUNCTION__,
          camera_id);
    return false;
  }

  // Check for PRIV output sizes
  if (IsMaxSupportedSizeGreaterThanOrEqual(priv_output_sizes, s1440pStreamSize) &&
      (priv_output_sizes.find(s1440pStreamSize) == priv_output_sizes.end() ||
       priv_output_sizes.find(s720pStreamSize) == priv_output_sizes.end())) {
    ALOGW("%s: 1440p + 720p PRIV outputs not found for camera id %u",
          __FUNCTION__, camera_id);
    return false;
  } else if (IsMaxSupportedSizeGreaterThanOrEqual(priv_output_sizes,
                                                  s720pStreamSize) &&
             priv_output_sizes.find(s720pStreamSize) == priv_output_sizes.end()) {
    ALOGW("%s: 720p PRIV output not found for camera id %u", __FUNCTION__,
          camera_id);
    return false;
  }

  // Check for BLOB output sizes
  if (IsMaxSupportedSizeGreaterThanOrEqual(blob_output_sizes, s1440pStreamSize) &&
      (blob_output_sizes.find(s1440pStreamSize) == blob_output_sizes.end() ||
       blob_output_sizes.find(s720pStreamSize) == blob_output_sizes.end())) {
    ALOGW("%s: 1440p + 720p BLOB outputs not found for camera id %u",
          __FUNCTION__, camera_id);
    return false;
  } else if (IsMaxSupportedSizeGreaterThanOrEqual(blob_output_sizes,
                                                  s720pStreamSize) &&
             blob_output_sizes.find(s720pStreamSize) == blob_output_sizes.end()) {
    ALOGW("%s: 720p BLOB output not found for camera id %u", __FUNCTION__,
          camera_id);
    return false;
  }

  return true;
}

status_t EmulatedCameraProviderHwlImpl::GetConcurrentStreamingCameraIds(
    std::vector<std::unordered_set<uint32_t>>* combinations) {
  if (combinations == nullptr) {
    return BAD_VALUE;
  }
  // Collect all camera ids that support the guaranteed stream combinations
  // (720p YUV and IMPLEMENTATION_DEFINED) and put them in one set. We don't
  // make all possible combinations since it should be possible to stream all
  // of them at once in the emulated camera.
  std::unordered_set<uint32_t> candidate_ids;
  for (auto const& [camera_id, _] : camera_id_map_) {
    if (SupportsMandatoryConcurrentStreams(camera_id)) {
      candidate_ids.insert(camera_id);
    }
  }
  combinations->emplace_back(std::move(candidate_ids));
  return OK;
}

status_t EmulatedCameraProviderHwlImpl::DumpState(int /*fd*/) {
  return OK;
}

status_t EmulatedCameraProviderHwlImpl::IsConcurrentStreamCombinationSupported(
    const std::vector<CameraIdAndStreamConfiguration>& configs,
    bool* is_supported) {
  *is_supported = false;

  // Go through the given camera ids, get their sensor characteristics, stream
  // config maps and call EmulatedSensor::IsStreamCombinationSupported()
  for (auto& config : configs) {
    uint32_t camera_id = config.camera_id;
    // TODO: Consider caching sensor characteristics and StreamConfigurationMap
    if (camera_id_map_.find(camera_id) == camera_id_map_.end()) {
      ALOGE("%s: Camera id %u does not exist", __FUNCTION__, camera_id);
      return BAD_VALUE;
    }

    auto stream_configuration_map = std::make_unique<StreamConfigurationMap>(
        *(static_metadata_.at(camera_id)));
    auto stream_configuration_map_max_resolution =
        std::make_unique<StreamConfigurationMap>(
            *(static_metadata_.at(camera_id)), /*maxResolution*/ true);

    LogicalCharacteristics sensor_chars;
    status_t ret = GetSensorCharacteristics(
        (static_metadata_.at(camera_id)).get(), &sensor_chars[camera_id]);
    if (ret != OK) {
      ALOGE("%s: Unable to extract sensor chars for camera id %u", __FUNCTION__,
            camera_id);
      return UNKNOWN_ERROR;
    }

    PhysicalStreamConfigurationMap physical_stream_configuration_map;
    PhysicalStreamConfigurationMap physical_stream_configuration_map_max_resolution;
    auto const& physicalCameraInfo = camera_id_map_.at(camera_id);
    for (size_t i = 0; i < physicalCameraInfo.size(); i++) {
      uint32_t physical_camera_id = physicalCameraInfo[i].second;
      physical_stream_configuration_map.emplace(
          physical_camera_id, std::make_unique<StreamConfigurationMap>(
                                  *(static_metadata_.at(physical_camera_id))));

      physical_stream_configuration_map_max_resolution.emplace(
          physical_camera_id, std::make_unique<StreamConfigurationMap>(
                                  *(static_metadata_.at(physical_camera_id)),
                                  /*maxResolution*/ true));

      ret = GetSensorCharacteristics(
          static_metadata_.at(physical_camera_id).get(),
          &sensor_chars[physical_camera_id]);
      if (ret != OK) {
        ALOGE("%s: Unable to extract camera %d sensor characteristics %s (%d)",
              __FUNCTION__, physical_camera_id, strerror(-ret), ret);
        return ret;
      }
    }

    if (!EmulatedSensor::IsStreamCombinationSupported(
            camera_id, config.stream_configuration, *stream_configuration_map,
            *stream_configuration_map_max_resolution,
            physical_stream_configuration_map,
            physical_stream_configuration_map_max_resolution, sensor_chars)) {
      return OK;
    }
  }

  *is_supported = true;
  return OK;
}

status_t EmulatedCameraProviderHwlImpl::Initialize() {
  std::vector<CameraConfiguration> camera_configs;
  status_t res = GetCameraConfigurations(&camera_configs);
  if (res != OK) {
    return res;
  }

  for (auto& config : camera_configs) {
    static_metadata_[config.id] = std::move(config.characteristics);
    if (!config.physical_camera_characteristics.empty()) {
      // This is a logical camera
      std::vector<std::pair<CameraDeviceStatus, uint32_t>> physical_ids;
      size_t physical_device_count = 0;
      for (auto const& [physical_id, physical_chars] :
           config.physical_camera_characteristics) {
        static_metadata_[physical_id] =
            HalCameraMetadata::Clone(physical_chars.get());
        auto device_status = (physical_device_count < 2)
                                 ? CameraDeviceStatus::kPresent
                                 : CameraDeviceStatus::kNotPresent;
        physical_ids.push_back({device_status, physical_id});
        physical_device_count++;
      }
      camera_id_map_[config.id] = std::move(physical_ids);

      auto physical_devices = std::make_unique<PhysicalDeviceMap>();
      for (const auto& physical_device : camera_id_map_.at(config.id)) {
        physical_devices->emplace(
            physical_device.second,
            std::make_pair(
                physical_device.first,
                HalCameraMetadata::Clone(
                    static_metadata_.at(physical_device.second).get())));
      }
      auto updated_logical_chars =
          EmulatedLogicalRequestState::AdaptLogicalCharacteristics(
              HalCameraMetadata::Clone(static_metadata_.at(config.id).get()),
              std::move(physical_devices));
      if (updated_logical_chars.get() != nullptr) {
        static_metadata_.at(config.id).swap(updated_logical_chars);
      } else {
        ALOGE(
            "%s: Failed to updating logical camera characteristics for id %u!",
            __FUNCTION__, config.id);
        return BAD_VALUE;
      }
    } else {
      camera_id_map_[config.id] = {};
    }
  }

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::SetCallback(
    const HwlCameraProviderCallback& callback) {
  torch_cb_ = callback.torch_mode_status_change;
  physical_camera_status_cb_ = callback.physical_camera_device_status_change;

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::TriggerDeferredCallbacks() {
  std::lock_guard<std::mutex> lock(status_callback_future_lock_);
  if (status_callback_future_.valid()) {
    return OK;
  }

  status_callback_future_ = std::async(
      std::launch::async,
      &EmulatedCameraProviderHwlImpl::NotifyPhysicalCameraUnavailable, this);
  return OK;
}

void EmulatedCameraProviderHwlImpl::WaitForStatusCallbackFuture() {
  {
    std::lock_guard<std::mutex> lock(status_callback_future_lock_);
    if (!status_callback_future_.valid()) {
      // If there is no future pending, construct an empty one.
      status_callback_future_ = std::async([]() { return; });
    }
  }
  status_callback_future_.wait();
}

void EmulatedCameraProviderHwlImpl::NotifyPhysicalCameraUnavailable() {
  for (const auto& one_map : camera_id_map_) {
    for (const auto& physical_device : one_map.second) {
      if (physical_device.first != CameraDeviceStatus::kNotPresent) {
        continue;
      }

      uint32_t logical_camera_id = one_map.first;
      uint32_t physical_camera_id = physical_device.second;
      physical_camera_status_cb_(
          logical_camera_id, physical_camera_id,
          CameraDeviceStatus::kNotPresent);
    }
  }
}

status_t EmulatedCameraProviderHwlImpl::GetVendorTags(
    std::vector<VendorTagSection>* vendor_tag_sections) {
  if (vendor_tag_sections == nullptr) {
    ALOGE("%s: vendor_tag_sections is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  // No vendor specific tags as of now
  return OK;
}

status_t EmulatedCameraProviderHwlImpl::GetVisibleCameraIds(
    std::vector<uint32_t>* camera_ids) {
  if (camera_ids == nullptr) {
    ALOGE("%s: camera_ids is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  for (const auto& device : camera_id_map_) {
    camera_ids->push_back(device.first);
  }

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::CreateCameraDeviceHwl(
    uint32_t camera_id, std::unique_ptr<CameraDeviceHwl>* camera_device_hwl) {
  if (camera_device_hwl == nullptr) {
    ALOGE("%s: camera_device_hwl is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  if (camera_id_map_.find(camera_id) == camera_id_map_.end()) {
    ALOGE("%s: Invalid camera id: %u", __func__, camera_id);
    return BAD_VALUE;
  }

  std::unique_ptr<HalCameraMetadata> meta =
      HalCameraMetadata::Clone(static_metadata_.at(camera_id).get());

  std::shared_ptr<EmulatedTorchState> torch_state;
  camera_metadata_ro_entry entry;
  bool flash_supported = false;
  auto ret = meta->Get(ANDROID_FLASH_INFO_AVAILABLE, &entry);
  if ((ret == OK) && (entry.count == 1)) {
    if (entry.data.u8[0] == ANDROID_FLASH_INFO_AVAILABLE_TRUE) {
      flash_supported = true;
    }
  }

  if (flash_supported) {
    torch_state = std::make_shared<EmulatedTorchState>(camera_id, torch_cb_);
  }

  auto physical_devices = std::make_unique<PhysicalDeviceMap>();
  for (const auto& physical_device : camera_id_map_.at(camera_id)) {
    physical_devices->emplace(
        physical_device.second,
        std::make_pair(physical_device.first,
                       HalCameraMetadata::Clone(
                           static_metadata_.at(physical_device.second).get())));
  }
  *camera_device_hwl = EmulatedCameraDeviceHwlImpl::Create(
      camera_id, std::move(meta), std::move(physical_devices), torch_state);
  if (*camera_device_hwl == nullptr) {
    ALOGE("%s: Cannot create EmulatedCameraDeviceHWlImpl.", __FUNCTION__);
    return BAD_VALUE;
  }

  return OK;
}

status_t EmulatedCameraProviderHwlImpl::CreateBufferAllocatorHwl(
    std::unique_ptr<CameraBufferAllocatorHwl>* camera_buffer_allocator_hwl) {
  if (camera_buffer_allocator_hwl == nullptr) {
    ALOGE("%s: camera_buffer_allocator_hwl is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  // Currently not supported
  return INVALID_OPERATION;
}

status_t EmulatedCameraProviderHwlImpl::NotifyDeviceStateChange(
    DeviceState /*device_state*/) {
  return OK;
}
}  // namespace android

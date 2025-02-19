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

#ifndef EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_HWL_H
#define EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_HWL_H

#include <camera_device_hwl.h>
#include <hal_types.h>

#include <vector>

#include "EmulatedCameraDeviceInfo.h"
#include "EmulatedSensor.h"
#include "EmulatedTorchState.h"
#include "utils/HWLUtils.h"
#include "utils/StreamConfigurationMap.h"

namespace android {

using google_camera_hal::CameraBufferAllocatorHwl;
using google_camera_hal::CameraDeviceHwl;
using google_camera_hal::CameraDeviceSessionHwl;
using google_camera_hal::CameraResourceCost;
using google_camera_hal::HalCameraMetadata;
using google_camera_hal::HwlMemoryConfig;
using google_camera_hal::kTemplateCount;
using google_camera_hal::RequestTemplate;
using google_camera_hal::StreamConfiguration;
using google_camera_hal::TorchMode;

class EmulatedCameraDeviceHwlImpl : public CameraDeviceHwl {
 public:
  static std::unique_ptr<CameraDeviceHwl> Create(
      uint32_t camera_id, std::unique_ptr<HalCameraMetadata> static_meta,
      PhysicalDeviceMapPtr physical_devices,
      std::shared_ptr<EmulatedTorchState> torch_state);

  virtual ~EmulatedCameraDeviceHwlImpl() = default;

  // Override functions in CameraDeviceHwl.
  uint32_t GetCameraId() const override;

  status_t GetResourceCost(CameraResourceCost* cost) const override;

  status_t GetCameraCharacteristics(
      std::unique_ptr<HalCameraMetadata>* characteristics) const override;

  status_t GetSessionCharacteristics(
      const StreamConfiguration& session_config,
      std::unique_ptr<HalCameraMetadata>& characteristics) const override;

  std::vector<uint32_t> GetPhysicalCameraIds() const override;

  status_t GetPhysicalCameraCharacteristics(
      uint32_t physical_camera_id,
      std::unique_ptr<HalCameraMetadata>* characteristics) const override;

  HwlMemoryConfig GetMemoryConfig() const override;

  status_t SetTorchMode(TorchMode mode) override;

  status_t TurnOnTorchWithStrengthLevel(int32_t torch_strength) override;

  status_t GetTorchStrengthLevel(int32_t& torch_strength) const override;

  status_t ConstructDefaultRequestSettings(
      RequestTemplate type,
      std::unique_ptr<HalCameraMetadata>* request_settings) override;

  status_t DumpState(int fd) override;

  status_t CreateCameraDeviceSessionHwl(
      CameraBufferAllocatorHwl* camera_allocator_hwl,
      std::unique_ptr<CameraDeviceSessionHwl>* session) override;

  bool IsStreamCombinationSupported(const StreamConfiguration& stream_config,
                                    const bool /*check_settings*/) const override;

  // End of override functions in CameraDeviceHwl.

 private:
  EmulatedCameraDeviceHwlImpl(uint32_t camera_id,
                              std::unique_ptr<HalCameraMetadata> static_meta,
                              PhysicalDeviceMapPtr physical_devices,
                              std::shared_ptr<EmulatedTorchState> torch_state);

  status_t Initialize();

  int32_t GetDefaultTorchStrengthLevel() const;
  int32_t GetMaximumTorchStrengthLevel() const;

  const uint32_t camera_id_ = 0;

  std::unique_ptr<HalCameraMetadata> static_metadata_;
  std::unique_ptr<EmulatedCameraDeviceInfo> device_info_;
  std::unique_ptr<StreamConfigurationMap> stream_configuration_map_;
  std::unique_ptr<StreamConfigurationMap> stream_configuration_map_max_resolution_;
  PhysicalStreamConfigurationMap physical_stream_configuration_map_;
  PhysicalStreamConfigurationMap physical_stream_configuration_map_max_resolution_;
  PhysicalDeviceMapPtr physical_device_map_;
  std::shared_ptr<EmulatedTorchState> torch_state_;
  LogicalCharacteristics sensor_chars_;
  int32_t default_torch_strength_level_ = 0;
  int32_t maximum_torch_strength_level_ = 0;

};

}  // namespace android

#endif  // EMULATOR_CAMERA_HAL_HWL_CAMERA_DEVICE_HWL_H

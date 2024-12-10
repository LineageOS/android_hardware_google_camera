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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_CAMERA_DEVICE_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_CAMERA_DEVICE_H_

#include <map>

#include "camera_buffer_allocator_hwl.h"
#include "camera_device_hwl.h"
#include "camera_device_session.h"
#include "hal_camera_metadata.h"
#include "hwl_types.h"
#include "profiler.h"

namespace android {
namespace google_camera_hal {

// Camera Device implements ICameraDevice. It provides methods to query static
// information about a camera device and create a camera device session for
// active use. It does not hold any states of the camera device.
class CameraDevice {
 public:
  // Create a camera device given a camera device HWL.
  // camera_device_hwl must be valid.
  // camera_allocator_hwl is owned by the caller and must be valid during the
  // lifetime of CameraDevice
  static std::unique_ptr<CameraDevice> Create(
      std::unique_ptr<CameraDeviceHwl> camera_device_hwl,
      CameraBufferAllocatorHwl* camera_allocator_hwl = nullptr);

  virtual ~CameraDevice();

  // Get the resource cost of this camera device.
  status_t GetResourceCost(CameraResourceCost* cost);

  // Get the characteristics of this camera device.
  // characteristics will be filled with this camera device's characteristics.
  status_t GetCameraCharacteristics(
      std::unique_ptr<HalCameraMetadata>* characteristics);

  // For certain feature combinations, some keys in camera characteristics
  // have more limited support range compared with that returned by
  // GetCameraCharacterics. This function will return the limited values of the
  // keys listed in CameraCharacteristics#getAvailableSessionCharacteristicsKeys
  // for the input StreamConfiguration.
  //
  // stream_config includes the requested streams and session settings for
  // which we are going to fetch the characteristics.
  //
  // session_characteristic will be filled with the session characteristics keys
  // with their limited ranges.
  status_t GetSessionCharacteristics(
      const StreamConfiguration& stream_config,
      std::unique_ptr<HalCameraMetadata>& session_characteristics);

  // Get the characteristics of this camera device's physical camera if the
  // physical_camera_id belongs to this camera device.
  // characteristics will be filled with the physical camera ID's
  // characteristics.
  status_t GetPhysicalCameraCharacteristics(
      uint32_t physical_camera_id,
      std::unique_ptr<HalCameraMetadata>* characteristics);

  // Set the torch mode of the camera device. The torch mode status remains
  // unchanged after this CameraDevice instance is destroyed.
  status_t SetTorchMode(TorchMode mode);

  // Change the brightness level of the flash unit in Torch mode.
  // If the torch is OFF and torchStrength > 0, the torch will be turned ON.
  status_t TurnOnTorchWithStrengthLevel(int32_t torch_strength);

  // Get the flash unit strength level of this camera device.
  status_t GetTorchStrengthLevel(int32_t& torch_strength) const;

  // Construct default request settings
  status_t ConstructDefaultRequestSettings(
      RequestTemplate type,
      std::unique_ptr<HalCameraMetadata>* request_settings);

  // Create a CameraDeviceSession to handle capture requests. This method will
  // return ALREADY_EXISTS if previous session has not been destroyed.
  // Created CameraDeviceSession remain valid even after this CameraDevice
  // instance is destroyed.
  status_t CreateCameraDeviceSession(
      std::unique_ptr<CameraDeviceSession>* session);

  // Dump the camera device states in fd, using dprintf() or write().
  status_t DumpState(int fd);

  // Get the public camera ID for this camera device.
  uint32_t GetPublicCameraId() const {
    return public_camera_id_;
  };

  // Get the applied memory config for this camera device.
  HwlMemoryConfig GetAppliedMemoryConfig() {
    HwlMemoryConfig memory_config = applied_memory_config_;
    return memory_config;
  }

  // Set the applied memory config for this camera device.
  void SetAppliedMemoryConfig(HwlMemoryConfig memory_config) {
    applied_memory_config_ = memory_config;
  }

  // Query whether a particular streams configuration is supported.
  // stream_config: It contains the stream info and a set of features, which are
  // described in the form of session settings.
  // check_settings: When check_settings is true, this function will check if
  // the input features combination in stream_config is supported. The feature
  // set camera hwl has to scan for reporting support status is defined in
  // framework by CameraCharacteristics#INFO_SESSION_CONFIGURATION_QUERY_VERSION.
  bool IsStreamCombinationSupported(const StreamConfiguration& stream_config,
                                    bool check_settings);

  status_t LoadExternalCaptureSession();

  std::unique_ptr<google::camera_common::Profiler> GetProfiler(uint32_t camere_id,
                                                               int option);

 protected:
  CameraDevice() = default;

 private:
  status_t Initialize(std::unique_ptr<CameraDeviceHwl> camera_device_hwl,
                      CameraBufferAllocatorHwl* camera_allocator_hwl);

  static HwlMemoryConfig applied_memory_config_;
  static std::mutex applied_memory_config_mutex_;

  uint32_t public_camera_id_ = 0;

  std::unique_ptr<CameraDeviceHwl> camera_device_hwl_;

  // hwl allocator
  CameraBufferAllocatorHwl* camera_allocator_hwl_ = nullptr;

  std::vector<GetCaptureSessionFactoryFunc> external_session_factory_entries_;
  // Opened library handles that should be closed on destruction
  std::vector<void*> external_capture_session_lib_handles_;
  // Stream use cases supported by this camera device
  std::map<uint32_t, std::set<int64_t>> camera_id_to_stream_use_cases_;
};

}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_CAMERA_DEVICE_H_

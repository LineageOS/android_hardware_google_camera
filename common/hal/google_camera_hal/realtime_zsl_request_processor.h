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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_REQUEST_PROCESSOR_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_REQUEST_PROCESSOR_H_

#include <shared_mutex>
#include <vector>

#include "process_block.h"
#include "request_processor.h"
#include "vendor_tag_types.h"

namespace android {
namespace google_camera_hal {

// RealtimeZslRequestProcessor implements a RequestProcessor that adds
// internal stream to request and forwards the request to its ProcessBlock.
class RealtimeZslRequestProcessor : public RequestProcessor {
 public:
  // device_session_hwl is owned by the caller and must be valid during the
  // lifetime of this RealtimeZslRequestProcessor.
  static std::unique_ptr<RealtimeZslRequestProcessor> Create(
      CameraDeviceSessionHwl* device_session_hwl,
      android_pixel_format_t pixel_format);

  virtual ~RealtimeZslRequestProcessor() = default;

  // Override functions of RequestProcessor start.
  // RealtimeZslRequestProcessor will configure all streams in stream_config.
  // And register one internal stream
  status_t ConfigureStreams(
      InternalStreamManager* internal_stream_manager,
      const StreamConfiguration& stream_config,
      StreamConfiguration* process_block_stream_config) override;

  // Set the realtime process block for sending requests later.
  status_t SetProcessBlock(std::unique_ptr<ProcessBlock> process_block) override;

  // Add one additional output to capture request
  // And forwards the capture request to realtime process
  status_t ProcessRequest(const CaptureRequest& request) override;

  status_t Flush() override;

  void RepeatingRequestEnd(int32_t frame_number,
                           const std::vector<int32_t>& stream_ids) override;
  // Override functions of RequestProcessor end.

 protected:
  RealtimeZslRequestProcessor(CameraDeviceSessionHwl* device_session_hwl)
      : device_session_hwl_(device_session_hwl) {};

 private:
  status_t Initialize(CameraDeviceSessionHwl* device_session_hwl);
  std::shared_mutex process_block_lock_;

  // Protected by process_block_lock_.
  std::unique_ptr<ProcessBlock> process_block_;

  InternalStreamManager* internal_stream_manager_ = nullptr;
  CameraDeviceSessionHwl* device_session_hwl_ = nullptr;
  bool preview_intent_seen_ = false;
  int32_t stream_id_ = -1;
  uint32_t active_array_width_ = 0;
  uint32_t active_array_height_ = 0;
};

}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_REQUEST_PROCESSOR_H_

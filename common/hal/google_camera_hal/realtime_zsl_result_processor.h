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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_RESULT_PROCESSOR_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_RESULT_PROCESSOR_H_

#include <shared_mutex>

#include "internal_stream_manager.h"
#include "result_processor.h"

namespace android {
namespace google_camera_hal {

// RealtimeZslResultProcessor implements a ResultProcessor that return
// filled buffer and metadata to internal stream manager.
class RealtimeZslResultProcessor : public ResultProcessor {
 public:
  static std::unique_ptr<RealtimeZslResultProcessor> Create(
      InternalStreamManager* internal_stream_manager, int32_t stream_id,
      android_pixel_format_t pixel_format, uint32_t partial_result_count = 1);

  virtual ~RealtimeZslResultProcessor() = default;

  // Override functions of ResultProcessor start.
  void SetResultCallback(
      ProcessCaptureResultFunc process_capture_result, NotifyFunc notify,
      ProcessBatchCaptureResultFunc process_batch_capture_result) override;

  status_t AddPendingRequests(
      const std::vector<ProcessBlockRequest>& process_block_requests,
      const CaptureRequest& remaining_session_request) override;

  // Return filled buffer and metadata to internal stream manager
  // and forwards the results without buffer to its callback functions.
  void ProcessResult(ProcessBlockResult block_result) override;

  void Notify(const ProcessBlockNotifyMessage& block_message) override;

  status_t FlushPendingRequests() override;
  // Override functions of ResultProcessor end.

 protected:
  RealtimeZslResultProcessor(InternalStreamManager* internal_stream_manager,
                             int32_t stream_id, uint32_t partial_result_count);

  InternalStreamManager* internal_stream_manager_;
  int32_t stream_id_ = -1;
  // Partial result count reported by HAL
  uint32_t partial_result_count_;

  std::mutex callback_lock_;

  // The following callbacks must be protected by callback_lock_.
  ProcessCaptureResultFunc process_capture_result_;
  NotifyFunc notify_;

 private:
  std::shared_mutex process_block_shared_lock_;
};

}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_GOOGLE_CAMERA_HAL_REALTIME_ZSL_RESULT_REQUEST_PROCESSOR_H_

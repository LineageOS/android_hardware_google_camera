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
#include "hal_types.h"
#define LOG_TAG "GCH_RealtimeZslResultProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <inttypes.h>
#include <log/log.h>
#include <utils/Trace.h>

#include "hal_utils.h"
#include "realtime_zsl_result_processor.h"

namespace android {
namespace google_camera_hal {

std::unique_ptr<RealtimeZslResultProcessor> RealtimeZslResultProcessor::Create(
    InternalStreamManager* internal_stream_manager, int32_t stream_id,
    android_pixel_format_t pixel_format, uint32_t partial_result_count) {
  ATRACE_CALL();
  if (internal_stream_manager == nullptr) {
    ALOGE("%s: internal_stream_manager is nullptr.", __FUNCTION__);
    return nullptr;
  }
  if (pixel_format != android_pixel_format_t::HAL_PIXEL_FORMAT_YCBCR_420_888) {
    ALOGE("%s: only YCBCR_420_888 is supported for YUV ZSL", __FUNCTION__);
    return nullptr;
  }

  auto result_processor =
      std::unique_ptr<RealtimeZslResultProcessor>(new RealtimeZslResultProcessor(
          internal_stream_manager, stream_id, partial_result_count));
  if (result_processor == nullptr) {
    ALOGE("%s: Creating RealtimeZslResultProcessor failed.", __FUNCTION__);
    return nullptr;
  }

  return result_processor;
}

RealtimeZslResultProcessor::RealtimeZslResultProcessor(
    InternalStreamManager* internal_stream_manager, int32_t stream_id,
    uint32_t partial_result_count) {
  internal_stream_manager_ = internal_stream_manager;
  stream_id_ = stream_id;
  partial_result_count_ = partial_result_count;
}

void RealtimeZslResultProcessor::SetResultCallback(
    ProcessCaptureResultFunc process_capture_result, NotifyFunc notify,
    ProcessBatchCaptureResultFunc /*process_batch_capture_result*/) {
  std::lock_guard<std::mutex> lock(callback_lock_);
  process_capture_result_ = process_capture_result;
  notify_ = notify;
}

status_t RealtimeZslResultProcessor::AddPendingRequests(
    const std::vector<ProcessBlockRequest>& process_block_requests,
    const CaptureRequest& remaining_session_request) {
  ATRACE_CALL();
  // This is the last result processor. Sanity check if requests contains
  // all remaining output buffers.
  if (!hal_utils::AreAllRemainingBuffersRequested(process_block_requests,
                                                  remaining_session_request)) {
    ALOGE("%s: Some output buffers will not be completed.", __FUNCTION__);
    return BAD_VALUE;
  }

  return OK;
}

void RealtimeZslResultProcessor::ProcessResult(ProcessBlockResult block_result) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(callback_lock_);
  std::unique_ptr<CaptureResult> result = std::move(block_result.result);
  if (result == nullptr) {
    ALOGW("%s: Received a nullptr result.", __FUNCTION__);
    return;
  }

  if (process_capture_result_ == nullptr) {
    ALOGE("%s: process_capture_result_ is nullptr. Dropping a result.",
          __FUNCTION__);
    return;
  }

  // Return filled buffer to internal stream manager
  // And remove buffer from result
  bool returned_output = false;
  status_t res;
  std::vector<StreamBuffer> modified_output_buffers;
  for (uint32_t i = 0; i < result->output_buffers.size(); i++) {
    if (stream_id_ == result->output_buffers[i].stream_id) {
      returned_output = true;
      res = internal_stream_manager_->ReturnFilledBuffer(
          result->frame_number, result->output_buffers[i]);
      if (res != OK) {
        ALOGW("%s: (%d)ReturnStreamBuffer fail", __FUNCTION__,
              result->frame_number);
      }
    } else {
      modified_output_buffers.push_back(result->output_buffers[i]);
    }
  }

  if (result->output_buffers.size() > 0) {
    result->output_buffers.clear();
    result->output_buffers = modified_output_buffers;
  }

  if (result->result_metadata) {
    result->result_metadata->Erase(ANDROID_CONTROL_ENABLE_ZSL);

    res = internal_stream_manager_->ReturnMetadata(
        stream_id_, result->frame_number, result->result_metadata.get(),
        result->partial_result);
    if (res != OK) {
      ALOGW("%s: (%d)ReturnMetadata fail", __FUNCTION__, result->frame_number);
    }

    if (result->partial_result == partial_result_count_) {
      res =
          hal_utils::SetEnableZslMetadata(result->result_metadata.get(), false);
      if (res != OK) {
        ALOGW("%s: SetEnableZslMetadata (%d) fail", __FUNCTION__,
              result->frame_number);
      }
    }
  }

  // Don't send result to framework if only internal callback
  if (returned_output && result->result_metadata == nullptr &&
      result->output_buffers.size() == 0) {
    return;
  }

  process_capture_result_(std::move(result));
}

void RealtimeZslResultProcessor::Notify(
    const ProcessBlockNotifyMessage& block_message) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(callback_lock_);
  const NotifyMessage& message = block_message.message;
  if (notify_ == nullptr) {
    ALOGE("%s: notify_ is nullptr. Dropping a message.", __FUNCTION__);
    return;
  }

  // Do not notify errors for internal streams
  if (message.type == MessageType::kError &&
      message.message.error.error_stream_id == stream_id_) {
    return;
  }

  notify_(message);
}

status_t RealtimeZslResultProcessor::FlushPendingRequests() {
  ATRACE_CALL();
  return INVALID_OPERATION;
}

}  // namespace google_camera_hal
}  // namespace android

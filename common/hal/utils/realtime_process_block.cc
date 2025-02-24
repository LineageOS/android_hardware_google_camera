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
#define LOG_TAG "GCH_RealtimeProcessBlock"
#define ATRACE_TAG ATRACE_TAG_CAMERA
#include "realtime_process_block.h"

#include <log/log.h>
#include <utils/Trace.h>

#include "hal_utils.h"
#include "result_processor.h"

namespace android {
namespace google_camera_hal {

std::unique_ptr<RealtimeProcessBlock> RealtimeProcessBlock::Create(
    CameraDeviceSessionHwl* device_session_hwl) {
  ATRACE_CALL();
  if (!IsSupported(device_session_hwl)) {
    ALOGE("%s: Not supported.", __FUNCTION__);
    return nullptr;
  }

  auto block = std::unique_ptr<RealtimeProcessBlock>(
      new RealtimeProcessBlock(device_session_hwl));
  if (block == nullptr) {
    ALOGE("%s: Creating RealtimeProcessBlock failed.", __FUNCTION__);
    return nullptr;
  }

  return block;
}

bool RealtimeProcessBlock::IsSupported(
    CameraDeviceSessionHwl* device_session_hwl) {
  if (device_session_hwl == nullptr) {
    ALOGE("%s: device_session_hwl is nullptr", __FUNCTION__);
    return false;
  }

  return true;
}

RealtimeProcessBlock::RealtimeProcessBlock(
    CameraDeviceSessionHwl* device_session_hwl)
    : kCameraId(device_session_hwl->GetCameraId()),
      device_session_hwl_(device_session_hwl) {
  hwl_pipeline_callback_.process_pipeline_result = HwlProcessPipelineResultFunc(
      [this](std::unique_ptr<HwlPipelineResult> result) {
        NotifyHwlPipelineResult(std::move(result));
      });

  hwl_pipeline_callback_.process_pipeline_batch_result =
      HwlProcessPipelineBatchResultFunc(
          [this](std::vector<std::unique_ptr<HwlPipelineResult>> results) {
            NotifyHwlPipelineBatchResult(std::move(results));
          });

  hwl_pipeline_callback_.notify = NotifyHwlPipelineMessageFunc(
      [this](uint32_t pipeline_id, const NotifyMessage& message) {
        NotifyHwlPipelineMessage(pipeline_id, message);
      });
}

status_t RealtimeProcessBlock::SetResultProcessor(
    std::unique_ptr<ResultProcessor> result_processor) {
  ATRACE_CALL();
  if (result_processor == nullptr) {
    ALOGE("%s: result_processor is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  std::lock_guard<std::mutex> lock(result_processor_lock_);
  if (result_processor_ != nullptr) {
    ALOGE("%s: result_processor_ was already set.", __FUNCTION__);
    return ALREADY_EXISTS;
  }

  result_processor_ = std::move(result_processor);
  return OK;
}

status_t RealtimeProcessBlock::ConfigureStreams(
    const StreamConfiguration& stream_config,
    const StreamConfiguration& overall_config) {
  ATRACE_CALL();
  std::lock_guard lock(configure_shared_mutex_);
  if (is_configured_) {
    ALOGE("%s: Already configured.", __FUNCTION__);
    return ALREADY_EXISTS;
  }

  status_t res = device_session_hwl_->ConfigurePipeline(
      kCameraId, hwl_pipeline_callback_, stream_config, overall_config,
      &pipeline_id_);
  if (res != OK) {
    ALOGE("%s: Configuring a pipeline failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  is_configured_ = true;
  return OK;
}

status_t RealtimeProcessBlock::GetConfiguredHalStreams(
    std::vector<HalStream>* hal_streams) const {
  ATRACE_CALL();
  std::lock_guard lock(configure_shared_mutex_);
  if (hal_streams == nullptr) {
    ALOGE("%s: hal_streams is nullptr.", __FUNCTION__);
    return BAD_VALUE;
  }

  if (!is_configured_) {
    ALOGE("%s: Not configured yet.", __FUNCTION__);
    return NO_INIT;
  }

  return device_session_hwl_->GetConfiguredHalStream(pipeline_id_, hal_streams);
}

status_t RealtimeProcessBlock::ProcessRequests(
    const std::vector<ProcessBlockRequest>& process_block_requests,
    const CaptureRequest& remaining_session_request) {
  ATRACE_CALL();
  if (process_block_requests.size() != 1) {
    ALOGE("%s: Only a single request is supported but there are %zu",
          __FUNCTION__, process_block_requests.size());
    return BAD_VALUE;
  }

  {
    std::lock_guard<std::mutex> lock(result_processor_lock_);
    if (result_processor_ == nullptr) {
      ALOGE("%s: result processor was not set.", __FUNCTION__);
      return NO_INIT;
    }

    status_t res = result_processor_->AddPendingRequests(
        process_block_requests, remaining_session_request);
    if (res != OK) {
      ALOGE("%s: Adding a pending request to result processor failed: %s(%d)",
            __FUNCTION__, strerror(-res), res);
      return res;
    }
  }

  std::shared_lock lock(configure_shared_mutex_);
  if (!is_configured_) {
    ALOGE("%s: block is not configured.", __FUNCTION__);
    return NO_INIT;
  }

  std::vector<HwlPipelineRequest> hwl_requests(1);
  status_t res = hal_utils::CreateHwlPipelineRequest(
      &hwl_requests[0], pipeline_id_, process_block_requests[0].request);
  if (res != OK) {
    ALOGE("%s: Creating HWL pipeline request failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  return device_session_hwl_->SubmitRequests(
      process_block_requests[0].request.frame_number, hwl_requests);
}

status_t RealtimeProcessBlock::Flush() {
  ATRACE_CALL();
  std::shared_lock lock(configure_shared_mutex_);
  if (!is_configured_) {
    return OK;
  }

  return device_session_hwl_->Flush();
}

void RealtimeProcessBlock::RepeatingRequestEnd(
    int32_t frame_number, const std::vector<int32_t>& stream_ids) {
  ATRACE_CALL();
  std::shared_lock lock(configure_shared_mutex_);
  if (is_configured_) {
    device_session_hwl_->RepeatingRequestEnd(frame_number, stream_ids);
  }
}

void RealtimeProcessBlock::NotifyHwlPipelineResult(
    std::unique_ptr<HwlPipelineResult> hwl_result) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_processor_lock_);
  if (result_processor_ == nullptr) {
    ALOGE("%s: result processor is nullptr. Dropping a result", __FUNCTION__);
    return;
  }

  auto capture_result = hal_utils::ConvertToCaptureResult(std::move(hwl_result));
  if (capture_result == nullptr) {
    ALOGE("%s: Converting to capture result failed.", __FUNCTION__);
    return;
  }

  ProcessBlockResult result = {.result = std::move(capture_result)};
  result_processor_->ProcessResult(std::move(result));
}

void RealtimeProcessBlock::NotifyHwlPipelineBatchResult(
    std::vector<std::unique_ptr<HwlPipelineResult>> hwl_results) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_processor_lock_);
  if (result_processor_ == nullptr) {
    ALOGE("%s: result processor is nullptr. Dropping a result", __FUNCTION__);
    return;
  }

  std::vector<ProcessBlockResult> block_results;
  block_results.reserve(hwl_results.size());
  for (auto& hwl_result : hwl_results) {
    auto capture_result =
        hal_utils::ConvertToCaptureResult(std::move(hwl_result));
    if (capture_result == nullptr) {
      ALOGE("%s: Converting to capture result failed.", __FUNCTION__);
      return;
    }

    block_results.push_back(
        ProcessBlockResult{.result = std::move(capture_result)});
  }
  result_processor_->ProcessBatchResult(std::move(block_results));
}

void RealtimeProcessBlock::NotifyHwlPipelineMessage(
    uint32_t /*pipeline_id*/, const NotifyMessage& message) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_processor_lock_);
  if (result_processor_ == nullptr) {
    ALOGE("%s: result processor is nullptr. Dropping a message", __FUNCTION__);
    return;
  }

  ProcessBlockNotifyMessage block_message = {.message = message};
  result_processor_->Notify(block_message);
}

}  // namespace google_camera_hal
}  // namespace android

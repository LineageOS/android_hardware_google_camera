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
#define LOG_TAG "GCH_BasicCaptureSession"
#define ATRACE_TAG ATRACE_TAG_CAMERA
#include "basic_capture_session.h"

#include <log/log.h>
#include <utils/Trace.h>

#include "basic_request_processor.h"
#include "basic_result_processor.h"
#include "hal_types.h"
#include "realtime_process_block.h"

namespace android {
namespace google_camera_hal {

bool BasicCaptureSession::IsStreamConfigurationSupported(
    CameraDeviceSessionHwl* device_session_hwl,
    const StreamConfiguration& /*stream_config*/) {
  ATRACE_CALL();
  if (device_session_hwl == nullptr) {
    ALOGE("%s: device_session_hwl is nullptr", __FUNCTION__);
    return false;
  }

  ALOGD("%s: BasicCaptureSession supports the stream config", __FUNCTION__);
  return true;
}

std::unique_ptr<CaptureSession> BasicCaptureSession::Create(
    CameraDeviceSessionHwl* device_session_hwl,
    const StreamConfiguration& stream_config,
    ProcessCaptureResultFunc process_capture_result,
    ProcessBatchCaptureResultFunc process_batch_capture_result,
    NotifyFunc notify, HwlSessionCallback /*session_callback*/,
    std::vector<HalStream>* hal_configured_streams,
    CameraBufferAllocatorHwl* /*camera_allocator_hwl*/) {
  ATRACE_CALL();
  auto session = std::unique_ptr<BasicCaptureSession>(new BasicCaptureSession());
  if (session == nullptr) {
    ALOGE("%s: Creating BasicCaptureSession failed.", __FUNCTION__);
    return nullptr;
  }

  status_t res = session->Initialize(
      device_session_hwl, stream_config, process_capture_result,
      process_batch_capture_result, notify, hal_configured_streams);
  if (res != OK) {
    ALOGE("%s: Initializing BasicCaptureSession failed: %s (%d).", __FUNCTION__,
          strerror(-res), res);
    return nullptr;
  }

  return session;
}

BasicCaptureSession::~BasicCaptureSession() {
  if (device_session_hwl_ != nullptr) {
    device_session_hwl_->DestroyPipelines();
  }
}

status_t BasicCaptureSession::ConfigureStreams(
    const StreamConfiguration& stream_config,
    RequestProcessor* request_processor, ProcessBlock* process_block) {
  ATRACE_CALL();
  if (request_processor == nullptr || process_block == nullptr) {
    ALOGE("%s: request_processor (%p) or process_block (%p) is nullptr",
          __FUNCTION__, request_processor, process_block);
    return BAD_VALUE;
  }

  // Configure streams for request processor
  StreamConfiguration process_block_stream_config;
  status_t res = request_processor->ConfigureStreams(
      internal_stream_manager_.get(), stream_config,
      &process_block_stream_config);
  if (res != OK) {
    ALOGE("%s: Configuring stream for request processor failed.", __FUNCTION__);
    return res;
  }

  // Check all streams are configured.
  if (stream_config.streams.size() !=
      process_block_stream_config.streams.size()) {
    ALOGE("%s: stream_config has %zu streams but only configured %zu streams",
          __FUNCTION__, stream_config.streams.size(),
          process_block_stream_config.streams.size());
    return UNKNOWN_ERROR;
  }

  for (auto& stream : stream_config.streams) {
    bool found = false;
    for (auto& configured_stream : process_block_stream_config.streams) {
      if (stream.id == configured_stream.id) {
        found = true;
        break;
      }
    }

    if (!found) {
      ALOGE("%s: Cannot find stream %u in configured streams.", __FUNCTION__,
            stream.id);
      return UNKNOWN_ERROR;
    }
  }

  // Configure streams for process block.
  res = process_block->ConfigureStreams(process_block_stream_config,
                                        stream_config);
  if (res != OK) {
    ALOGE("%s: Configuring stream for process block failed.", __FUNCTION__);
    return res;
  }

  return OK;
}

status_t BasicCaptureSession::BuildPipelines(
    ProcessBlock* process_block,
    std::vector<HalStream>* hal_configured_streams) {
  ATRACE_CALL();
  if (process_block == nullptr || hal_configured_streams == nullptr) {
    ALOGE("%s: process_block (%p) or hal_configured_streams (%p) is nullptr",
          __FUNCTION__, process_block, hal_configured_streams);
    return BAD_VALUE;
  }

  status_t res = device_session_hwl_->BuildPipelines();
  if (res != OK) {
    ALOGE("%s: Building pipelines failed: %s(%d)", __FUNCTION__, strerror(-res),
          res);
    return res;
  }

  res = process_block->GetConfiguredHalStreams(hal_configured_streams);
  if (res != OK) {
    ALOGE("%s: Getting HAL streams failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  return OK;
}

status_t BasicCaptureSession::ConnectProcessChain(
    RequestProcessor* request_processor,
    std::unique_ptr<ProcessBlock> process_block,
    std::unique_ptr<ResultProcessor> result_processor) {
  ATRACE_CALL();
  if (request_processor == nullptr) {
    ALOGE("%s: request_processor is nullptr", __FUNCTION__);
    return BAD_VALUE;
  }

  status_t res = process_block->SetResultProcessor(std::move(result_processor));
  if (res != OK) {
    ALOGE("%s: Setting result process in process block failed.", __FUNCTION__);
    return res;
  }

  res = request_processor->SetProcessBlock(std::move(process_block));
  if (res != OK) {
    ALOGE("%s: Setting process block for BasicRequestProcessor failed: %s(%d)",
          __FUNCTION__, strerror(-res), res);
    return res;
  }

  return OK;
}

status_t BasicCaptureSession::Initialize(
    CameraDeviceSessionHwl* device_session_hwl,
    const StreamConfiguration& stream_config,
    ProcessCaptureResultFunc process_capture_result,
    ProcessBatchCaptureResultFunc process_batch_capture_result,
    NotifyFunc notify, std::vector<HalStream>* hal_configured_streams) {
  ATRACE_CALL();
  if (!IsStreamConfigurationSupported(device_session_hwl, stream_config)) {
    ALOGE("%s: stream configuration is not supported.", __FUNCTION__);
    return BAD_VALUE;
  }

  device_session_hwl_ = device_session_hwl;
  internal_stream_manager_ = InternalStreamManager::Create();
  if (internal_stream_manager_ == nullptr) {
    ALOGE("%s: Cannot create internal stream manager.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  std::unique_ptr<HalCameraMetadata> characteristics;
  uint32_t partial_result_count = 1;
  status_t res = device_session_hwl->GetCameraCharacteristics(&characteristics);
  if (res != OK) {
    ALOGE("GetCameraCharacteristics failed");
    return BAD_VALUE;
  }
  camera_metadata_ro_entry partial_result_entry;
  res = characteristics->Get(ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
                             &partial_result_entry);
  if (res == OK) {
    partial_result_count = partial_result_entry.data.i32[0];
  }

  // Create result dispatcher.
  std::string result_dispatcher_name =
      "Cam" + std::to_string(device_session_hwl_->GetCameraId()) +
      "_ResultDispatcher";
  result_dispatcher_ =
      ResultDispatcher::Create(partial_result_count, process_capture_result,
                               process_batch_capture_result, notify,
                               stream_config, result_dispatcher_name);
  if (result_dispatcher_ == nullptr) {
    ALOGE("Creating ResultDispatcher failed");
    return UNKNOWN_ERROR;
  }

  // Create result processor.
  auto result_processor = BasicResultProcessor::Create();
  if (result_processor == nullptr) {
    ALOGE("%s: Creating BasicResultProcessor failed.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  auto process_capture_result_cb = [this](std::unique_ptr<CaptureResult> result) {
    ProcessCaptureResult(std::move(result));
  };
  auto notify_cb = [this](const NotifyMessage& message) { Notify(message); };
  auto process_batch_capture_result_cb =
      [this](std::vector<std::unique_ptr<CaptureResult>> results) {
        ProcessBatchCaptureResult(std::move(results));
      };
  result_processor->SetResultCallback(process_capture_result_cb, notify_cb,
                                      process_batch_capture_result_cb);

  // Create process block.
  auto process_block = RealtimeProcessBlock::Create(device_session_hwl_);
  if (process_block == nullptr) {
    ALOGE("%s: Creating RealtimeProcessBlock failed.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  // Create request processor.
  request_processor_ = BasicRequestProcessor::Create(device_session_hwl_);
  if (request_processor_ == nullptr) {
    ALOGE("%s: Creating BasicRequestProcessor failed.", __FUNCTION__);
    return UNKNOWN_ERROR;
  }

  res = ConfigureStreams(stream_config, request_processor_.get(),
                         process_block.get());
  if (res != OK) {
    ALOGE("%s: Configuring stream failed: %s(%d)", __FUNCTION__, strerror(-res),
          res);
    return res;
  }

  res = BuildPipelines(process_block.get(), hal_configured_streams);
  if (res != OK) {
    ALOGE("%s: Building pipelines failed: %s(%d)", __FUNCTION__, strerror(-res),
          res);
    return res;
  }

  res = ConnectProcessChain(request_processor_.get(), std::move(process_block),
                            std::move(result_processor));
  if (res != OK) {
    ALOGE("%s: Connecting process chain failed: %s(%d)", __FUNCTION__,
          strerror(-res), res);
    return res;
  }

  return OK;
}

status_t BasicCaptureSession::ProcessRequest(const CaptureRequest& request) {
  ATRACE_CALL();
  result_dispatcher_->AddPendingRequest(request);
  return request_processor_->ProcessRequest(request);
}

status_t BasicCaptureSession::Flush() {
  ATRACE_CALL();
  return request_processor_->Flush();
}

void BasicCaptureSession::RepeatingRequestEnd(
    int32_t frame_number, const std::vector<int32_t>& stream_ids) {
  ATRACE_CALL();
  if (request_processor_ != nullptr) {
    return request_processor_->RepeatingRequestEnd(frame_number, stream_ids);
  }
}

void BasicCaptureSession::ProcessCaptureResult(
    std::unique_ptr<CaptureResult> result) {
  result_dispatcher_->AddResult(std::move(result));
}

void BasicCaptureSession::Notify(const NotifyMessage& message) {
  if (message.type == MessageType::kShutter) {
    result_dispatcher_->AddShutter(message.message.shutter.frame_number,
                                   message.message.shutter.timestamp_ns,
                                   message.message.shutter.readout_timestamp_ns);
  } else {
    result_dispatcher_->AddError(message.message.error);
  }
}

void BasicCaptureSession::ProcessBatchCaptureResult(
    std::vector<std::unique_ptr<CaptureResult>> results) {
  result_dispatcher_->AddBatchResult(std::move(results));
}

}  // namespace google_camera_hal
}  // namespace android

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
#define LOG_TAG "GCH_ResultDispatcher"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include "result_dispatcher.h"

#include <inttypes.h>
#include <log/log.h>
#include <sys/resource.h>
#include <utils/Trace.h>

#include <string>
#include <string_view>

#include "hal_types.h"
#include "utils.h"

namespace android {
namespace google_camera_hal {

std::unique_ptr<ResultDispatcher> ResultDispatcher::Create(
    uint32_t partial_result_count,
    ProcessCaptureResultFunc process_capture_result,
    ProcessBatchCaptureResultFunc process_batch_capture_result,
    NotifyFunc notify, const StreamConfiguration& stream_config,
    std::string_view name) {
  ATRACE_CALL();
  auto dispatcher = std::make_unique<ResultDispatcher>(
      partial_result_count, process_capture_result,
      process_batch_capture_result, notify, stream_config, name);
  if (dispatcher == nullptr) {
    ALOGE("[%s] %s: Creating ResultDispatcher failed.",
          std::string(name).c_str(), __FUNCTION__);
    return nullptr;
  }

  return dispatcher;
}

ResultDispatcher::ResultDispatcher(
    uint32_t partial_result_count,
    ProcessCaptureResultFunc process_capture_result,
    ProcessBatchCaptureResultFunc process_batch_capture_result,
    NotifyFunc notify, const StreamConfiguration& stream_config,
    std::string_view name)
    : kPartialResultCount(partial_result_count),
      name_(name),
      process_capture_result_(process_capture_result),
      process_batch_capture_result_(process_batch_capture_result),
      notify_(notify) {
  ATRACE_CALL();
  pending_shutters_ = DispatchQueue<PendingShutter>(name_, "shutter");
  pending_early_metadata_ =
      DispatchQueue<PendingResultMetadata>(name_, "early result metadata");
  pending_final_metadata_ =
      DispatchQueue<PendingResultMetadata>(name_, "final result metadata");

  notify_callback_thread_ =
      std::thread([this] { this->NotifyCallbackThreadLoop(); });

  // Assign higher priority to reduce the preemption when CPU usage is high
  //
  // As from b/295977499, we need to make it realtime for priority inheritance
  // to avoid CameraServer thread being the bottleneck
  status_t res =
      utils::SetRealtimeThread(notify_callback_thread_.native_handle());
  if (res != OK) {
    ALOGE("[%s] %s: SetRealtimeThread fail", name_.c_str(), __FUNCTION__);
  } else {
    ALOGI("[%s] %s: SetRealtimeThread OK", name_.c_str(), __FUNCTION__);
  }
  InitializeGroupStreamIdsMap(stream_config);
}

ResultDispatcher::~ResultDispatcher() {
  ATRACE_CALL();
  {
    std::unique_lock<std::mutex> lock(notify_callback_lock_);
    notify_callback_thread_exiting_ = true;
  }

  notify_callback_condition_.notify_one();
  notify_callback_thread_.join();
}

void ResultDispatcher::RemovePendingRequest(uint32_t frame_number) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_lock_);
  RemovePendingRequestLocked(frame_number);
}

status_t ResultDispatcher::AddPendingRequest(
    const CaptureRequest& pending_request) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_lock_);

  status_t res = AddPendingRequestLocked(pending_request);
  if (res != OK) {
    ALOGE("[%s] %s: Adding a pending request failed: %s(%d).", name_.c_str(),
          __FUNCTION__, strerror(-res), res);
    RemovePendingRequestLocked(pending_request.frame_number);
    return res;
  }

  return OK;
}

status_t ResultDispatcher::AddPendingRequestLocked(
    const CaptureRequest& pending_request) {
  ATRACE_CALL();
  uint32_t frame_number = pending_request.frame_number;
  const RequestType request_type = pending_request.input_buffers.empty()
                                       ? RequestType::kNormal
                                       : RequestType::kReprocess;

  status_t res = pending_shutters_.AddRequest(frame_number, request_type);
  if (res != OK) {
    ALOGE("[%s] %s: Adding pending shutter for frame %u failed: %s(%d)",
          name_.c_str(), __FUNCTION__, frame_number, strerror(-res), res);
    return res;
  }

  res = pending_early_metadata_.AddRequest(frame_number, request_type);
  if (res != OK) {
    ALOGE("[%s] %s: Adding pending early metadata for frame %u failed: %s(%d)",
          name_.c_str(), __FUNCTION__, frame_number, strerror(-res), res);
    return res;
  }

  res = pending_final_metadata_.AddRequest(frame_number, request_type);
  if (res != OK) {
    ALOGE("[%s] %s: Adding pending final metadata for frame %u failed: %s(%d)",
          name_.c_str(), __FUNCTION__, frame_number, strerror(-res), res);
    return res;
  }

  for (auto& buffer : pending_request.input_buffers) {
    res = AddPendingBufferLocked(frame_number, buffer, request_type);
    if (res != OK) {
      ALOGE("[%s] %s: Adding pending input buffer for frame %u failed: %s(%d)",
            name_.c_str(), __FUNCTION__, frame_number, strerror(-res), res);
      return res;
    }
  }

  for (auto& buffer : pending_request.output_buffers) {
    res = AddPendingBufferLocked(frame_number, buffer, request_type);
    if (res != OK) {
      ALOGE("[%s] %s: Adding pending output buffer for frame %u failed: %s(%d)",
            name_.c_str(), __FUNCTION__, frame_number, strerror(-res), res);
      return res;
    }
  }

  return OK;
}

status_t ResultDispatcher::AddPendingBufferLocked(uint32_t frame_number,
                                                  const StreamBuffer& buffer,
                                                  RequestType request_type) {
  ATRACE_CALL();
  StreamKey stream_key = CreateStreamKey(buffer.stream_id);
  if (!stream_pending_buffers_map_.contains(stream_key)) {
    stream_pending_buffers_map_[stream_key] = DispatchQueue<PendingBuffer>(
        name_, "buffer of stream " + DumpStreamKey(stream_key));
  }

  return stream_pending_buffers_map_[stream_key].AddRequest(frame_number,
                                                            request_type);
}

void ResultDispatcher::RemovePendingRequestLocked(uint32_t frame_number) {
  ATRACE_CALL();
  pending_shutters_.RemoveRequest(frame_number);
  pending_early_metadata_.RemoveRequest(frame_number);
  pending_final_metadata_.RemoveRequest(frame_number);

  for (auto& pending_buffers : stream_pending_buffers_map_) {
    pending_buffers.second.RemoveRequest(frame_number);
  }
}

status_t ResultDispatcher::AddResultImpl(std::unique_ptr<CaptureResult> result) {
  status_t res;
  bool failed = false;
  uint32_t frame_number = result->frame_number;

  if (result->result_metadata != nullptr) {
    res = AddResultMetadata(frame_number, std::move(result->result_metadata),
                            std::move(result->physical_metadata),
                            result->partial_result);
    if (res != OK) {
      ALOGE("[%s] %s: Adding result metadata failed: %s (%d)", name_.c_str(),
            __FUNCTION__, strerror(-res), res);
      failed = true;
    }
  }

  for (auto& buffer : result->output_buffers) {
    res = AddBuffer(frame_number, buffer, /*is_input=*/false);
    if (res != OK) {
      ALOGE("[%s] %s: Adding an output buffer failed: %s (%d)", name_.c_str(),
            __FUNCTION__, strerror(-res), res);
      failed = true;
    }
  }

  for (auto& buffer : result->input_buffers) {
    res = AddBuffer(frame_number, buffer, /*is_input=*/true);
    if (res != OK) {
      ALOGE("[%s] %s: Adding an input buffer failed: %s (%d)", name_.c_str(),
            __FUNCTION__, strerror(-res), res);
      failed = true;
    }
  }

  return failed ? UNKNOWN_ERROR : OK;
}

status_t ResultDispatcher::AddResult(std::unique_ptr<CaptureResult> result) {
  ATRACE_CALL();
  const status_t res = AddResultImpl(std::move(result));
  {
    std::unique_lock<std::mutex> lock(notify_callback_lock_);
    is_result_shutter_updated_ = true;
    notify_callback_condition_.notify_one();
  }
  return res;
}

status_t ResultDispatcher::AddBatchResult(
    std::vector<std::unique_ptr<CaptureResult>> results) {
  std::optional<status_t> last_error;
  for (auto& result : results) {
    const status_t res = AddResultImpl(std::move(result));
    if (res != OK) {
      last_error = res;
    }
  }
  {
    std::unique_lock<std::mutex> lock(notify_callback_lock_);
    is_result_shutter_updated_ = true;
    notify_callback_condition_.notify_one();
  }
  return last_error.value_or(OK);
}

status_t ResultDispatcher::AddShutter(uint32_t frame_number,
                                      int64_t timestamp_ns,
                                      int64_t readout_timestamp_ns) {
  ATRACE_CALL();

  {
    std::lock_guard<std::mutex> lock(result_lock_);
    status_t res = pending_shutters_.AddResult(
        frame_number, PendingShutter{
                          .timestamp_ns = timestamp_ns,
                          .readout_timestamp_ns = readout_timestamp_ns,
                          .ready = true,
                      });
    if (res != OK) {
      ALOGE(
          "[%s] %s: Failed to add shutter for frame %u , New timestamp "
          "%" PRId64,
          name_.c_str(), __FUNCTION__, frame_number, timestamp_ns);
      return res;
    }
  }
  {
    std::unique_lock<std::mutex> lock(notify_callback_lock_);
    is_result_shutter_updated_ = true;
    notify_callback_condition_.notify_one();
  }
  return OK;
}

status_t ResultDispatcher::AddError(const ErrorMessage& error) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_lock_);
  uint32_t frame_number = error.frame_number;
  // No need to deliver the shutter message on an error
  if (error.error_code == ErrorCode::kErrorDevice ||
      error.error_code == ErrorCode::kErrorResult ||
      error.error_code == ErrorCode::kErrorRequest) {
    pending_shutters_.RemoveRequest(frame_number);
  }
  // No need to deliver the result metadata on a result metadata error
  if (error.error_code == ErrorCode::kErrorResult ||
      error.error_code == ErrorCode::kErrorRequest) {
    pending_early_metadata_.RemoveRequest(frame_number);
    pending_final_metadata_.RemoveRequest(frame_number);
  }

  NotifyMessage message = {.type = MessageType::kError, .message.error = error};
  ALOGV("[%s] %s: Notify error %u for frame %u stream %d", name_.c_str(),
        __FUNCTION__, error.error_code, frame_number, error.error_stream_id);
  notify_(message);

  return OK;
}

std::unique_ptr<CaptureResult> ResultDispatcher::MakeResultMetadata(
    uint32_t frame_number, std::unique_ptr<HalCameraMetadata> metadata,
    std::vector<PhysicalCameraMetadata> physical_metadata,
    uint32_t partial_result) {
  ATRACE_CALL();
  auto result = std::make_unique<CaptureResult>(CaptureResult({}));
  result->frame_number = frame_number;
  result->result_metadata = std::move(metadata);
  result->physical_metadata = std::move(physical_metadata);
  result->partial_result = partial_result;
  return result;
}

status_t ResultDispatcher::AddResultMetadata(
    uint32_t frame_number, std::unique_ptr<HalCameraMetadata> metadata,
    std::vector<PhysicalCameraMetadata> physical_metadata,
    uint32_t partial_result) {
  ATRACE_CALL();
  if (metadata == nullptr) {
    ALOGE("[%s] %s: metadata is nullptr.", name_.c_str(), __FUNCTION__);
    return BAD_VALUE;
  }

  if (partial_result > kPartialResultCount) {
    ALOGE(
        "[%s] %s: partial_result %u cannot be larger than partial result count "
        "%u",
        name_.c_str(), __FUNCTION__, partial_result, kPartialResultCount);
    return BAD_VALUE;
  }

  std::lock_guard<std::mutex> lock(result_lock_);
  DispatchQueue<PendingResultMetadata>& queue =
      partial_result < kPartialResultCount ? pending_early_metadata_
                                           : pending_final_metadata_;
  return queue.AddResult(frame_number,
                         PendingResultMetadata{
                             .metadata = std::move(metadata),
                             .physical_metadata = std::move(physical_metadata),
                             .partial_result_count = partial_result,
                             .ready = true,
                         });
}

status_t ResultDispatcher::AddBuffer(uint32_t frame_number, StreamBuffer buffer,
                                     bool is_input) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_lock_);

  StreamKey stream_key = CreateStreamKey(buffer.stream_id);
  auto pending_buffers_it = stream_pending_buffers_map_.find(stream_key);
  if (pending_buffers_it == stream_pending_buffers_map_.end()) {
    ALOGE("[%s] %s: Cannot find the pending buffer for stream %s",
          name_.c_str(), __FUNCTION__, DumpStreamKey(stream_key).c_str());
    return NAME_NOT_FOUND;
  }

  return pending_buffers_it->second.AddResult(frame_number,
                                              PendingBuffer{
                                                  .buffer = buffer,
                                                  .is_input = is_input,
                                                  .ready = true,
                                              });
}

void ResultDispatcher::NotifyCallbackThreadLoop() {
  // '\0' counts toward the 16-character restriction.
  constexpr int kPthreadNameLenMinusOne = 16 - 1;
  pthread_setname_np(
      pthread_self(),
      name_.substr(/*pos=*/0, /*count=*/kPthreadNameLenMinusOne).c_str());

  while (1) {
    NotifyShutters();
    NotifyResultMetadata();
    NotifyBuffers();

    std::unique_lock<std::mutex> lock(notify_callback_lock_);
    if (notify_callback_thread_exiting_) {
      ALOGV("[%s] %s: NotifyCallbackThreadLoop exits.", name_.c_str(),
            __FUNCTION__);
      return;
    }
    if (!is_result_shutter_updated_) {
      if (notify_callback_condition_.wait_for(
              lock, std::chrono::milliseconds(kCallbackThreadTimeoutMs)) ==
          std::cv_status::timeout) {
        PrintTimeoutMessages();
      }
    }
    is_result_shutter_updated_ = false;
  }
}

void ResultDispatcher::PrintTimeoutMessages() {
  std::lock_guard<std::mutex> lock(result_lock_);
  pending_shutters_.PrintTimeoutMessages();
  pending_early_metadata_.PrintTimeoutMessages();
  pending_final_metadata_.PrintTimeoutMessages();

  for (auto& [stream_key, pending_buffers] : stream_pending_buffers_map_) {
    pending_buffers.PrintTimeoutMessages();
  }
}

void ResultDispatcher::InitializeGroupStreamIdsMap(
    const StreamConfiguration& stream_config) {
  std::lock_guard<std::mutex> lock(result_lock_);
  for (const auto& stream : stream_config.streams) {
    if (stream.group_id != -1) {
      group_stream_map_[stream.id] = stream.group_id;
    }
  }
}

ResultDispatcher::StreamKey ResultDispatcher::CreateStreamKey(
    int32_t stream_id) const {
  if (group_stream_map_.count(stream_id) == 0) {
    return StreamKey(stream_id, StreamKeyType::kSingleStream);
  } else {
    return StreamKey(group_stream_map_.at(stream_id),
                     StreamKeyType::kGroupStream);
  }
}

std::string ResultDispatcher::DumpStreamKey(const StreamKey& stream_key) const {
  switch (stream_key.second) {
    case StreamKeyType::kSingleStream:
      return std::to_string(stream_key.first);
    case StreamKeyType::kGroupStream:
      return "group " + std::to_string(stream_key.first);
    default:
      return "Invalid stream key type";
  }
}

void ResultDispatcher::NotifyShutters() {
  ATRACE_CALL();
  NotifyMessage message = {};
  // TODO: b/347771898 - Update to not depend on running faster than data is
  // ready
  while (true) {
    uint32_t frame_number = 0;
    PendingShutter pending_shutter;
    std::lock_guard<std::mutex> lock(result_lock_);
    if (pending_shutters_.GetReadyData(frame_number, pending_shutter) != OK) {
      break;
    }
    message.type = MessageType::kShutter;
    message.message.shutter.frame_number = frame_number;
    message.message.shutter.timestamp_ns = pending_shutter.timestamp_ns;
    message.message.shutter.readout_timestamp_ns =
        pending_shutter.readout_timestamp_ns;
    ALOGV("[%s] %s: Notify shutter for frame %u timestamp %" PRIu64
          " readout_timestamp %" PRIu64,
          name_.c_str(), __FUNCTION__, message.message.shutter.frame_number,
          message.message.shutter.timestamp_ns,
          message.message.shutter.readout_timestamp_ns);
    notify_(message);
  }
}

void ResultDispatcher::NotifyCaptureResults(
    std::vector<std::unique_ptr<CaptureResult>> results) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(process_capture_result_lock_);
  if (process_batch_capture_result_ != nullptr) {
    process_batch_capture_result_(std::move(results));
  } else {
    for (auto& result : results) {
      process_capture_result_(std::move(result));
    }
  }
}

void ResultDispatcher::NotifyResultMetadata() {
  ATRACE_CALL();
  uint32_t frame_number = 0;
  std::vector<std::unique_ptr<CaptureResult>> early_results;
  std::vector<std::unique_ptr<CaptureResult>> final_results;
  PendingResultMetadata early_result_metadata;
  PendingResultMetadata final_result_metadata;
  // TODO: b/347771898 - Assess if notify can hold the lock for less time
  {
    std::lock_guard<std::mutex> lock(result_lock_);
    while (pending_early_metadata_.GetReadyData(frame_number,
                                                early_result_metadata) == OK) {
      ALOGV("[%s] %s: Notify early metadata for frame %u", name_.c_str(),
            __FUNCTION__, frame_number);
      early_results.push_back(MakeResultMetadata(
          frame_number, std::move(early_result_metadata.metadata),
          std::move(early_result_metadata.physical_metadata),
          early_result_metadata.partial_result_count));
    }

    while (pending_final_metadata_.GetReadyData(frame_number,
                                                final_result_metadata) == OK) {
      ALOGV("[%s] %s: Notify final metadata for frame %u", name_.c_str(),
            __FUNCTION__, frame_number);
      // Removes the pending early metadata if it exists, in case the HAL only
      // sent the final metadata
      pending_early_metadata_.RemoveRequest(frame_number);

      final_results.push_back(MakeResultMetadata(
          frame_number, std::move(final_result_metadata.metadata),
          std::move(final_result_metadata.physical_metadata),
          final_result_metadata.partial_result_count));
    }
  }
  if (!early_results.empty()) {
    NotifyCaptureResults(std::move(early_results));
  }
  if (!final_results.empty()) {
    NotifyCaptureResults(std::move(final_results));
  }
}

status_t ResultDispatcher::GetReadyBufferResult(
    std::unique_ptr<CaptureResult>* result) {
  ATRACE_CALL();
  std::lock_guard<std::mutex> lock(result_lock_);
  if (result == nullptr) {
    ALOGE("[%s] %s: result is nullptr.", name_.c_str(), __FUNCTION__);
    return BAD_VALUE;
  }

  *result = nullptr;

  for (auto& pending_buffers : stream_pending_buffers_map_) {
    uint32_t frame_number = 0;
    PendingBuffer buffer_data;
    if (pending_buffers.second.GetReadyData(frame_number, buffer_data) == OK) {
      std::unique_ptr<CaptureResult> buffer_result =
          std::make_unique<CaptureResult>(CaptureResult({}));

      buffer_result->frame_number = frame_number;
      if (buffer_data.is_input) {
        buffer_result->input_buffers.push_back(buffer_data.buffer);
      } else {
        buffer_result->output_buffers.push_back(buffer_data.buffer);
      }
      *result = std::move(buffer_result);
      return OK;
    }
  }

  return NAME_NOT_FOUND;
}

void ResultDispatcher::NotifyBuffers() {
  ATRACE_CALL();
  std::vector<std::unique_ptr<CaptureResult>> results;
  std::unique_ptr<CaptureResult> result;

  // TODO: b/347771898 - Update to not depend on running faster than data is
  // ready
  while (GetReadyBufferResult(&result) == OK) {
    if (result == nullptr) {
      ALOGE("[%s] %s: result is nullptr", name_.c_str(), __FUNCTION__);
      return;
    }
    ALOGV("[%s] %s: Notify Buffer for frame %u", name_.c_str(), __FUNCTION__,
          result->frame_number);
    results.push_back(std::move(result));
  }
  if (!results.empty()) {
    NotifyCaptureResults(std::move(results));
  }
}

template <typename FrameData>
ResultDispatcher::DispatchQueue<FrameData>::DispatchQueue(
    std::string_view dispatcher_name, std::string_view data_name)
    : dispatcher_name_(dispatcher_name), data_name_(data_name) {
}

template <typename FrameData>
status_t ResultDispatcher::DispatchQueue<FrameData>::AddRequest(
    uint32_t frame_number, RequestType request_type) {
  if (normal_request_map_.contains(frame_number) ||
      reprocess_request_map_.contains(frame_number)) {
    ALOGE("[%s] %s: Pending %s for frame %u already exists.",
          std::string(dispatcher_name_).c_str(), __FUNCTION__,
          data_name_.c_str(), frame_number);
    return ALREADY_EXISTS;
  }
  if (request_type == RequestType::kNormal) {
    normal_request_map_[frame_number] = FrameData();
  } else {
    reprocess_request_map_[frame_number] = FrameData();
  }
  return OK;
}

template <typename FrameData>
void ResultDispatcher::DispatchQueue<FrameData>::RemoveRequest(
    uint32_t frame_number) {
  normal_request_map_.erase(frame_number);
  reprocess_request_map_.erase(frame_number);
}

template <typename FrameData>
status_t ResultDispatcher::DispatchQueue<FrameData>::AddResult(
    uint32_t frame_number, FrameData result) {
  auto it = normal_request_map_.find(frame_number);
  if (it == normal_request_map_.end()) {
    it = reprocess_request_map_.find(frame_number);
    if (it == reprocess_request_map_.end()) {
      ALOGE("[%s] %s: Cannot find the pending %s for frame %u",
            std::string(dispatcher_name_).c_str(), __FUNCTION__,
            data_name_.c_str(), frame_number);
      return NAME_NOT_FOUND;
    }
  }

  if (it->second.ready) {
    ALOGE("[%s] %s: Already received %s for frame %u",
          std::string(dispatcher_name_).c_str(), __FUNCTION__,
          data_name_.c_str(), frame_number);
    return ALREADY_EXISTS;
  }

  it->second = std::move(result);
  return OK;
}

template <typename FrameData>
status_t ResultDispatcher::DispatchQueue<FrameData>::GetReadyData(
    uint32_t& frame_number, FrameData& ready_data) {
  auto it = normal_request_map_.begin();
  if (it != normal_request_map_.end() && it->second.ready) {
    frame_number = it->first;
    ready_data = std::move(it->second);
    normal_request_map_.erase(it);
    return OK;
  }

  it = reprocess_request_map_.begin();
  if (it != reprocess_request_map_.end() && it->second.ready) {
    frame_number = it->first;
    ready_data = std::move(it->second);
    reprocess_request_map_.erase(it);
    return OK;
  }
  // The first pending data is not ready
  return NAME_NOT_FOUND;
}

template <typename FrameData>
void ResultDispatcher::DispatchQueue<FrameData>::PrintTimeoutMessages() {
  for (auto& [frame_number, pending_data] : normal_request_map_) {
    ALOGW("[%s] %s: pending %s for frame %u ready %d",
          std::string(dispatcher_name_).c_str(), __FUNCTION__,
          data_name_.c_str(), frame_number, pending_data.ready);
  }
  for (auto& [frame_number, pending_data] : reprocess_request_map_) {
    ALOGW("[%s] %s: pending %s for frame %u ready %d",
          std::string(dispatcher_name_).c_str(), __FUNCTION__,
          data_name_.c_str(), frame_number, pending_data.ready);
  }
}
template class ResultDispatcher::DispatchQueue<ResultDispatcher::PendingShutter>;
template class ResultDispatcher::DispatchQueue<ResultDispatcher::PendingBuffer>;
template class ResultDispatcher::DispatchQueue<
    ResultDispatcher::PendingResultMetadata>;

}  // namespace google_camera_hal
}  // namespace android

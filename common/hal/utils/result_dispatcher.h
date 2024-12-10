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

#ifndef HARDWARE_GOOGLE_CAMERA_HAL_UTILS_RESULT_DISPATCHER_H_
#define HARDWARE_GOOGLE_CAMERA_HAL_UTILS_RESULT_DISPATCHER_H_

#include <android-base/thread_annotations.h>

#include <map>
#include <string>
#include <string_view>
#include <thread>

#include "hal_types.h"

namespace android {
namespace google_camera_hal {

// ResultDispatcher dispatches capture results in the order of frame numbers,
// including result metadata, shutters, and stream buffers.
//
// The client can add results and shutters via AddResult() (or AddBatchResult())
// and AddShutter() in any order. ResultDispatcher will invoke
// ProcessCaptureResultFunc (or ProcessBatchCaptureResultFunc) and NotifyFunc to
// notify result metadata, shutters, and stream buffers in the in the order of
// increasing frame numbers.
class ResultDispatcher {
 public:
  // Create a ResultDispatcher.
  // partial_result_count is the partial result count.
  // process_capture_result is the callback to notify a capture result.
  // process_batch_capture_result is the callback to notify multiple capture
  // results at once.
  // stream_config is the session stream configuration.
  // notify is the function to notify shutter messages.
  // If process_batch_capture_result is not null, it has the priority over
  // process_capture_result.
  static std::unique_ptr<ResultDispatcher> Create(
      uint32_t partial_result_count,
      ProcessCaptureResultFunc process_capture_result,
      ProcessBatchCaptureResultFunc process_batch_capture_result,
      NotifyFunc notify, const StreamConfiguration& stream_config,
      std::string_view name = "ResultDispatcher");

  virtual ~ResultDispatcher();

  // Add a pending request. This tells ResultDispatcher to watch for
  // the shutter, result metadata, and stream buffers for this request,
  // that will be added later via AddResult() and AddShutter().
  status_t AddPendingRequest(const CaptureRequest& pending_request)
      EXCLUDES(result_lock_);

  // Add a ready result. If the result doesn't belong to a pending request that
  // was previously added via AddPendingRequest(), an error will be returned.
  status_t AddResult(std::unique_ptr<CaptureResult> result);

  // Add a batch of results which contains multiple ready results.
  status_t AddBatchResult(std::vector<std::unique_ptr<CaptureResult>> results);

  // Add a shutter for a frame number. If the frame number doesn't belong to a
  // pending request that was previously added via AddPendingRequest(), an error
  // will be returned.
  status_t AddShutter(uint32_t frame_number, int64_t timestamp_ns,
                      int64_t readout_timestamp_ns) EXCLUDES(result_lock_);

  // Add an error notification for a frame number. When this is called, we no
  // longer wait for a shutter message or result metadata for the given frame.
  status_t AddError(const ErrorMessage& error) EXCLUDES(result_lock_);

  // Remove a pending request.
  void RemovePendingRequest(uint32_t frame_number) EXCLUDES(result_lock_);

  ResultDispatcher(uint32_t partial_result_count,
                   ProcessCaptureResultFunc process_capture_result,
                   ProcessBatchCaptureResultFunc process_batch_capture_result,
                   NotifyFunc notify, const StreamConfiguration& stream_config,
                   std::string_view name = "ResultDispatcher");

 private:
  static constexpr uint32_t kCallbackThreadTimeoutMs = 500;
  const uint32_t kPartialResultCount;

  // Define the request types. Normal is for general application.
  // Reprocess is for reprocessing requests.
  enum class RequestType : uint32_t {
    kNormal = 0,
    kReprocess,
  };

  // Define the stream key types. Single stream type is for normal streams.
  // Group stream type is for the group streams of multi-resolution streams.
  enum class StreamKeyType : uint32_t {
    kSingleStream = 0,
    kGroupStream,
  };

  // The key of the stream_pending_buffers_map_, which has different types.
  // Type kSingleStream indicates the StreamKey represents a single stream, and
  // the id will be the stream id.
  // Type kGroupStream indicates the StreamKey represents a stream group, and
  // the id will be the stream group id. All of the buffers of certain stream
  // group will be tracked together, as there's only one buffer from the group
  // streams should be returned each request.
  typedef std::pair</*id=*/int32_t, StreamKeyType> StreamKey;

  // Define a pending shutter that will be ready later when AddShutter() is
  // called.
  struct PendingShutter {
    int64_t timestamp_ns = 0;
    int64_t readout_timestamp_ns = 0;
    bool ready = false;
  };

  // Define a pending buffer that will be ready later when AddResult() is
  // called.
  struct PendingBuffer {
    StreamBuffer buffer = {};
    bool is_input = false;
    bool ready = false;
  };

  // Define a pending result metadata that will be ready later when AddResult()
  // is called.
  struct PendingResultMetadata {
    std::unique_ptr<HalCameraMetadata> metadata;
    std::vector<PhysicalCameraMetadata> physical_metadata;
    uint32_t partial_result_count = 0;
    bool ready = false;
  };

  // Template class for pending data queues.
  // Pending data can be shutter, early/final result metadata, buffer, and each
  // type of data has its own queue. Handles having multiple queues per request
  // type, adds to the appropriate queue and checks all queues for ready data.
  template <typename FrameData>
  class DispatchQueue {
   public:
    DispatchQueue(std::string_view dispatcher_name = "DefaultDispatcher",
                  std::string_view data_name = "DefaultData");

    // Add a request to the dispatch queue that will later be populated with
    // results.
    status_t AddRequest(uint32_t frame_number, RequestType request_type);

    // Remove request for frame number from data queue
    void RemoveRequest(uint32_t frame_number);

    // Add results for the request in the queue of the same frame number
    status_t AddResult(uint32_t frame_number, FrameData result);

    // Move ready data to caller, returns failure status if no data is ready
    // Data is ready if its result has been added and is the first in its queue
    status_t GetReadyData(uint32_t& frame_number, FrameData& ready_data);

    void PrintTimeoutMessages();

   private:
    // Name of the dispatcher for debug messages
    std::string_view dispatcher_name_;
    // Name of the data (shutter, metadata, buffer + stream key) for debug
    // messages
    std::string data_name_;

    // Queue for data of reprocess request types
    std::map<uint32_t, FrameData> reprocess_request_map_;
    // Queue for data of normal request types
    std::map<uint32_t, FrameData> normal_request_map_;
  };

  // Add a pending shutter, result metadata, and buffers for a frame number.
  status_t AddPendingRequestLocked(const CaptureRequest& pending_request)
      EXCLUSIVE_LOCKS_REQUIRED(result_lock_);

  // Add a pending buffer for the associated stream
  status_t AddPendingBufferLocked(uint32_t frame_number,
                                  const StreamBuffer& buffer,
                                  RequestType request_type)
      EXCLUSIVE_LOCKS_REQUIRED(result_lock_);

  // Remove pending shutter, result metadata, and buffers for a frame number.
  void RemovePendingRequestLocked(uint32_t frame_number)
      EXCLUSIVE_LOCKS_REQUIRED(result_lock_);

  // Add result metadata and buffers to the storage to send them from the notify
  // callback thread.
  status_t AddResultImpl(std::unique_ptr<CaptureResult> result);

  // Compose a capture result which contains a result metadata.
  std::unique_ptr<CaptureResult> MakeResultMetadata(
      uint32_t frame_number, std::unique_ptr<HalCameraMetadata> metadata,
      std::vector<PhysicalCameraMetadata> physical_metadata,
      uint32_t partial_result);

  // Invoke the capture result callback to notify capture results.
  void NotifyCaptureResults(std::vector<std::unique_ptr<CaptureResult>> results);

  status_t AddResultMetadata(
      uint32_t frame_number, std::unique_ptr<HalCameraMetadata> metadata,
      std::vector<PhysicalCameraMetadata> physical_metadata,
      uint32_t partial_result) EXCLUDES(result_lock_);
  ;

  status_t AddBuffer(uint32_t frame_number, StreamBuffer buffer, bool is_input)
      EXCLUDES(result_lock_);

  // Check all pending shutters and invoke notify_ with shutters that are ready.
  void NotifyShutters() EXCLUDES(result_lock_);

  // Check all pending result metadata and invoke the capture result callback
  // with the result metadata that are ready.
  void NotifyResultMetadata() EXCLUDES(result_lock_);

  // Get a result with a buffer that is ready to be notified via the capture
  // result callback.
  status_t GetReadyBufferResult(std::unique_ptr<CaptureResult>* result)
      EXCLUDES(result_lock_);

  // Check all pending buffers and invoke notify_ with buffers that are ready.
  void NotifyBuffers();

  // Thread loop to check pending shutters, result metadata, and buffers. It
  // notifies the client when one is ready.
  void NotifyCallbackThreadLoop();

  void PrintTimeoutMessages() EXCLUDES(result_lock_);

  // Initialize the group stream ids map if needed. Must be protected with
  // result_lock_.
  void InitializeGroupStreamIdsMap(const StreamConfiguration& stream_config)
      EXCLUDES(result_lock_);

  // Name used for debugging purpose to disambiguate multiple ResultDispatchers.
  std::string name_;

  std::mutex result_lock_;

  // Queue for shutter data.
  DispatchQueue<PendingShutter> pending_shutters_ GUARDED_BY(result_lock_);
  // Queue for early result metadata.
  DispatchQueue<PendingResultMetadata> pending_early_metadata_
      GUARDED_BY(result_lock_);
  // Queue for final result metadata.
  DispatchQueue<PendingResultMetadata> pending_final_metadata_
      GUARDED_BY(result_lock_);

  // Maps from a stream or stream group to a queue for buffer data.
  // Protected by result_lock_.
  // For single streams, pending buffers would be tracked by streams.
  // For multi-resolution streams, camera HAL can return only one stream buffer
  // within the same stream group each request. So all of the buffers of certain
  // stream group will be tracked together via a single map.
  // TODO: b/347771069 - Update to use unordered_map
  std::map<StreamKey, DispatchQueue<PendingBuffer>> stream_pending_buffers_map_
      GUARDED_BY(result_lock_);

  // Create a StreamKey for a stream
  inline StreamKey CreateStreamKey(int32_t stream_id) const;

  // Dump a StreamKey to a debug string
  inline std::string DumpStreamKey(const StreamKey& stream_key) const;

  std::mutex process_capture_result_lock_;
  ProcessCaptureResultFunc process_capture_result_;
  ProcessBatchCaptureResultFunc process_batch_capture_result_;
  NotifyFunc notify_;

  // A thread to run NotifyCallbackThreadLoop().
  std::thread notify_callback_thread_;

  std::mutex notify_callback_lock_;

  // Condition to wake up notify_callback_thread_. Used with
  // notify_callback_lock.
  std::condition_variable notify_callback_condition_;

  // Protected by notify_callback_lock.
  bool notify_callback_thread_exiting_ = false;

  // State of callback thread is notified or not.
  volatile bool is_result_shutter_updated_ = false;

  // A map of group streams only, from stream ID to the group ID it belongs.
  std::map</*stream id=*/int32_t, /*group id=*/int32_t> group_stream_map_;
};

}  // namespace google_camera_hal
}  // namespace android

#endif  // HARDWARE_GOOGLE_CAMERA_HAL_UTILS_RESULT_DISPATCHER_H_

// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ProducerEventProcessor.h"

#include <absl/container/flat_hash_map.h>

#include "OrbitBase/Logging.h"
#include "capture.pb.h"

namespace orbit_service {

namespace {

using orbit_grpc_protos::AddressInfo;
using orbit_grpc_protos::Callstack;
using orbit_grpc_protos::ClientCaptureEvent;
using orbit_grpc_protos::FullCallstackSample;
using orbit_grpc_protos::FunctionCall;
using orbit_grpc_protos::GpuDebugMarker;
using orbit_grpc_protos::GpuJob;
using orbit_grpc_protos::GpuQueueSubmission;
using orbit_grpc_protos::InternedCallstack;
using orbit_grpc_protos::InternedCallstackSample;
using orbit_grpc_protos::InternedString;
using orbit_grpc_protos::InternedTracepointInfo;
using orbit_grpc_protos::IntrospectionScope;
using orbit_grpc_protos::ModuleUpdateEvent;
using orbit_grpc_protos::ProducerCaptureEvent;
using orbit_grpc_protos::SchedulingSlice;
using orbit_grpc_protos::ThreadName;
using orbit_grpc_protos::ThreadStateSlice;
using orbit_grpc_protos::TracepointEvent;

template <typename T>
class InternedCache final {
 public:
  explicit InternedCache(std::atomic<uint64_t>* id_counter) : id_counter_{id_counter} {}

  // Return pair of <id, assigned>, where assigned is true if the entry was assigned a new id
  // and false if returning id for already existing entry.
  std::pair<uint64_t, bool> GetOrAssignId(const T& entry) {
    absl::MutexLock lock(&entry_to_id_mutex_);
    auto it = entry_to_id_.find(entry);
    if (it != entry_to_id_.end()) {
      return std::make_pair(it->second, false);
    }

    uint64_t new_id = (*id_counter_)++;
    auto [unused_it, inserted] = entry_to_id_.insert_or_assign(entry, new_id);
    CHECK(inserted);
    return std::make_pair(new_id, true);
  }

 private:
  std::atomic<uint64_t>* id_counter_ = nullptr;
  absl::flat_hash_map<T, uint64_t> entry_to_id_;
  absl::Mutex entry_to_id_mutex_;
};

class ProducerEventProcessorImpl : public ProducerEventProcessor {
 public:
  explicit ProducerEventProcessorImpl(CaptureEventBuffer* capture_event_buffer)
      : capture_event_buffer_{capture_event_buffer},
        callstack_id_counter_{1},
        string_id_counter_{1},
        tracepoint_id_counter_{1},
        callstack_cache_{&callstack_id_counter_},
        string_cache_{&string_id_counter_},
        tracepoint_cache_{&tracepoint_id_counter_},
        interned_callstack_id_cache_{&callstack_id_counter_},
        interned_string_id_cache_{&string_id_counter_},
        interned_tracepoint_id_cache_{&tracepoint_id_counter_} {}

  void ProcessEvent(uint64_t producer_id, ProducerCaptureEvent event) override;

 private:
  void ProcessAddressInfo(uint64_t producer_id, AddressInfo* address_info);
  void ProcessFullCallstackSample(FullCallstackSample* callstack_sample);
  void ProcessFunctionCall(FunctionCall* function_call);
  void ProcessGpuJob(uint64_t producer, GpuJob* gpu_job);
  void ProcessGpuQueueSubmission(uint64_t producer_id, GpuQueueSubmission* gpu_queue_submission);
  void ProcessInternedCallstack(uint64_t producer_id, InternedCallstack* interned_callstack);
  void ProcessInternedCallstackSample(uint64_t producer_id,
                                      InternedCallstackSample* callstack_sample);
  void ProcessInternedString(uint64_t producer_id, InternedString* interned_string);
  void ProcessInternedTracepointInfo(uint64_t producer_id,
                                     InternedTracepointInfo* interned_tracepoint_info);
  void ProcessIntrospectionScope(IntrospectionScope* introspection_scope);
  void ProcessModuleUpdateEvent(ModuleUpdateEvent* module_update_event);
  void ProcessSchedulingSlice(SchedulingSlice* scheduling_slice);
  void ProcessThreadName(ThreadName* thread_name);
  void ProcessThreadStateSlice(ThreadStateSlice* thread_state_slice);
  void ProcessTracepointEvent(uint64_t producer_id, TracepointEvent* tracepoint_event);

  void SendStringAndKeyEvent(uint64_t key, std::string value);

  CaptureEventBuffer* capture_event_buffer_;

  std::atomic<uint64_t> callstack_id_counter_;
  std::atomic<uint64_t> string_id_counter_;
  std::atomic<uint64_t> tracepoint_id_counter_;

  InternedCache<std::vector<uint64_t>> callstack_cache_;
  InternedCache<std::string> string_cache_;
  InternedCache<std::pair<std::string, std::string>> tracepoint_cache_;

  // These are mapping InternStrings and Callstacks from producer ids
  // to client ids:
  // <producer_id, producer_string_id> -> client_string_id and
  // <producer_id, producer_callstack_id> -> client_callstack_id
  InternedCache<std::pair<uint64_t, uint64_t>> interned_callstack_id_cache_;
  InternedCache<std::pair<uint64_t, uint64_t>> interned_string_id_cache_;
  InternedCache<std::pair<uint64_t, uint64_t>> interned_tracepoint_id_cache_;
};

void ProducerEventProcessorImpl::ProcessAddressInfo(uint64_t producer_id,
                                                    AddressInfo* address_info) {
  // TODO: Split address info into 2 separate messages, one with strings another one with keys.
  if (address_info->function_name_or_key_case() == AddressInfo::kFunctionName) {
    auto [function_name_key, assigned] = string_cache_.GetOrAssignId(address_info->function_name());
    if (assigned) {
      SendStringAndKeyEvent(function_name_key, address_info->function_name());
    }
    address_info->set_function_name_key(function_name_key);
  } else {
    CHECK(address_info->function_name_or_key_case() == AddressInfo::kFunctionNameKey);
    auto [function_name_key, assigned] =
        interned_string_id_cache_.GetOrAssignId({producer_id, address_info->function_name_key()});
    CHECK(!assigned);
    address_info->set_function_name_key(function_name_key);
  }

  if (address_info->map_name_or_key_case() == AddressInfo::kMapName) {
    auto [module_name_key, assigned] = string_cache_.GetOrAssignId(address_info->map_name());
    if (assigned) {
      SendStringAndKeyEvent(module_name_key, address_info->map_name());
    }
    address_info->set_map_name_key(module_name_key);
  } else {
    CHECK(address_info->map_name_or_key_case() == AddressInfo::kMapNameKey);
    auto [module_name_key, assigned] =
        interned_string_id_cache_.GetOrAssignId({producer_id, address_info->map_name_key()});
    CHECK(!assigned);
    address_info->set_map_name_key(module_name_key);
  }

  ClientCaptureEvent event;
  *event.mutable_address_info() = std::move(*address_info);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessFunctionCall(FunctionCall* function_call) {
  ClientCaptureEvent event;
  *event.mutable_function_call() = std::move(*function_call);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessGpuJob(uint64_t producer_id, GpuJob* gpu_job) {
  // TODO: Split GpuJob into 2 separate messages, one with string and another one with the key
  if (gpu_job->timeline_or_key_case() == GpuJob::kTimeline) {
    auto [timeline_key, assigned] = string_cache_.GetOrAssignId(gpu_job->timeline());
    if (assigned) {
      SendStringAndKeyEvent(timeline_key, gpu_job->timeline());
    }
    gpu_job->set_timeline_key(timeline_key);
  } else {  // gpu_job->timeline_or_key_case() == GpuJob::kTimelineKey
    CHECK(gpu_job->timeline_or_key_case() == GpuJob::kTimelineKey);
    auto [timeline_key, assigned] =
        interned_string_id_cache_.GetOrAssignId({producer_id, gpu_job->timeline_key()});
    CHECK(!assigned);
    gpu_job->set_timeline_key(timeline_key);
  }

  ClientCaptureEvent event;
  *event.mutable_gpu_job() = std::move(*gpu_job);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessGpuQueueSubmission(
    uint64_t producer_id, GpuQueueSubmission* gpu_queue_submission) {
  // Translate debug marker keys
  for (GpuDebugMarker& mutable_marker : *gpu_queue_submission->mutable_completed_markers()) {
    auto [client_text_key, assigned] =
        interned_string_id_cache_.GetOrAssignId({producer_id, mutable_marker.text_key()});
    CHECK(!assigned);
    mutable_marker.set_text_key(client_text_key);
  }

  ClientCaptureEvent event;
  *event.mutable_gpu_queue_submission() = std::move(*gpu_queue_submission);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessFullCallstackSample(FullCallstackSample* callstack_sample) {
  Callstack* callstack = callstack_sample->mutable_callstack();
  auto [callstack_id, assigned] =
      callstack_cache_.GetOrAssignId({callstack->pcs().begin(), callstack->pcs().end()});

  if (assigned) {
    ClientCaptureEvent interned_callstack_event;
    interned_callstack_event.mutable_interned_callstack()->set_key(callstack_id);
    *interned_callstack_event.mutable_interned_callstack()->mutable_intern() =
        std::move(*callstack);
    capture_event_buffer_->AddEvent(std::move(interned_callstack_event));
  }

  ClientCaptureEvent interned_callstack_sample_event;
  InternedCallstackSample* interned_callstack_sample =
      interned_callstack_sample_event.mutable_interned_callstack_sample();
  interned_callstack_sample->set_pid(callstack_sample->pid());
  interned_callstack_sample->set_tid(callstack_sample->tid());
  interned_callstack_sample->set_timestamp_ns(callstack_sample->timestamp_ns());
  interned_callstack_sample->set_callstack_id(callstack_id);
  capture_event_buffer_->AddEvent(std::move(interned_callstack_sample_event));
}

void ProducerEventProcessorImpl::ProcessInternedCallstack(uint64_t producer_id,
                                                          InternedCallstack* interned_callstack) {
  auto [interned_callstack_id, assigned] =
      interned_callstack_id_cache_.GetOrAssignId({producer_id, interned_callstack->key()});
  CHECK(assigned);
  interned_callstack->set_key(interned_callstack_id);

  ClientCaptureEvent event;
  *event.mutable_interned_callstack() = std::move(*interned_callstack);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessInternedCallstackSample(
    uint64_t producer_id, InternedCallstackSample* callstack_sample) {
  // traslate ids
  auto [callstack_id, assigned] =
      interned_callstack_id_cache_.GetOrAssignId({producer_id, callstack_sample->callstack_id()});
  CHECK(!assigned);
  callstack_sample->set_callstack_id(callstack_id);

  ClientCaptureEvent event;
  *event.mutable_interned_callstack_sample() = std::move(*callstack_sample);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessInternedString(uint64_t producer_id,
                                                       InternedString* interned_string) {
  auto [interned_string_id, assigned] =
      interned_string_id_cache_.GetOrAssignId({producer_id, interned_string->key()});
  CHECK(assigned);
  interned_string->set_key(interned_string_id);

  ClientCaptureEvent event;
  *event.mutable_interned_string() = std::move(*interned_string);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessInternedTracepointInfo(
    uint64_t producer_id, InternedTracepointInfo* interned_tracepoint_info) {
  auto [interned_tracepoint_id, assigned] =
      interned_tracepoint_id_cache_.GetOrAssignId({producer_id, interned_tracepoint_info->key()});
  CHECK(assigned);
  interned_tracepoint_info->set_key(interned_tracepoint_id);

  ClientCaptureEvent event;
  *event.mutable_interned_tracepoint_info() = std::move(*interned_tracepoint_info);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessIntrospectionScope(
    IntrospectionScope* introspection_scope) {
  ClientCaptureEvent event;
  *event.mutable_introspection_scope() = std::move(*introspection_scope);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessModuleUpdateEvent(
    orbit_grpc_protos::ModuleUpdateEvent* module_update_event) {
  ClientCaptureEvent event;
  *event.mutable_module_update_event() = std::move(*module_update_event);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessSchedulingSlice(SchedulingSlice* scheduling_slice) {
  ClientCaptureEvent event;
  *event.mutable_scheduling_slice() = std::move(*scheduling_slice);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessThreadName(ThreadName* thread_name) {
  ClientCaptureEvent event;
  *event.mutable_thread_name() = std::move(*thread_name);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessThreadStateSlice(ThreadStateSlice* thread_state_slice) {
  ClientCaptureEvent event;
  *event.mutable_thread_state_slice() = std::move(*thread_state_slice);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessTracepointEvent(uint64_t producer_id,
                                                        TracepointEvent* tracepoint_event) {
  // TODO: Split TracepointEvent in two, one with key another one with string
  if (tracepoint_event->tracepoint_info_or_key_case() == TracepointEvent::kTracepointInfo) {
    auto [tracepoint_key, assigned] =
        tracepoint_cache_.GetOrAssignId({tracepoint_event->tracepoint_info().category(),
                                         tracepoint_event->tracepoint_info().name()});
    if (assigned) {
      ClientCaptureEvent event;
      event.mutable_interned_tracepoint_info()->set_key(tracepoint_key);
      *event.mutable_interned_tracepoint_info()->mutable_intern() =
          tracepoint_event->tracepoint_info();
      capture_event_buffer_->AddEvent(std::move(event));
    }
    tracepoint_event->set_tracepoint_info_key(tracepoint_key);
  } else {
    CHECK(tracepoint_event->tracepoint_info_or_key_case() == TracepointEvent::kTracepointInfoKey);
    auto [tracepoint_key, assigned] = interned_tracepoint_id_cache_.GetOrAssignId(
        {producer_id, tracepoint_event->tracepoint_info_key()});
    CHECK(!assigned);
    tracepoint_event->set_tracepoint_info_key(tracepoint_key);
  }

  ClientCaptureEvent event;
  *event.mutable_tracepoint_event() = std::move(*tracepoint_event);
  capture_event_buffer_->AddEvent(std::move(event));
}

void ProducerEventProcessorImpl::ProcessEvent(uint64_t producer_id, ProducerCaptureEvent event) {
  switch (event.event_case()) {
    case ProducerCaptureEvent::kInternedCallstack:
      ProcessInternedCallstack(producer_id, event.mutable_interned_callstack());
      break;
    case ProducerCaptureEvent::kSchedulingSlice:
      ProcessSchedulingSlice(event.mutable_scheduling_slice());
      break;
    case ProducerCaptureEvent::kInternedCallstackSample:
      ProcessInternedCallstackSample(producer_id, event.mutable_interned_callstack_sample());
      break;
    case ProducerCaptureEvent::kFullCallstackSample:
      ProcessFullCallstackSample(event.mutable_full_callstack_sample());
      break;
    case ProducerCaptureEvent::kFunctionCall:
      ProcessFunctionCall(event.mutable_function_call());
      break;
    case ProducerCaptureEvent::kInternedString:
      ProcessInternedString(producer_id, event.mutable_interned_string());
      break;
    case ProducerCaptureEvent::kGpuJob:
      ProcessGpuJob(producer_id, event.mutable_gpu_job());
      break;
    case ProducerCaptureEvent::kGpuQueueSubmission:
      ProcessGpuQueueSubmission(producer_id, event.mutable_gpu_queue_submission());
      break;
    case ProducerCaptureEvent::kThreadName:
      ProcessThreadName(event.mutable_thread_name());
      break;
    case ProducerCaptureEvent::kThreadStateSlice:
      ProcessThreadStateSlice(event.mutable_thread_state_slice());
      break;
    case ProducerCaptureEvent::kAddressInfo:
      ProcessAddressInfo(producer_id, event.mutable_address_info());
      break;
    case ProducerCaptureEvent::kInternedTracepointInfo:
      ProcessInternedTracepointInfo(producer_id, event.mutable_interned_tracepoint_info());
      break;
    case ProducerCaptureEvent::kTracepointEvent:
      ProcessTracepointEvent(producer_id, event.mutable_tracepoint_event());
      break;
    case ProducerCaptureEvent::kIntrospectionScope:
      ProcessIntrospectionScope(event.mutable_introspection_scope());
      break;
    case ProducerCaptureEvent::kModuleUpdateEvent:
      ProcessModuleUpdateEvent(event.mutable_module_update_event());
      break;
    case ProducerCaptureEvent::EVENT_NOT_SET:
      UNREACHABLE();
  }
}

void ProducerEventProcessorImpl::SendStringAndKeyEvent(uint64_t key, std::string value) {
  ClientCaptureEvent event;
  InternedString* interned_string = event.mutable_interned_string();
  interned_string->set_key(key);
  interned_string->set_intern(std::move(value));
  capture_event_buffer_->AddEvent(std::move(event));
}

}  // namespace

std::unique_ptr<ProducerEventProcessor> CreateProducerEventProcessor(
    CaptureEventBuffer* capture_event_buffer) {
  return std::make_unique<ProducerEventProcessorImpl>(capture_event_buffer);
}

}  // namespace orbit_service
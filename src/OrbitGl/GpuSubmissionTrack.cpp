// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "GpuSubmissionTrack.h"

#include <absl/time/time.h>

#include <memory>

#include "App.h"
#include "Batcher.h"
#include "ClientData/TextBox.h"
#include "ClientData/TimerChain.h"
#include "DisplayFormats/DisplayFormats.h"
#include "GlCanvas.h"
#include "GlUtils.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ThreadConstants.h"
#include "TimeGraph.h"
#include "TimeGraphLayout.h"
#include "TriangleToggle.h"
#include "absl/strings/str_format.h"

using orbit_client_protos::TimerInfo;

constexpr const char* kSwQueueString = "sw queue";
constexpr const char* kHwQueueString = "hw queue";
constexpr const char* kHwExecutionString = "hw execution";
constexpr const char* kCmdBufferString = "command buffer";

GpuSubmissionTrack::GpuSubmissionTrack(Track* parent, TimeGraph* time_graph,
                                       orbit_gl::Viewport* viewport, TimeGraphLayout* layout,
                                       uint64_t timeline_hash, OrbitApp* app,
                                       const orbit_client_model::CaptureData* capture_data,
                                       uint32_t indentation_level)
    : TimerTrack(parent, time_graph, viewport, layout, app, capture_data, indentation_level) {
  SetLabel("Submissions");
  draw_background_ = false;
  text_renderer_ = time_graph->GetTextRenderer();
  timeline_hash_ = timeline_hash;
  string_manager_ = app->GetStringManager();
  parent_ = parent;
}

std::string GpuSubmissionTrack::GetTooltip() const {
  return "Shows scheduling and execution times for selected GPU job "
         "submissions";
}

void GpuSubmissionTrack::OnTimer(const orbit_client_protos::TimerInfo& timer_info) {
  // In case of having command buffer timers, we need to double the depth of the GPU timers (as we
  // are drawing the corresponding command buffer timers below them). Therefore, we watch out for
  // those timers.
  if (timer_info.type() == TimerInfo::kGpuCommandBuffer) {
    has_vulkan_layer_command_buffer_timers_ = true;
  }
  TimerTrack::OnTimer(timer_info);
}

bool GpuSubmissionTrack::IsTimerActive(const TimerInfo& timer_info) const {
  bool is_same_tid_as_selected = timer_info.thread_id() == app_->selected_thread_id();
  // We do not properly track the PID for GPU jobs and we still want to show
  // all jobs as active when no thread is selected, so this logic is a bit
  // different than SchedulerTrack::IsTimerActive.
  bool no_thread_selected = app_->selected_thread_id() == orbit_base::kAllProcessThreadsTid;

  return is_same_tid_as_selected || no_thread_selected;
}

Color GpuSubmissionTrack::GetTimerColor(const TimerInfo& timer_info, bool is_selected,
                                        bool is_highlighted) const {
  const Color kInactiveColor(100, 100, 100, 255);
  const Color kSelectionColor(0, 128, 255, 255);
  if (is_highlighted) {
    return TimerTrack::kHighlightColor;
  }
  if (is_selected) {
    return kSelectionColor;
  }
  if (!IsTimerActive(timer_info)) {
    return kInactiveColor;
  }
  if (timer_info.has_color()) {
    CHECK(timer_info.color().red() < 256);
    CHECK(timer_info.color().green() < 256);
    CHECK(timer_info.color().blue() < 256);
    CHECK(timer_info.color().alpha() < 256);
    return Color(static_cast<uint8_t>(timer_info.color().red()),
                 static_cast<uint8_t>(timer_info.color().green()),
                 static_cast<uint8_t>(timer_info.color().blue()),
                 static_cast<uint8_t>(timer_info.color().alpha()));
  }

  // We color code the timeslices for GPU activity using the color
  // of the CPU thread track that submitted the job.
  Color color = TimeGraph::GetThreadColor(timer_info.thread_id());

  // We disambiguate the different types of GPU activity based on the
  // string that is displayed on their timeslice.
  float coeff = 1.0f;
  std::string gpu_stage = string_manager_->Get(timer_info.user_data_key()).value_or("");
  if (gpu_stage == kSwQueueString) {
    coeff = 0.5f;
  } else if (gpu_stage == kHwQueueString) {
    coeff = 0.75f;
  } else if (gpu_stage == kHwExecutionString) {
    coeff = 1.0f;
  }

  color[0] = static_cast<uint8_t>(coeff * color[0]);
  color[1] = static_cast<uint8_t>(coeff * color[1]);
  color[2] = static_cast<uint8_t>(coeff * color[2]);

  constexpr uint8_t kOddAlpha = 210;
  if ((timer_info.depth() & 0x1) == 0u) {
    color[3] = kOddAlpha;
  }

  return color;
}

float GpuSubmissionTrack::GetYFromTimer(const TimerInfo& timer_info) const {
  auto adjusted_depth = static_cast<float>(timer_info.depth());
  if (ShouldShowCollapsed()) {
    adjusted_depth = 0.f;
  }
  CHECK(timer_info.type() == TimerInfo::kGpuActivity ||
        timer_info.type() == TimerInfo::kGpuCommandBuffer);

  // We are drawing a small gap between each depth, for visualization purposes.
  // There won't be a gap between "hw execution"timers and command buffer timers, which
  // is why the gap space needs to be calculated before adjusting the depth further (see below).
  float gap_space = adjusted_depth * layout_->GetSpaceBetweenGpuDepths();

  // Command buffer timers are drawn underneath the matching "hw execution" timer, which has the
  // same depth value as the command buffer timer. Therefore, we need to double the depth in the
  // case that we have command buffer timers.
  if (has_vulkan_layer_command_buffer_timers_) {
    adjusted_depth *= 2.f;
  }

  // Command buffer timers have the same depth value as their matching "hw execution" timer.
  // As we want to draw command buffers underneath the hw execution timers, we need to increase
  // the depth by one.
  if (timer_info.type() == TimerInfo::kGpuCommandBuffer) {
    adjusted_depth += 1.f;
  }
  return pos_[1] - layout_->GetTrackTabHeight() -
         layout_->GetTextBoxHeight() * (adjusted_depth + 1.f) - gap_space;
}

// When track or its parent is collapsed, only draw "hardware execution" timers.
bool GpuSubmissionTrack::TimerFilter(const TimerInfo& timer_info) const {
  if (ShouldShowCollapsed()) {
    std::string gpu_stage = string_manager_->Get(timer_info.user_data_key()).value_or("");
    return gpu_stage == kHwExecutionString;
  }
  return true;
}

void GpuSubmissionTrack::SetTimesliceText(const TimerInfo& timer_info,
                                          orbit_client_data::TextBox* text_box) {
  if (text_box->GetText().empty()) {
    std::string time = orbit_display_formats::GetDisplayTime(
        absl::Nanoseconds(timer_info.end() - timer_info.start()));

    CHECK(timer_info.type() == TimerInfo::kGpuActivity ||
          timer_info.type() == TimerInfo::kGpuCommandBuffer);

    std::string text = absl::StrFormat(
        "%s  %s", string_manager_->Get(timer_info.user_data_key()).value_or(""), time.c_str());
    text_box->SetText(text);
  }
}

float GpuSubmissionTrack::GetHeight() const {
  bool collapsed = ShouldShowCollapsed();
  uint32_t depth = collapsed ? 1 : GetDepth();
  uint32_t num_gaps = depth > 0 ? depth - 1 : 0;
  if (has_vulkan_layer_command_buffer_timers_ && !collapsed) {
    depth *= 2;
  }
  return layout_->GetTrackTabHeight() + layout_->GetTextBoxHeight() * depth +
         (num_gaps * layout_->GetSpaceBetweenGpuDepths()) + layout_->GetTrackBottomMargin();
}

const orbit_client_data::TextBox* GpuSubmissionTrack::GetLeft(
    const orbit_client_data::TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  uint64_t timeline_hash = timer_info.user_data_key();
  if (timeline_hash == timeline_hash_) {
    std::shared_ptr<orbit_client_data::TimerChain> timers = GetTimers(timer_info.depth());
    if (timers) return timers->GetElementBefore(text_box);
  }
  return nullptr;
}

const orbit_client_data::TextBox* GpuSubmissionTrack::GetRight(
    const orbit_client_data::TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  uint64_t timeline_hash = timer_info.user_data_key();
  if (timeline_hash == timeline_hash_) {
    std::shared_ptr<orbit_client_data::TimerChain> timers = GetTimers(timer_info.depth());
    if (timers) return timers->GetElementAfter(text_box);
  }
  return nullptr;
}

std::string GpuSubmissionTrack::GetBoxTooltip(const Batcher& batcher, PickingId id) const {
  const orbit_client_data::TextBox* text_box = batcher.GetTextBox(id);
  if ((text_box == nullptr) || text_box->GetTimerInfo().type() == TimerInfo::kCoreActivity) {
    return "";
  }

  std::string gpu_stage =
      string_manager_->Get(text_box->GetTimerInfo().user_data_key()).value_or("");
  if (gpu_stage == kSwQueueString) {
    return GetSwQueueTooltip(text_box->GetTimerInfo());
  }
  if (gpu_stage == kHwQueueString) {
    return GetHwQueueTooltip(text_box->GetTimerInfo());
  }
  if (gpu_stage == kHwExecutionString) {
    return GetHwExecutionTooltip(text_box->GetTimerInfo());
  }
  if (gpu_stage == kCmdBufferString) {
    return GetCommandBufferTooltip(text_box->GetTimerInfo());
  }

  return "";
}

std::string GpuSubmissionTrack::GetSwQueueTooltip(const TimerInfo& timer_info) const {
  CHECK(capture_data_ != nullptr);
  return absl::StrFormat(
      "<b>Software Queue</b><br/>"
      "<i>Time between amdgpu_cs_ioctl (job submitted) and "
      "amdgpu_sched_run_job (job scheduled)</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from process:</b> %s [%d]<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      capture_data_->GetThreadName(timer_info.process_id()), timer_info.process_id(),
      capture_data_->GetThreadName(timer_info.thread_id()), timer_info.thread_id(),
      orbit_display_formats::GetDisplayTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}

std::string GpuSubmissionTrack::GetHwQueueTooltip(const TimerInfo& timer_info) const {
  CHECK(capture_data_ != nullptr);
  return absl::StrFormat(
      "<b>Hardware Queue</b><br/><i>Time between amdgpu_sched_run_job "
      "(job scheduled) and start of GPU execution</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from process:</b> %s [%d]<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      capture_data_->GetThreadName(timer_info.process_id()), timer_info.process_id(),
      capture_data_->GetThreadName(timer_info.thread_id()), timer_info.thread_id(),
      orbit_display_formats::GetDisplayTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}

std::string GpuSubmissionTrack::GetHwExecutionTooltip(const TimerInfo& timer_info) const {
  CHECK(capture_data_ != nullptr);
  return absl::StrFormat(
      "<b>Harware Execution</b><br/>"
      "<i>End is marked by \"dma_fence_signaled\" event for this command "
      "buffer submission</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from process:</b> %s [%d]<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      capture_data_->GetThreadName(timer_info.process_id()), timer_info.process_id(),
      capture_data_->GetThreadName(timer_info.thread_id()), timer_info.thread_id(),
      orbit_display_formats::GetDisplayTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}

std::string GpuSubmissionTrack::GetCommandBufferTooltip(
    const orbit_client_protos::TimerInfo& timer_info) const {
  return absl::StrFormat(
      "<b>Command Buffer Execution</b><br/>"
      "<i>At `vkBeginCommandBuffer` and `vkEndCommandBuffer` `vkCmdWriteTimestamp`s have been "
      "inserted. The GPU timestamps get aligned with the corresponding hardware execution of the "
      "submission.</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from process:</b> %s [%d]<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      capture_data_->GetThreadName(timer_info.process_id()), timer_info.process_id(),
      capture_data_->GetThreadName(timer_info.thread_id()), timer_info.thread_id(),
      orbit_display_formats::GetDisplayTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}

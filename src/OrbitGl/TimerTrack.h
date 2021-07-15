// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_TIMER_TRACK_H_
#define ORBIT_GL_TIMER_TRACK_H_

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "BlockChain.h"
#include "CallstackThreadBar.h"
#include "CaptureViewElement.h"
#include "ClientData/CallstackTypes.h"
#include "ClientData/TextBox.h"
#include "ClientData/TimerChain.h"
#include "CoreMath.h"
#include "PickingManager.h"
#include "TextRenderer.h"
#include "TracepointThreadBar.h"
#include "Track.h"
#include "Viewport.h"
#include "absl/synchronization/mutex.h"
#include "capture_data.pb.h"

class OrbitApp;

namespace internal {
struct DrawData {
  uint64_t min_tick;
  uint64_t max_tick;
  uint64_t highlighted_function_id;
  uint64_t ns_per_pixel;
  uint64_t min_timegraph_tick;
  Batcher* batcher;
  orbit_gl::Viewport* viewport;
  const orbit_client_data::TextBox* selected_textbox;
  double inv_time_window;
  float world_start_x;
  float world_width;
  float z_offset;
  float z;
  bool is_collapsed;
};
}  // namespace internal

class TimerTrack : public Track {
 public:
  explicit TimerTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                      orbit_gl::Viewport* viewport, TimeGraphLayout* layout, OrbitApp* app,
                      const orbit_client_model::CaptureData* capture_data,
                      uint32_t indentation_level = 0);
  ~TimerTrack() override = default;

  // Pickable
  void Draw(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
            PickingMode picking_mode, float z_offset = 0) override;
  void OnTimer(const orbit_client_protos::TimerInfo& timer_info) override;
  [[nodiscard]] std::string GetTooltip() const override;

  // Track
  void UpdatePrimitives(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                        PickingMode /*picking_mode*/, float z_offset = 0) override;
  [[nodiscard]] Type GetType() const override { return Type::kTimerTrack; }

  [[nodiscard]] std::vector<std::shared_ptr<orbit_client_data::TimerChain>> GetTimers()
      const override;
  [[nodiscard]] uint32_t GetDepth() const { return depth_; }
  [[nodiscard]] std::string GetExtraInfo(const orbit_client_protos::TimerInfo& timer);

  [[nodiscard]] const orbit_client_data::TextBox* GetFirstAfterTime(uint64_t time,
                                                                    uint32_t depth) const;
  [[nodiscard]] const orbit_client_data::TextBox* GetFirstBeforeTime(uint64_t time,
                                                                     uint32_t depth) const;

  // Must be overriden by child class for sensible behavior.
  [[nodiscard]] virtual const orbit_client_data::TextBox* GetLeft(
      const orbit_client_data::TextBox* textbox) const {
    return textbox;
  };
  // Must be overriden by child class for sensible behavior.
  [[nodiscard]] virtual const orbit_client_data::TextBox* GetRight(
      const orbit_client_data::TextBox* textbox) const {
    return textbox;
  };

  [[nodiscard]] virtual const orbit_client_data::TextBox* GetUp(
      const orbit_client_data::TextBox* textbox) const;
  [[nodiscard]] virtual const orbit_client_data::TextBox* GetDown(
      const orbit_client_data::TextBox* textbox) const;

  [[nodiscard]] std::vector<std::shared_ptr<orbit_client_data::TimerChain>> GetAllChains()
      const override;
  [[nodiscard]] std::vector<std::shared_ptr<orbit_client_data::TimerChain>>
  GetAllSerializableChains() const override;
  [[nodiscard]] std::vector<const orbit_client_data::TextBox*> GetScopesInRange(
      uint64_t start_ns, uint64_t end_ns) const;
  [[nodiscard]] bool IsEmpty() const override;

  [[nodiscard]] bool IsCollapsible() const override { return depth_ > 1; }

  virtual void UpdateBoxHeight();
  [[nodiscard]] virtual float GetTextBoxHeight(
      const orbit_client_protos::TimerInfo& /*timer_info*/) const;
  [[nodiscard]] virtual float GetYFromTimer(const orbit_client_protos::TimerInfo& timer_info) const;
  [[nodiscard]] virtual float GetYFromDepth(uint32_t depth) const;

  [[nodiscard]] virtual float GetHeaderHeight() const;

  [[nodiscard]] int GetVisiblePrimitiveCount() const override { return visible_timer_count_; }

  float GetHeight() const override;

 protected:
  [[nodiscard]] virtual bool IsTimerActive(
      const orbit_client_protos::TimerInfo& /*timer_info*/) const {
    return true;
  }
  [[nodiscard]] virtual Color GetTimerColor(const orbit_client_protos::TimerInfo& timer_info,
                                            bool is_selected, bool is_highlighted) const = 0;
  [[nodiscard]] virtual bool TimerFilter(
      const orbit_client_protos::TimerInfo& /*timer_info*/) const {
    return true;
  }

  [[nodiscard]] bool DrawTimer(const orbit_client_data::TextBox* prev_text_box,
                               const orbit_client_data::TextBox* next_text_box,
                               const internal::DrawData& draw_data,
                               orbit_client_data::TextBox* current_text_box, uint64_t* min_ignore,
                               uint64_t* max_ignore);

  void UpdateDepth(uint32_t depth) {
    if (depth > depth_) depth_ = depth;
  }
  [[nodiscard]] std::shared_ptr<orbit_client_data::TimerChain> GetTimers(uint32_t depth) const;

  virtual void SetTimesliceText(const orbit_client_protos::TimerInfo& /*timer*/,
                                orbit_client_data::TextBox* /*text_box*/) {}

  virtual void DrawTimesliceText(const orbit_client_protos::TimerInfo& /*timer*/, float /*min_x*/,
                                 float /*z_offset*/, orbit_client_data::TextBox* /*text_box*/);

  [[nodiscard]] static internal::DrawData GetDrawData(
      uint64_t min_tick, uint64_t max_tick, float z_offset, Batcher* batcher, TimeGraph* time_graph,
      orbit_gl::Viewport* viewport, bool is_collapsed,
      const orbit_client_data::TextBox* selected_textbox, uint64_t highlighted_function_id);

  TextRenderer* text_renderer_ = nullptr;
  uint32_t depth_ = 0;
  mutable absl::Mutex mutex_;
  int visible_timer_count_ = 0;

  [[nodiscard]] virtual std::string GetBoxTooltip(const Batcher& batcher, PickingId id) const;
  [[nodiscard]] std::unique_ptr<PickingUserData> CreatePickingUserData(
      const Batcher& batcher, const orbit_client_data::TextBox& text_box) {
    return std::make_unique<PickingUserData>(
        &text_box, [this, &batcher](PickingId id) { return this->GetBoxTooltip(batcher, id); });
  }

  float box_height_ = 0.0f;

  static const Color kHighlightColor;

  OrbitApp* app_ = nullptr;
};

#endif  // ORBIT_GL_TIMER_TRACK_H_

// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_FRAME_TRACK_H_
#define ORBIT_GL_FRAME_TRACK_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "CallstackThreadBar.h"
#include "ClientData/TextBox.h"
#include "ClientData/TimerChain.h"
#include "CoreMath.h"
#include "PickingManager.h"
#include "TimerTrack.h"
#include "Track.h"
#include "Viewport.h"
#include "capture_data.pb.h"

class OrbitApp;

class FrameTrack : public TimerTrack {
 public:
  explicit FrameTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                      orbit_gl::Viewport* viewport, TimeGraphLayout* layout,
                      orbit_grpc_protos::InstrumentedFunction function, OrbitApp* app,
                      const orbit_client_model::CaptureData* capture_data,
                      uint32_t indentation_level = 0);

  [[nodiscard]] Type GetType() const override { return Type::kFrameTrack; }
  [[nodiscard]] uint64_t GetFunctionId() const { return function_.function_id(); }
  [[nodiscard]] bool IsCollapsible() const override { return GetMaximumScaleFactor() > 0.f; }

  [[nodiscard]] float GetYFromTimer(
      const orbit_client_protos::TimerInfo& timer_info) const override;
  void OnTimer(const orbit_client_protos::TimerInfo& timer_info) override;

  [[nodiscard]] float GetTextBoxHeight(
      const orbit_client_protos::TimerInfo& timer_info) const override;
  [[nodiscard]] float GetHeaderHeight() const override;

  [[nodiscard]] std::string GetTimesliceText(
      const orbit_client_protos::TimerInfo& timer) const override;
  [[nodiscard]] std::string GetTooltip() const override;
  [[nodiscard]] std::string GetBoxTooltip(const Batcher& batcher, PickingId id) const override;

  void Draw(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
            PickingMode picking_mode, float z_offset = 0) override;

  void UpdateBoxHeight() override;

  [[nodiscard]] std::vector<std::shared_ptr<orbit_client_data::TimerChain>>
  GetAllSerializableChains() const override;

 protected:
  [[nodiscard]] Color GetTimerColor(const orbit_client_protos::TimerInfo& timer_info,
                                    bool is_selected, bool is_highlighted) const override;
  [[nodiscard]] float GetHeight() const override;

 private:
  [[nodiscard]] float GetMaximumScaleFactor() const;
  [[nodiscard]] float GetMaximumBoxHeight() const;
  [[nodiscard]] float GetAverageBoxHeight() const;

  orbit_grpc_protos::InstrumentedFunction function_;
  orbit_client_protos::FunctionStats stats_;
};

#endif  // ORBIT_GL_FRAME_TRACK_H_

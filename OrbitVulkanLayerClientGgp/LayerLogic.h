// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_VULKAN_LAYER_CLIENT_GGP_LAYER_LOGIC_H_
#define ORBIT_VULKAN_LAYER_CLIENT_GGP_LAYER_LOGIC_H_

#include <chrono>
#include <string>

#include "OrbitCaptureGgpClient/OrbitCaptureGgpClient.h"

// Contains the logic of the OrbitVulkanLayerClientGgp to run Orbit captures automatically when the
// time per frame is higher than a certain threshold. It also instantiates the classes and variables
// needed for this so the layer itself is transparent to it.
class LayerLogic {
 public:
  LayerLogic() : data_initialized_{false}, orbit_capture_running_{false}, skip_logic_call_{true} {}

  void Init();
  void Destroy();
  void ProcessQueuePresentKHR();

 private:
  bool data_initialized_;
  bool orbit_capture_running_;
  bool skip_logic_call_;
  std::unique_ptr<CaptureClientGgpClient> ggp_capture_client_;
  std::chrono::steady_clock::time_point last_frame_time_;
  std::chrono::steady_clock::time_point capture_started_time_;

  void StartOrbitCaptureService();
  void RunCapture();
  void StopCapture();
};

#endif  // ORBIT_VULKAN_LAYER_CLIENT_GGP_LAYER_LOGIC_H_

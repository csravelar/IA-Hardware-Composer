/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#ifndef WSI_LOGICALDISPLAY_MANAGER_H_
#define WSI_LOGICALDISPLAY_MANAGER_H_

#include <stdint.h>
#include <stdlib.h>

#include <nativedisplay.h>
#include "logicaldisplay.h"

namespace hwcomposer {

class LogicalDisplayManager;

class LogicalDisplayManager {
 public:
  /**
  * Constructor that sets the physical display for the class.
  *
  * @param physical_display a NativeDisplay object representing a physical
  *                         display
  */
  LogicalDisplayManager(NativeDisplay* physical_display);
  ~LogicalDisplayManager();

  /**
  * Initialize new logical displays up to the passed in total count.
  *
  * @param total an unsigned 32 bit integer to dictate the amount of logical
  *              displays to initialize
  */
  void InitializeLogicalDisplays(uint32_t total);
  /**
  * Switch the physical display's power mode to on or off based on previous state.
  */
  void UpdatePowerMode();
  /**
  * Enable or disable the VSyncControl based on the physical display's EnableVSync.
  */
  void UpdateVSyncControl();
  /**
  * Register the hot plug and notifications if the hot plug is not already registered.
  */
  void RegisterHotPlugNotification();

  bool Present(std::vector<HwcLayer*>& source_layers, int32_t* retire_fence,
               PixelUploaderCallback* call_back, bool handle_constraints);

  /**
  * Run all display's VSyncUpdate and pass timestamp to each call.
  *
  * @param timestamp a 64 bit integer
  */
  void VSyncCallback(int64_t timestamp);

  /**
  * Run all display's RefreshUpdate
  */
  void RefreshCallback();

  /**
  * Run all display's HotPlugUpdate and pass the parameter connected to each.
  *
  * @param connected a boolean value
  */
  void HotPlugCallback(bool connected);

  NativeDisplay* GetPhysicalDisplay() const {
    return physical_display_;
  }

  void GetLogicalDisplays(std::vector<LogicalDisplay*>& displays);

  void SetHDCPState(HWCContentProtection state, HWCContentType content_type);

 private:
  NativeDisplay* physical_display_;
  std::vector<std::unique_ptr<LogicalDisplay>> displays_;
  std::vector<HwcLayer*> layers_;
  std::vector<HwcLayer*> cursor_layers_;
  uint32_t queued_displays_ = 0;
  bool hot_plug_registered_ = false;
  bool handle_hoplug_notifications_ = false;
};

}  // namespace hwcomposer
#endif  // WSI_LOGICALDISPLAY_MANAGER_H_

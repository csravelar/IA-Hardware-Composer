/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "iahwc2.h"
#include "utils_android.h"

#include <inttypes.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>

#include <gpudevice.h>
#include <hwcdefs.h>
#include <nativedisplay.h>

namespace android {

class IAHWC2VsyncCallback : public hwcomposer::VsyncCallback {
 public:
  IAHWC2VsyncCallback(hwc2_callback_data_t data, hwc2_function_pointer_t hook)
      : data_(data), hook_(hook) {
  }

  void Callback(uint32_t display, int64_t timestamp) {
    auto hook = reinterpret_cast<HWC2_PFN_VSYNC>(hook_);
    hook(data_, display, timestamp);
  }

 private:
  hwc2_callback_data_t data_;
  hwc2_function_pointer_t hook_;
};

class IAHWC2RefreshCallback : public hwcomposer::RefreshCallback {
 public:
  IAHWC2RefreshCallback(hwc2_callback_data_t data, hwc2_function_pointer_t hook)
      : data_(data), hook_(hook) {
  }

  void Callback(uint32_t display) {
    auto hook = reinterpret_cast<HWC2_PFN_REFRESH>(hook_);
    hook(data_, display);
  }

 private:
  hwc2_callback_data_t data_;
  hwc2_function_pointer_t hook_;
};

IAHWC2::IAHWC2() {
  common.tag = HARDWARE_DEVICE_TAG;
  common.version = HWC_DEVICE_API_VERSION_2_0;
  common.close = HookDevClose;
  getCapabilities = HookDevGetCapabilities;
  getFunction = HookDevGetFunction;
}

HWC2::Error IAHWC2::Init() {
  char value[PROPERTY_VALUE_MAX];
  property_get("board.disable.explicit.sync", value, "0");
  disable_explicit_sync_ = atoi(value);
  if (disable_explicit_sync_)
    ALOGI("EXPLICIT SYNC support is disabled");
  else
    ALOGI("EXPLICIT SYNC support is enabled");

  displays_.emplace(std::piecewise_construct,
                    std::forward_as_tuple(HWC_DISPLAY_PRIMARY),
                    std::forward_as_tuple(&device_, HWC_DISPLAY_PRIMARY,
                                          HWC2::DisplayType::Physical));

  displays_.at(HWC_DISPLAY_PRIMARY).Init(disable_explicit_sync_);

  // Start the hwc service
  // FIXME(IAHWC-76): On Android, with userdebug on Joule this is causing
  //        to hang in a loop while cold boot before going to home screen
  // hwcService_.Start(*this);

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::CreateVirtualDisplay(uint32_t width, uint32_t height,
                                         int32_t *format,
                                         hwc2_display_t *display) {
  displays_.emplace(std::piecewise_construct,
                    std::forward_as_tuple(HWC_DISPLAY_VIRTUAL),
                    std::forward_as_tuple(&device_, HWC_DISPLAY_VIRTUAL,
                                          HWC2::DisplayType::Virtual));
  *display = (hwc2_display_t)HWC_DISPLAY_VIRTUAL;
  displays_.at(*display).Init(width, height, disable_explicit_sync_);
  if (*format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
    // fallback to RGBA_8888, align with framework requirement
    *format = HAL_PIXEL_FORMAT_RGBA_8888;
  }

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::DestroyVirtualDisplay(hwc2_display_t display) {
  if (display != (hwc2_display_t)HWC_DISPLAY_VIRTUAL) {
    ALOGE("Not Virtual Display Type in DestroyVirtualDisplay");
    return HWC2::Error::BadDisplay;
  }

  displays_.erase(display);
  return HWC2::Error::None;
}

void IAHWC2::Dump(uint32_t * /*size*/, char * /*buffer*/) {
  ALOGV("Unsupported function: %s", __func__);
}

uint32_t IAHWC2::GetMaxVirtualDisplayCount() {
  return 1;
}

HWC2::Error IAHWC2::RegisterCallback(int32_t descriptor,
                                     hwc2_callback_data_t data,
                                     hwc2_function_pointer_t function) {
  auto callback = static_cast<HWC2::Callback>(descriptor);

  switch (callback) {
    case HWC2::Callback::Hotplug: {
      auto hotplug = reinterpret_cast<HWC2_PFN_HOTPLUG>(function);
      hotplug(data, HWC_DISPLAY_PRIMARY,
              static_cast<int32_t>(HWC2::Connection::Connected));
      break;
    }
    case HWC2::Callback::Vsync: {
      for (std::pair<const hwc2_display_t, IAHWC2::HwcDisplay> &d : displays_)
        d.second.RegisterVsyncCallback(data, function);
      break;
    }
    case HWC2::Callback::Refresh: {
      for (std::pair<const hwc2_display_t, IAHWC2::HwcDisplay> &d : displays_)
        d.second.RegisterRefreshCallback(data, function);
      break;
    }
    default:
      break;
  }
  return HWC2::Error::None;
}

IAHWC2::HwcDisplay::HwcDisplay(hwcomposer::GpuDevice *device,
                               hwc2_display_t handle, HWC2::DisplayType type)
    : device_(device), handle_(handle), type_(type) {
}

// This function will be called only for Virtual Display Init
HWC2::Error IAHWC2::HwcDisplay::Init(uint32_t width, uint32_t height,
                                     bool disable_explicit_sync) {
  if (!device_->Initialize()) {
    ALOGE("Can't initialize drm object.");
    return HWC2::Error::NoResources;
  }
  display_ = device_->GetVirtualDisplay();
  display_->InitVirtualDisplay(width, height);
  return Init(disable_explicit_sync);
}

HWC2::Error IAHWC2::HwcDisplay::Init(bool disable_explicit_sync) {
  if (type_ != HWC2::DisplayType::Virtual) {
    int display = static_cast<int>(handle_);
    if (!device_->Initialize()) {
      ALOGE("Can't initialize drm object.");
      return HWC2::Error::NoResources;
    }
    display_ = device_->GetDisplay(display);
    if (!display_) {
      ALOGE("Failed to retrieve display %d", display);
      return HWC2::Error::BadDisplay;
    }
  }

  disable_explicit_sync_ = disable_explicit_sync;
  display_->SetExplicitSyncSupport(disable_explicit_sync_);

  // Fetch the number of modes from the display
  uint32_t num_configs;
  HWC2::Error err = GetDisplayConfigs(&num_configs, NULL);
  if (err != HWC2::Error::None || !num_configs)
    return err;

  // Grab the first mode, we'll choose this as the active mode
  hwc2_config_t default_config;
  num_configs = 1;
  err = GetDisplayConfigs(&num_configs, &default_config);
  if (err != HWC2::Error::None)
    return err;

  return SetActiveConfig(default_config);
}

HWC2::Error IAHWC2::HwcDisplay::RegisterVsyncCallback(
    hwc2_callback_data_t data, hwc2_function_pointer_t func) {
  auto callback = std::make_shared<IAHWC2VsyncCallback>(data, func);
  int ret = display_->RegisterVsyncCallback(std::move(callback),
                                            static_cast<int>(handle_));
  if (ret) {
    ALOGE("Failed to register callback d=%" PRIu64 " ret=%d", handle_, ret);
    return HWC2::Error::BadDisplay;
  }
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::RegisterRefreshCallback(
    hwc2_callback_data_t data, hwc2_function_pointer_t func) {
  auto callback = std::make_shared<IAHWC2RefreshCallback>(data, func);
  display_->RegisterRefreshCallback(std::move(callback),
                                    static_cast<int>(handle_));
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::AcceptDisplayChanges() {
  if (!checkValidateDisplay) {
    ALOGV("AcceptChanges failed, not validated");
    return HWC2::Error::NotValidated;
  }

  for (std::pair<const hwc2_layer_t, IAHWC2::HwcLayer> &l : layers_)
    l.second.accept_type_change();

  // reset the value to false
  checkValidateDisplay = false;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::CreateLayer(hwc2_layer_t *layer) {
  layers_.emplace(static_cast<hwc2_layer_t>(layer_idx_), HwcLayer());
  *layer = static_cast<hwc2_layer_t>(layer_idx_);
  ++layer_idx_;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::DestroyLayer(hwc2_layer_t layer) {
  layers_.erase(layer);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetActiveConfig(hwc2_config_t *config) {
  if (!display_->GetActiveConfig(config))
    return HWC2::Error::BadConfig;

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetChangedCompositionTypes(
    uint32_t *num_elements, hwc2_layer_t *layers, int32_t *types) {
  uint32_t num_changes = 0;
  for (std::pair<const hwc2_layer_t, IAHWC2::HwcLayer> &l : layers_) {
    if (l.second.type_changed()) {
      if (layers && num_changes < *num_elements)
        layers[num_changes] = l.first;
      if (types && num_changes < *num_elements)
        types[num_changes] = static_cast<int32_t>(l.second.validated_type());
      ++num_changes;
    }
  }
  if (!layers && !types)
    *num_elements = num_changes;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetClientTargetSupport(uint32_t width,
                                                       uint32_t height,
                                                       int32_t format,
                                                       int32_t dataspace) {
  if (width != display_->Width() || height != display_->Height()) {
    return HWC2::Error::Unsupported;
  }

  if (format == HAL_PIXEL_FORMAT_RGBA_8888 &&
      (dataspace == HAL_DATASPACE_UNKNOWN ||
       dataspace == HAL_DATASPACE_STANDARD_UNSPECIFIED)) {
    return HWC2::Error::None;
  } else {
    // Convert HAL to fourcc-based DRM formats
    uint32_t drm_format = GetDrmFormatFromHALFormat(format);
    if (display_->CheckPlaneFormat(drm_format) &&
        (dataspace == HAL_DATASPACE_UNKNOWN ||
         dataspace == HAL_DATASPACE_STANDARD_UNSPECIFIED))
      return HWC2::Error::None;
  }

  return HWC2::Error::Unsupported;
}

HWC2::Error IAHWC2::HwcDisplay::GetColorModes(uint32_t *num_modes,
                                              int32_t *modes) {
  if (!modes)
    *num_modes = 1;
#ifndef DISABLE_NATIVE_COLOR_MODES
  if (modes)
    *modes = HAL_COLOR_MODE_NATIVE;
#endif

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetDisplayAttribute(hwc2_config_t config,
                                                    int32_t attribute_in,
                                                    int32_t *value) {
  auto attribute = static_cast<HWC2::Attribute>(attribute_in);
  switch (attribute) {
    case HWC2::Attribute::Width:
      display_->GetDisplayAttribute(
          config, hwcomposer::HWCDisplayAttribute::kWidth, value);
      break;
    case HWC2::Attribute::Height:
      display_->GetDisplayAttribute(
          config, hwcomposer::HWCDisplayAttribute::kHeight, value);
      break;
    case HWC2::Attribute::VsyncPeriod:
      // in nanoseconds
      display_->GetDisplayAttribute(
          config, hwcomposer::HWCDisplayAttribute::kRefreshRate, value);
      break;
    case HWC2::Attribute::DpiX:
      // Dots per 1000 inches
      display_->GetDisplayAttribute(
          config, hwcomposer::HWCDisplayAttribute::kDpiX, value);
      break;
    case HWC2::Attribute::DpiY:
      // Dots per 1000 inches
      display_->GetDisplayAttribute(
          config, hwcomposer::HWCDisplayAttribute::kDpiY, value);
      break;
    default:
      *value = -1;
      return HWC2::Error::BadConfig;
  }
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetDisplayConfigs(uint32_t *num_configs,
                                                  hwc2_config_t *configs) {
  if (!display_->GetDisplayConfigs(num_configs, configs))
    return HWC2::Error::BadDisplay;

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetDisplayName(uint32_t *size, char *name) {
  if (!display_->GetDisplayName(size, name))
    return HWC2::Error::BadDisplay;

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetDisplayRequests(
    int32_t * /*display_requests*/, uint32_t *num_elements,
    hwc2_layer_t * /*layers*/, int32_t * /*layer_requests*/) {
  *num_elements = 0;
  ALOGV("Unsupported function: %s", __func__);
  return HWC2::Error::Unsupported;
}

HWC2::Error IAHWC2::HwcDisplay::GetDisplayType(int32_t *type) {
  *type = static_cast<int32_t>(type_);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetDozeSupport(int32_t *support) {
  *support = true;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetHdrCapabilities(
    uint32_t *num_types, int32_t * /*types*/, float * /*max_luminance*/,
    float * /*max_average_luminance*/, float * /*min_luminance*/) {
  *num_types = 0;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::GetReleaseFences(uint32_t *num_elements,
                                                 hwc2_layer_t *layers,
                                                 int32_t *fences) {
  if (layers == NULL || fences == NULL) {
    *num_elements = layers_.size();
    return HWC2::Error::None;
  }

  uint32_t num_layers = 0;
  for (std::pair<const hwc2_layer_t, IAHWC2::HwcLayer> &l : layers_) {
    ++num_layers;
    if (num_layers > *num_elements) {
      ALOGW("Overflow num_elements %d/%d", num_layers, *num_elements);
      return HWC2::Error::None;
    }

    layers[num_layers - 1] = l.first;
    fences[num_layers - 1] = l.second.GetLayer()->GetReleaseFence();
  }

  *num_elements = num_layers;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::PresentDisplay(int32_t *retire_fence) {
  // order the layers by z-order
  bool use_client_layer = false;
  uint32_t client_z_order = 0;
  bool use_cursor_layer = false;
  uint32_t cursor_z_order = 0;
  IAHWC2::HwcLayer *cursor_layer;
  *retire_fence = -1;
  std::map<uint32_t, IAHWC2::HwcLayer *> z_map;

  // if the power mode is doze suspend then its the hint that the drawing
  // into the display has suspended and remain in the low power state and
  // continue displaying the current state and stop applying display
  // update from the client
  if (display_->PowerMode() == HWC2_POWER_MODE_DOZE_SUSPEND)
    return HWC2::Error::None;
  for (std::pair<const hwc2_layer_t, IAHWC2::HwcLayer> &l : layers_) {
    switch (l.second.validated_type()) {
      case HWC2::Composition::Device:
        z_map.emplace(std::make_pair(l.second.z_order(), &l.second));
        break;
      case HWC2::Composition::Cursor:
        use_cursor_layer = true;
        cursor_layer = &l.second;
        cursor_z_order = l.second.z_order();
        break;
      case HWC2::Composition::Client:
        // Place it at the z_order of the highest client layer
        use_client_layer = true;
        client_z_order = std::max(client_z_order, l.second.z_order());
        break;
      default:
        continue;
    }
  }
  if (use_client_layer && client_layer_.GetLayer() &&
      client_layer_.GetLayer()->GetNativeHandle() &&
      client_layer_.GetLayer()->GetNativeHandle()->handle_) {
    z_map.emplace(std::make_pair(client_z_order, &client_layer_));
  }

  // Place the cursor at the highest z-order
  if (use_cursor_layer) {
    if (z_map.size()) {
      if (z_map.rbegin()->second->z_order() > cursor_z_order)
        cursor_z_order = (z_map.rbegin()->second->z_order()) + 1;
    }
    z_map.emplace(std::make_pair(cursor_z_order, cursor_layer));
  }

  std::vector<hwcomposer::HwcLayer *> layers;
  // now that they're ordered by z, add them to the composition
  for (std::pair<const uint32_t, IAHWC2::HwcLayer *> &l : z_map) {
    layers.emplace_back(l.second->GetLayer());
  }

  if (layers.empty())
    return HWC2::Error::None;

  bool success = display_->Present(layers, retire_fence);
  if (!success) {
    ALOGE("Failed to set layers in the composition");
    return HWC2::Error::BadLayer;
  }

  ++frame_no_;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::SetActiveConfig(hwc2_config_t config) {
  if (!display_->SetActiveConfig(config)) {
    ALOGE("Could not find active mode for %d", config);
    return HWC2::Error::BadConfig;
  }

  // Setup the client layer's dimensions
  hwc_rect_t display_frame = {.left = 0,
                              .top = 0,
                              .right = static_cast<int>(display_->Width()),
                              .bottom = static_cast<int>(display_->Height())};
  client_layer_.SetLayerDisplayFrame(display_frame);
  hwc_frect_t source_crop = {.left = 0.0f,
                             .top = 0.0f,
                             .right = display_->Width() + 0.0f,
                             .bottom = display_->Height() + 0.0f};
  client_layer_.SetLayerSourceCrop(source_crop);

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::SetClientTarget(buffer_handle_t target,
                                                int32_t acquire_fence,
                                                int32_t dataspace,
                                                hwc_region_t damage) {
  client_layer_.set_buffer(target);
  client_layer_.set_acquire_fence(acquire_fence);
  client_layer_.SetLayerDataspace(dataspace);
  client_layer_.SetLayerSurfaceDamage(damage);

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::SetColorMode(int32_t mode) {
  color_mode_ = mode;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::SetColorTransform(const float * /*matrix*/,
                                                  int32_t /*hint*/) {
  ALOGV("Unsupported function: %s", __func__);
  return HWC2::Error::Unsupported;
}

HWC2::Error IAHWC2::HwcDisplay::SetOutputBuffer(buffer_handle_t buffer,
                                                int32_t release_fence) {
  struct gralloc_handle *temp = new struct gralloc_handle();
  temp->handle_ = buffer;
  display_->SetOutputBuffer(temp, release_fence);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::SetPowerMode(int32_t mode_in) {
  uint32_t power_mode = 0;
  auto mode = static_cast<HWC2::PowerMode>(mode_in);
  switch (mode) {
    case HWC2::PowerMode::Off:
      power_mode = HWC2_POWER_MODE_OFF;
      break;
    case HWC2::PowerMode::Doze:
      power_mode = HWC2_POWER_MODE_DOZE;
      break;
    case HWC2::PowerMode::DozeSuspend:
      power_mode = HWC2_POWER_MODE_DOZE_SUSPEND;
      break;
    case HWC2::PowerMode::On:
      power_mode = HWC2_POWER_MODE_ON;
      break;
    default:
      ALOGI("Power mode %d is unsupported\n", mode);
      return HWC2::Error::BadParameter;
  };

  display_->SetPowerMode(power_mode);

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::SetVsyncEnabled(int32_t enabled) {
  display_->VSyncControl(enabled);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcDisplay::ValidateDisplay(uint32_t *num_types,
                                                uint32_t *num_requests) {
  *num_requests = 0;
  uint32_t total_types = 0;
  for (std::pair<const hwc2_layer_t, IAHWC2::HwcLayer> &l : layers_) {
    IAHWC2::HwcLayer &layer = l.second;
    switch (layer.sf_type()) {
      case HWC2::Composition::SolidColor:
      case HWC2::Composition::Sideband:
        layer.set_validated_type(HWC2::Composition::Client);
        ++total_types;
        break;
      default:
        if (disable_explicit_sync_ ||
            display_->PowerMode() == HWC2_POWER_MODE_DOZE_SUSPEND) {
          layer.set_validated_type(HWC2::Composition::Client);
        } else {
          layer.set_validated_type(layer.sf_type());
        }
        break;
    }
  }

  checkValidateDisplay = true;
  *num_types = total_types;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetCursorPosition(int32_t x, int32_t y) {
  cursor_x_ = x;
  cursor_y_ = y;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerBlendMode(int32_t mode) {
  switch (static_cast<HWC2::BlendMode>(mode)) {
    case HWC2::BlendMode::None:
      hwc_layer_.SetBlending(hwcomposer::HWCBlending::kBlendingNone);
      break;
    case HWC2::BlendMode::Premultiplied:
      hwc_layer_.SetBlending(hwcomposer::HWCBlending::kBlendingPremult);
      break;
    case HWC2::BlendMode::Coverage:
      hwc_layer_.SetBlending(hwcomposer::HWCBlending::kBlendingCoverage);
      break;
    default:
      ALOGE("Unknown blending mode b=%d", mode);
      hwc_layer_.SetBlending(hwcomposer::HWCBlending::kBlendingNone);
      break;
  }
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerBuffer(buffer_handle_t buffer,
                                             int32_t acquire_fence) {
  // The buffer and acquire_fence are handled elsewhere
  if (sf_type_ == HWC2::Composition::Client ||
      sf_type_ == HWC2::Composition::Sideband ||
      sf_type_ == HWC2::Composition::SolidColor)
    return HWC2::Error::None;

  native_handle_.handle_ = buffer;
  hwc_layer_.SetNativeHandle(&native_handle_);
  if (acquire_fence > 0)
    hwc_layer_.SetAcquireFence(dup(acquire_fence));
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerColor(hwc_color_t /*color*/) {
  sf_type_ = HWC2::Composition::Client;
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerCompositionType(int32_t type) {
  sf_type_ = static_cast<HWC2::Composition>(type);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerDataspace(int32_t dataspace) {
  dataspace_ = static_cast<android_dataspace_t>(dataspace);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerDisplayFrame(hwc_rect_t frame) {
  hwc_layer_.SetDisplayFrame(hwcomposer::HwcRect<int>(
      frame.left, frame.top, frame.right, frame.bottom));
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerPlaneAlpha(float alpha) {
  hwc_layer_.SetAlpha(static_cast<uint8_t>(255.0f * alpha + 0.5f));
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerSidebandStream(
    const native_handle_t * /*stream*/) {
  ALOGV("Unsupported function: %s", __func__);
  return HWC2::Error::Unsupported;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerSourceCrop(hwc_frect_t crop) {
  hwc_layer_.SetSourceCrop(
      hwcomposer::HwcRect<float>(crop.left, crop.top, crop.right, crop.bottom));
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerSurfaceDamage(hwc_region_t damage) {
  uint32_t num_rects = damage.numRects;
  hwcomposer::HwcRegion hwc_region;

  for (size_t rect = 0; rect < num_rects; ++rect) {
    hwc_region.emplace_back(damage.rects[rect].left, damage.rects[rect].top,
                            damage.rects[rect].right,
                            damage.rects[rect].bottom);
  }

  hwc_layer_.SetSurfaceDamage(hwc_region);

  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerTransform(int32_t transform) {
  // 270* and 180* cannot be combined with flips. More specifically, they
  // already contain both horizontal and vertical flips, so those fields are
  // redundant in this case. 90* rotation can be combined with either horizontal
  // flip or vertical flip, so treat it differently
  if (transform == HWC_TRANSFORM_ROT_270) {
    transform = hwcomposer::HWCTransform::kRotate270;
  } else if (transform == HWC_TRANSFORM_ROT_180) {
    transform = hwcomposer::HWCTransform::kRotate180;
  } else {
    if (transform & HWC_TRANSFORM_FLIP_H)
      transform |= hwcomposer::HWCTransform::kReflectX;
    if (transform & HWC_TRANSFORM_FLIP_V)
      transform |= hwcomposer::HWCTransform::kReflectY;
    if (transform & HWC_TRANSFORM_ROT_90)
      transform |= hwcomposer::HWCTransform::kRotate90;
  }
  hwc_layer_.SetTransform(transform);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerVisibleRegion(hwc_region_t visible) {
  uint32_t num_rects = visible.numRects;
  hwcomposer::HwcRegion hwc_region;

  for (size_t rect = 0; rect < num_rects; ++rect) {
    hwc_region.emplace_back(visible.rects[rect].left, visible.rects[rect].top,
                            visible.rects[rect].right,
                            visible.rects[rect].bottom);
  }

  hwc_layer_.SetVisibleRegion(hwc_region);
  return HWC2::Error::None;
}

HWC2::Error IAHWC2::HwcLayer::SetLayerZOrder(uint32_t order) {
  hwc_layer_.SetLayerZOrder(order);
  return HWC2::Error::None;
}

// static
int IAHWC2::HookDevClose(hw_device_t * /*dev*/) {
  ALOGV("Unsupported function: %s", __func__);
  return 0;
}

// static
void IAHWC2::HookDevGetCapabilities(hwc2_device_t * /*dev*/,
                                    uint32_t *out_count,
                                    int32_t * /*out_capabilities*/) {
  *out_count = 0;
}

// static
hwc2_function_pointer_t IAHWC2::HookDevGetFunction(struct hwc2_device * /*dev*/,
                                                   int32_t descriptor) {
  auto func = static_cast<HWC2::FunctionDescriptor>(descriptor);
  switch (func) {
    // Device functions
    case HWC2::FunctionDescriptor::CreateVirtualDisplay:
      return ToHook<HWC2_PFN_CREATE_VIRTUAL_DISPLAY>(
          DeviceHook<int32_t, decltype(&IAHWC2::CreateVirtualDisplay),
                     &IAHWC2::CreateVirtualDisplay, uint32_t, uint32_t,
                     int32_t *, hwc2_display_t *>);
    case HWC2::FunctionDescriptor::DestroyVirtualDisplay:
      return ToHook<HWC2_PFN_DESTROY_VIRTUAL_DISPLAY>(
          DeviceHook<int32_t, decltype(&IAHWC2::DestroyVirtualDisplay),
                     &IAHWC2::DestroyVirtualDisplay, hwc2_display_t>);
    case HWC2::FunctionDescriptor::Dump:
      return ToHook<HWC2_PFN_DUMP>(DeviceHook<
          void, decltype(&IAHWC2::Dump), &IAHWC2::Dump, uint32_t *, char *>);
    case HWC2::FunctionDescriptor::GetMaxVirtualDisplayCount:
      return ToHook<HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT>(
          DeviceHook<uint32_t, decltype(&IAHWC2::GetMaxVirtualDisplayCount),
                     &IAHWC2::GetMaxVirtualDisplayCount>);
    case HWC2::FunctionDescriptor::RegisterCallback:
      return ToHook<HWC2_PFN_REGISTER_CALLBACK>(
          DeviceHook<int32_t, decltype(&IAHWC2::RegisterCallback),
                     &IAHWC2::RegisterCallback, int32_t, hwc2_callback_data_t,
                     hwc2_function_pointer_t>);

    // Display functions
    case HWC2::FunctionDescriptor::AcceptDisplayChanges:
      return ToHook<HWC2_PFN_ACCEPT_DISPLAY_CHANGES>(
          DisplayHook<decltype(&HwcDisplay::AcceptDisplayChanges),
                      &HwcDisplay::AcceptDisplayChanges>);
    case HWC2::FunctionDescriptor::CreateLayer:
      return ToHook<HWC2_PFN_CREATE_LAYER>(
          DisplayHook<decltype(&HwcDisplay::CreateLayer),
                      &HwcDisplay::CreateLayer, hwc2_layer_t *>);
    case HWC2::FunctionDescriptor::DestroyLayer:
      return ToHook<HWC2_PFN_DESTROY_LAYER>(
          DisplayHook<decltype(&HwcDisplay::DestroyLayer),
                      &HwcDisplay::DestroyLayer, hwc2_layer_t>);
    case HWC2::FunctionDescriptor::GetActiveConfig:
      return ToHook<HWC2_PFN_GET_ACTIVE_CONFIG>(
          DisplayHook<decltype(&HwcDisplay::GetActiveConfig),
                      &HwcDisplay::GetActiveConfig, hwc2_config_t *>);
    case HWC2::FunctionDescriptor::GetChangedCompositionTypes:
      return ToHook<HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES>(
          DisplayHook<decltype(&HwcDisplay::GetChangedCompositionTypes),
                      &HwcDisplay::GetChangedCompositionTypes, uint32_t *,
                      hwc2_layer_t *, int32_t *>);
    case HWC2::FunctionDescriptor::GetClientTargetSupport:
      return ToHook<HWC2_PFN_GET_CLIENT_TARGET_SUPPORT>(
          DisplayHook<decltype(&HwcDisplay::GetClientTargetSupport),
                      &HwcDisplay::GetClientTargetSupport, uint32_t, uint32_t,
                      int32_t, int32_t>);
    case HWC2::FunctionDescriptor::GetColorModes:
      return ToHook<HWC2_PFN_GET_COLOR_MODES>(
          DisplayHook<decltype(&HwcDisplay::GetColorModes),
                      &HwcDisplay::GetColorModes, uint32_t *, int32_t *>);
    case HWC2::FunctionDescriptor::GetDisplayAttribute:
      return ToHook<HWC2_PFN_GET_DISPLAY_ATTRIBUTE>(DisplayHook<
          decltype(&HwcDisplay::GetDisplayAttribute),
          &HwcDisplay::GetDisplayAttribute, hwc2_config_t, int32_t, int32_t *>);
    case HWC2::FunctionDescriptor::GetDisplayConfigs:
      return ToHook<HWC2_PFN_GET_DISPLAY_CONFIGS>(DisplayHook<
          decltype(&HwcDisplay::GetDisplayConfigs),
          &HwcDisplay::GetDisplayConfigs, uint32_t *, hwc2_config_t *>);
    case HWC2::FunctionDescriptor::GetDisplayName:
      return ToHook<HWC2_PFN_GET_DISPLAY_NAME>(
          DisplayHook<decltype(&HwcDisplay::GetDisplayName),
                      &HwcDisplay::GetDisplayName, uint32_t *, char *>);
    case HWC2::FunctionDescriptor::GetDisplayRequests:
      return ToHook<HWC2_PFN_GET_DISPLAY_REQUESTS>(
          DisplayHook<decltype(&HwcDisplay::GetDisplayRequests),
                      &HwcDisplay::GetDisplayRequests, int32_t *, uint32_t *,
                      hwc2_layer_t *, int32_t *>);
    case HWC2::FunctionDescriptor::GetDisplayType:
      return ToHook<HWC2_PFN_GET_DISPLAY_TYPE>(
          DisplayHook<decltype(&HwcDisplay::GetDisplayType),
                      &HwcDisplay::GetDisplayType, int32_t *>);
    case HWC2::FunctionDescriptor::GetDozeSupport:
      return ToHook<HWC2_PFN_GET_DOZE_SUPPORT>(
          DisplayHook<decltype(&HwcDisplay::GetDozeSupport),
                      &HwcDisplay::GetDozeSupport, int32_t *>);
    case HWC2::FunctionDescriptor::GetHdrCapabilities:
      return ToHook<HWC2_PFN_GET_HDR_CAPABILITIES>(
          DisplayHook<decltype(&HwcDisplay::GetHdrCapabilities),
                      &HwcDisplay::GetHdrCapabilities, uint32_t *, int32_t *,
                      float *, float *, float *>);
    case HWC2::FunctionDescriptor::GetReleaseFences:
      return ToHook<HWC2_PFN_GET_RELEASE_FENCES>(
          DisplayHook<decltype(&HwcDisplay::GetReleaseFences),
                      &HwcDisplay::GetReleaseFences, uint32_t *, hwc2_layer_t *,
                      int32_t *>);
    case HWC2::FunctionDescriptor::PresentDisplay:
      return ToHook<HWC2_PFN_PRESENT_DISPLAY>(
          DisplayHook<decltype(&HwcDisplay::PresentDisplay),
                      &HwcDisplay::PresentDisplay, int32_t *>);
    case HWC2::FunctionDescriptor::SetActiveConfig:
      return ToHook<HWC2_PFN_SET_ACTIVE_CONFIG>(
          DisplayHook<decltype(&HwcDisplay::SetActiveConfig),
                      &HwcDisplay::SetActiveConfig, hwc2_config_t>);
    case HWC2::FunctionDescriptor::SetClientTarget:
      return ToHook<HWC2_PFN_SET_CLIENT_TARGET>(DisplayHook<
          decltype(&HwcDisplay::SetClientTarget), &HwcDisplay::SetClientTarget,
          buffer_handle_t, int32_t, int32_t, hwc_region_t>);
    case HWC2::FunctionDescriptor::SetColorMode:
      return ToHook<HWC2_PFN_SET_COLOR_MODE>(
          DisplayHook<decltype(&HwcDisplay::SetColorMode),
                      &HwcDisplay::SetColorMode, int32_t>);
    case HWC2::FunctionDescriptor::SetColorTransform:
      return ToHook<HWC2_PFN_SET_COLOR_TRANSFORM>(
          DisplayHook<decltype(&HwcDisplay::SetColorTransform),
                      &HwcDisplay::SetColorTransform, const float *, int32_t>);
    case HWC2::FunctionDescriptor::SetOutputBuffer:
      return ToHook<HWC2_PFN_SET_OUTPUT_BUFFER>(
          DisplayHook<decltype(&HwcDisplay::SetOutputBuffer),
                      &HwcDisplay::SetOutputBuffer, buffer_handle_t, int32_t>);
    case HWC2::FunctionDescriptor::SetPowerMode:
      return ToHook<HWC2_PFN_SET_POWER_MODE>(
          DisplayHook<decltype(&HwcDisplay::SetPowerMode),
                      &HwcDisplay::SetPowerMode, int32_t>);
    case HWC2::FunctionDescriptor::SetVsyncEnabled:
      return ToHook<HWC2_PFN_SET_VSYNC_ENABLED>(
          DisplayHook<decltype(&HwcDisplay::SetVsyncEnabled),
                      &HwcDisplay::SetVsyncEnabled, int32_t>);
    case HWC2::FunctionDescriptor::ValidateDisplay:
      return ToHook<HWC2_PFN_VALIDATE_DISPLAY>(
          DisplayHook<decltype(&HwcDisplay::ValidateDisplay),
                      &HwcDisplay::ValidateDisplay, uint32_t *, uint32_t *>);

    // Layer functions
    case HWC2::FunctionDescriptor::SetCursorPosition:
      return ToHook<HWC2_PFN_SET_CURSOR_POSITION>(
          LayerHook<decltype(&HwcLayer::SetCursorPosition),
                    &HwcLayer::SetCursorPosition, int32_t, int32_t>);
    case HWC2::FunctionDescriptor::SetLayerBlendMode:
      return ToHook<HWC2_PFN_SET_LAYER_BLEND_MODE>(
          LayerHook<decltype(&HwcLayer::SetLayerBlendMode),
                    &HwcLayer::SetLayerBlendMode, int32_t>);
    case HWC2::FunctionDescriptor::SetLayerBuffer:
      return ToHook<HWC2_PFN_SET_LAYER_BUFFER>(
          LayerHook<decltype(&HwcLayer::SetLayerBuffer),
                    &HwcLayer::SetLayerBuffer, buffer_handle_t, int32_t>);
    case HWC2::FunctionDescriptor::SetLayerColor:
      return ToHook<HWC2_PFN_SET_LAYER_COLOR>(
          LayerHook<decltype(&HwcLayer::SetLayerColor),
                    &HwcLayer::SetLayerColor, hwc_color_t>);
    case HWC2::FunctionDescriptor::SetLayerCompositionType:
      return ToHook<HWC2_PFN_SET_LAYER_COMPOSITION_TYPE>(
          LayerHook<decltype(&HwcLayer::SetLayerCompositionType),
                    &HwcLayer::SetLayerCompositionType, int32_t>);
    case HWC2::FunctionDescriptor::SetLayerDataspace:
      return ToHook<HWC2_PFN_SET_LAYER_DATASPACE>(
          LayerHook<decltype(&HwcLayer::SetLayerDataspace),
                    &HwcLayer::SetLayerDataspace, int32_t>);
    case HWC2::FunctionDescriptor::SetLayerDisplayFrame:
      return ToHook<HWC2_PFN_SET_LAYER_DISPLAY_FRAME>(
          LayerHook<decltype(&HwcLayer::SetLayerDisplayFrame),
                    &HwcLayer::SetLayerDisplayFrame, hwc_rect_t>);
    case HWC2::FunctionDescriptor::SetLayerPlaneAlpha:
      return ToHook<HWC2_PFN_SET_LAYER_PLANE_ALPHA>(
          LayerHook<decltype(&HwcLayer::SetLayerPlaneAlpha),
                    &HwcLayer::SetLayerPlaneAlpha, float>);
    case HWC2::FunctionDescriptor::SetLayerSidebandStream:
      return ToHook<HWC2_PFN_SET_LAYER_SIDEBAND_STREAM>(LayerHook<
          decltype(&HwcLayer::SetLayerSidebandStream),
          &HwcLayer::SetLayerSidebandStream, const native_handle_t *>);
    case HWC2::FunctionDescriptor::SetLayerSourceCrop:
      return ToHook<HWC2_PFN_SET_LAYER_SOURCE_CROP>(
          LayerHook<decltype(&HwcLayer::SetLayerSourceCrop),
                    &HwcLayer::SetLayerSourceCrop, hwc_frect_t>);
    case HWC2::FunctionDescriptor::SetLayerSurfaceDamage:
      return ToHook<HWC2_PFN_SET_LAYER_SURFACE_DAMAGE>(
          LayerHook<decltype(&HwcLayer::SetLayerSurfaceDamage),
                    &HwcLayer::SetLayerSurfaceDamage, hwc_region_t>);
    case HWC2::FunctionDescriptor::SetLayerTransform:
      return ToHook<HWC2_PFN_SET_LAYER_TRANSFORM>(
          LayerHook<decltype(&HwcLayer::SetLayerTransform),
                    &HwcLayer::SetLayerTransform, int32_t>);
    case HWC2::FunctionDescriptor::SetLayerVisibleRegion:
      return ToHook<HWC2_PFN_SET_LAYER_VISIBLE_REGION>(
          LayerHook<decltype(&HwcLayer::SetLayerVisibleRegion),
                    &HwcLayer::SetLayerVisibleRegion, hwc_region_t>);
    case HWC2::FunctionDescriptor::SetLayerZOrder:
      return ToHook<HWC2_PFN_SET_LAYER_Z_ORDER>(
          LayerHook<decltype(&HwcLayer::SetLayerZOrder),
                    &HwcLayer::SetLayerZOrder, uint32_t>);
    case HWC2::FunctionDescriptor::Invalid:
    default:
      return NULL;
  }
}

// static
int IAHWC2::HookDevOpen(const struct hw_module_t *module, const char *name,
                        struct hw_device_t **dev) {
  if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
    ALOGE("Invalid module name- %s", name);
    return -EINVAL;
  }

  std::unique_ptr<IAHWC2> ctx(new IAHWC2());
  if (!ctx) {
    ALOGE("Failed to allocate IAHWC2");
    return -ENOMEM;
  }

  HWC2::Error err = ctx->Init();
  if (err != HWC2::Error::None) {
    ALOGE("Failed to initialize IAHWC2 err=%d\n", err);
    return -EINVAL;
  }

  ctx->common.module = const_cast<hw_module_t *>(module);
  *dev = &ctx->common;
  ctx.release();
  return 0;
}

}  // namespace android

static struct hw_module_methods_t iahwc2_module_methods = {
    .open = android::IAHWC2::HookDevOpen,
};

hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(2, 0),
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "IAHWC2 module",
    .author = "The Android Open Source Project",
    .methods = &iahwc2_module_methods,
    .dso = NULL,
    .reserved = {0},
};
// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>

#include "services/device/public/mojom/sensor.mojom-blink.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/modules/device_orientation/device_orientation_event_pump.h"

namespace {

bool IsAngleDifferentThreshold(bool has_angle1,
                               double angle1,
                               bool has_angle2,
                               double angle2) {
  if (has_angle1 != has_angle2)
    return true;

  return (has_angle1 &&
          std::fabs(angle1 - angle2) >=
              blink::DeviceOrientationEventPump::kOrientationThreshold);
}

bool IsSignificantlyDifferent(const device::OrientationData& data1,
                              const device::OrientationData& data2) {
  return IsAngleDifferentThreshold(data1.has_alpha, data1.alpha,
                                   data2.has_alpha, data2.alpha) ||
         IsAngleDifferentThreshold(data1.has_beta, data1.beta, data2.has_beta,
                                   data2.beta) ||
         IsAngleDifferentThreshold(data1.has_gamma, data1.gamma,
                                   data2.has_gamma, data2.gamma);
}

}  // namespace

namespace blink {

template class DeviceSensorEventPump<blink::WebDeviceOrientationListener>;

const double DeviceOrientationEventPump::kOrientationThreshold = 0.1;

DeviceOrientationEventPump::DeviceOrientationEventPump(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    bool absolute)
    : DeviceSensorEventPump<blink::WebDeviceOrientationListener>(task_runner),
      relative_orientation_sensor_(
          this,
          device::mojom::SensorType::RELATIVE_ORIENTATION_EULER_ANGLES),
      absolute_orientation_sensor_(
          this,
          device::mojom::SensorType::ABSOLUTE_ORIENTATION_EULER_ANGLES),
      absolute_(absolute),
      fall_back_to_absolute_orientation_sensor_(!absolute) {}

DeviceOrientationEventPump::~DeviceOrientationEventPump() {
  StopIfObserving();
}

void DeviceOrientationEventPump::SendStartMessage(LocalFrame* frame) {
  if (!sensor_provider_) {
    DCHECK(frame);

    frame->GetInterfaceProvider().GetInterface(
        mojo::MakeRequest(&sensor_provider_));
    sensor_provider_.set_connection_error_handler(
        WTF::Bind(&DeviceSensorEventPump::HandleSensorProviderError,
                  WTF::Unretained(this)));
  }

  if (absolute_) {
    absolute_orientation_sensor_.Start(sensor_provider_.get());
  } else {
    fall_back_to_absolute_orientation_sensor_ = true;
    should_suspend_absolute_orientation_sensor_ = false;
    relative_orientation_sensor_.Start(sensor_provider_.get());
  }
}

void DeviceOrientationEventPump::SendStopMessage() {
  // SendStopMessage() gets called both when the page visibility changes and if
  // all device orientation event listeners are unregistered. Since removing
  // the event listener is more rare than the page visibility changing,
  // Sensor::Suspend() is used to optimize this case for not doing extra work.

  relative_orientation_sensor_.Stop();
  // This is needed in case we fallback to using the absolute orientation
  // sensor. In this case, the relative orientation sensor is marked as
  // SensorState::SHOULD_SUSPEND, and if the relative orientation sensor
  // is not available, the absolute orientation sensor should also be marked as
  // SensorState::SHOULD_SUSPEND, but only after the
  // absolute_orientation_sensor_.Start() is called for initializing
  // the absolute orientation sensor in
  // DeviceOrientationEventPump::DidStartIfPossible().
  if (relative_orientation_sensor_.sensor_state ==
          SensorState::SHOULD_SUSPEND &&
      fall_back_to_absolute_orientation_sensor_) {
    should_suspend_absolute_orientation_sensor_ = true;
  }

  absolute_orientation_sensor_.Stop();

  // Reset the cached data because DeviceOrientationDispatcher resets its
  // data when stopping. If we don't reset here as well, then when starting back
  // up we won't notify DeviceOrientationDispatcher of the orientation, since
  // we think it hasn't changed.
  data_ = device::OrientationData();
}

void DeviceOrientationEventPump::FireEvent(TimerBase*) {
  device::OrientationData data;

  DCHECK(listener());

  GetDataFromSharedMemory(&data);

  if (ShouldFireEvent(data)) {
    data_ = data;
    listener()->DidChangeDeviceOrientation(data);
  }
}

void DeviceOrientationEventPump::DidStartIfPossible() {
  if (!absolute_ && !relative_orientation_sensor_.sensor &&
      fall_back_to_absolute_orientation_sensor_ && sensor_provider_) {
    // When relative orientation sensor is not available fall back to using
    // the absolute orientation sensor but only on the first failure.
    fall_back_to_absolute_orientation_sensor_ = false;
    absolute_orientation_sensor_.Start(sensor_provider_.get());
    if (should_suspend_absolute_orientation_sensor_) {
      // The absolute orientation sensor needs to be marked as
      // SensorState::SUSPENDED when it is successfully initialized.
      absolute_orientation_sensor_.sensor_state = SensorState::SHOULD_SUSPEND;
      should_suspend_absolute_orientation_sensor_ = false;
    }
    return;
  }
  DeviceSensorEventPump::DidStartIfPossible();
}

bool DeviceOrientationEventPump::SensorsReadyOrErrored() const {
  if (!relative_orientation_sensor_.ReadyOrErrored() ||
      !absolute_orientation_sensor_.ReadyOrErrored()) {
    return false;
  }

  // At most one sensor can be successfully initialized.
  DCHECK(!relative_orientation_sensor_.sensor ||
         !absolute_orientation_sensor_.sensor);

  return true;
}

void DeviceOrientationEventPump::GetDataFromSharedMemory(
    device::OrientationData* data) {
  data->all_available_sensors_are_active = true;

  if (!absolute_ && relative_orientation_sensor_.SensorReadingCouldBeRead()) {
    // For DeviceOrientation Event, this provides relative orientation data.
    data->all_available_sensors_are_active =
        relative_orientation_sensor_.reading.timestamp() != 0.0;
    if (!data->all_available_sensors_are_active)
      return;
    data->alpha = relative_orientation_sensor_.reading.orientation_euler.z;
    data->beta = relative_orientation_sensor_.reading.orientation_euler.x;
    data->gamma = relative_orientation_sensor_.reading.orientation_euler.y;
    data->has_alpha = !std::isnan(
        relative_orientation_sensor_.reading.orientation_euler.z.value());
    data->has_beta = !std::isnan(
        relative_orientation_sensor_.reading.orientation_euler.x.value());
    data->has_gamma = !std::isnan(
        relative_orientation_sensor_.reading.orientation_euler.y.value());
    data->absolute = false;
  } else if (absolute_orientation_sensor_.SensorReadingCouldBeRead()) {
    // For DeviceOrientationAbsolute Event, this provides absolute orientation
    // data.
    //
    // For DeviceOrientation Event, this provides absolute orientation data if
    // relative orientation data is not available.
    data->all_available_sensors_are_active =
        absolute_orientation_sensor_.reading.timestamp() != 0.0;
    if (!data->all_available_sensors_are_active)
      return;
    data->alpha = absolute_orientation_sensor_.reading.orientation_euler.z;
    data->beta = absolute_orientation_sensor_.reading.orientation_euler.x;
    data->gamma = absolute_orientation_sensor_.reading.orientation_euler.y;
    data->has_alpha = !std::isnan(
        absolute_orientation_sensor_.reading.orientation_euler.z.value());
    data->has_beta = !std::isnan(
        absolute_orientation_sensor_.reading.orientation_euler.x.value());
    data->has_gamma = !std::isnan(
        absolute_orientation_sensor_.reading.orientation_euler.y.value());
    data->absolute = true;
  } else {
    data->absolute = absolute_;
  }
}

bool DeviceOrientationEventPump::ShouldFireEvent(
    const device::OrientationData& data) const {
  if (!data.all_available_sensors_are_active)
    return false;

  if (!data.has_alpha && !data.has_beta && !data.has_gamma) {
    // no data can be provided, this is an all-null event.
    return true;
  }

  return IsSignificantlyDifferent(data_, data);
}

}  // namespace blink
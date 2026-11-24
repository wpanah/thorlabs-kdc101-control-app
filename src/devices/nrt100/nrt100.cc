#include "nrt100/nrt100.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "Thorlabs.MotionControl.Benchtop.StepperMotor.h"
#include "error/error.h"

namespace thorlabs {
using errors::Error;
namespace {
using Direction = NRT100::Direction;
using StopMode = NRT100::StopMode;
using JogMode = NRT100::JogMode;
using VelocityParameters = NRT100::VelocityParameters;
using MotorConfiguration = NRT100::MotorConfiguration;
using MotorStatus = NRT100::MotorStatus;
using MotorEvent = NRT100::MotorEvent;
}  // namespace

namespace {

// Acquisition and release are serialized across axis worker threads. Keeping
// channel ownership until cleanup completes prevents a reconnect racing
// teardown.
std::mutex controller_mutex;
std::map<std::string, std::set<short>> controller_channels;

Error OpenDevice(const std::string& serial_number, short channel) {
  std::lock_guard lock(controller_mutex);
  auto existing = controller_channels.find(serial_number);
  if (existing != controller_channels.end()) {
    if (!existing->second.insert(channel).second)
      return Error::InvalidArgument(
          "Controller channel already belongs to another axis.");
    return Error::Ok();
  }
  auto list_status = Error::FromKinesis(TLI_BuildDeviceList());
  if (!list_status.ok()) return list_status.WithContext("TLI_BuildDeviceList");
  Error open_status = Error::FromKinesis(SBC_Open(serial_number.c_str()));
  if (!open_status.ok()) {
    return open_status.WithContext("SBC_Open");
  }
  controller_channels[serial_number].insert(channel);

  return Error::Ok();
}

Error LoadSettings(const std::string& serial_number, short channel,
                   NRT100::StageModel stage) {
  const char* profile = stage == NRT100::StageModel::kNrt150M
                            ? "HS NRT150/M Enc Stage 150mm"
                            : "HS NRT100/M Enc Stage 100mm";
  // Use the known actuator profile, not a previously saved stage selection.
  if (!SBC_LoadNamedSettings(serial_number.c_str(), channel, profile)) {
    return Error::FailedToLoadSettings(serial_number)
        .WithContext(std::string("SBC_LoadNamedSettings: ") + profile);
  }

  return Error::Ok();
}

Error EnableChannel(const std::string& serial_number, short channel) {
  Error channel_status =
      Error::FromKinesis(SBC_EnableChannel(serial_number.c_str(), channel));
  if (!channel_status.ok()) {
    return channel_status.WithContext("SBC_EnableChannel");
  }

  return Error::Ok();
}

Error StartPolling(const std::string& serial_number, short channel,
                   int polling_interval_ms) {
  // Start the device polling
  if (!SBC_StartPolling(serial_number.c_str(), channel, polling_interval_ms)) {
    return Error::FailedToStartPolling(serial_number)
        .WithContext("SBC_StartPolling");
  }

  return Error::Ok();
}

}  // namespace

NRT100::NRT100(std::string serial_number, short channel,
               std::shared_ptr<const KinesisSimulation> simulation,
               StageModel stage)
    : serial_number_{serial_number},
      channel_{channel},
      stage_{stage},
      simulation_{std::move(simulation)},
      connected_{false},
      polling_{false},
      channel_enabled_{false} {}

NRT100::CreateResult NRT100::CreateMotor(
    std::string serial_number, short channel, int polling_interval_ms,
    std::shared_ptr<const KinesisSimulation> simulation, StageModel stage,
    std::optional<TravelLimits> limits) {
  auto device = std::unique_ptr<NRT100>(
      new NRT100(serial_number, channel, std::move(simulation), stage));

  if (stage != StageModel::kNrt100M && stage != StageModel::kNrt150M) {
    return std::unexpected(Error::InvalidArgument("Unknown NRT stage model."));
  }
  if (serial_number.empty() || channel < 1 || polling_interval_ms <= 0) {
    return std::unexpected(Error::InvalidArgument(
        "Serial, channel and polling interval must be valid."));
  }
  auto status = OpenDevice(serial_number, channel);
  if (!status.ok()) return std::unexpected(status);
  device->connected_ = true;
  if (channel > SBC_GetNumChannels(serial_number.c_str())) {
    return std::unexpected(
        Error::InvalidArgument("Controller channel does not exist."));
  }

  // Load Settings
  status = LoadSettings(serial_number, channel, stage);
  if (!status.ok()) {
    return std::unexpected(status);
  }

  // Reserve two feedback intervals to decelerate near a software limit.
  device->approach_time_s_ = 2.0 * polling_interval_ms / 1000;
  // Apply the saved travel range before enabling the channel.
  status = device->SetTravelLimits(
      limits.value_or(TravelLimits{0, device->GetTravelMm()}));
  if (!status.ok()) return std::unexpected(status);
  status = EnableChannel(serial_number, channel);
  if (!status.ok()) {
    return std::unexpected(status);
  }

  device->channel_enabled_ = true;

  // Start Polling
  status = StartPolling(serial_number, channel, polling_interval_ms);
  if (!status.ok()) {
    return std::unexpected(status);
  }

  device->polling_ = true;

  // Keep the stage-specific velocity, home and jog settings from Kinesis.
  return device;
}

NRT100::~NRT100() {
  std::lock_guard lock(controller_mutex);
  if (channel_enabled_) {
    SBC_ClearMessageQueue(serial_number_.c_str(), channel_);
    SBC_StopProfiled(serial_number_.c_str(),
                     channel_);  // No point waiting for stop to finish
  }

  if (channel_enabled_) {
    SBC_DisableChannel(serial_number_.c_str(), channel_);
  }

  if (polling_) {
    SBC_StopPolling(serial_number_.c_str(), channel_);
  }

  if (connected_) {
    auto owner = controller_channels.find(serial_number_);
    if (owner != controller_channels.end()) {
      owner->second.erase(channel_);
      if (owner->second.empty()) {
        SBC_Close(serial_number_.c_str());
        controller_channels.erase(owner);
      }
    }
  }
}

namespace {

constexpr int kUnitTypeDistance = 0;
constexpr int kUnitTypeSpeed = 1;
constexpr int kUnitTypeAcceleration = 2;

std::expected<int, Error> ConvertMotionParameter(
    const std::string& serial_number, short channel, double value,
    int unit_type) {
  if (!std::isfinite(value) || value <= 0.0) {
    return std::unexpected(Error::FromKinesis(FT_InvalidParameter));
  }
  int device_units = 0;
  auto status = Error::FromKinesis(SBC_GetDeviceUnitFromRealValue(
      serial_number.c_str(), channel, value, &device_units, unit_type));

  if (!status.ok()) {
    return std::unexpected(status);
  }

  if (device_units <= 0) {
    return std::unexpected(Error::FromKinesis(FT_InvalidParameter));
  }

  return device_units;
}

struct StageMotionLimits {
  double speed;
  double acceleration;
};

std::expected<StageMotionLimits, Error> ReadStageMotionLimits(
    const std::string& serial_number, short channel) {
  StageMotionLimits limits{};
  auto status = Error::FromKinesis(SBC_GetMotorVelocityLimits(
      serial_number.c_str(), channel, &limits.speed, &limits.acceleration));
  if (!status.ok()) return std::unexpected(status);
  if (!(std::isfinite(limits.speed) && limits.speed > 0 &&
        std::isfinite(limits.acceleration) && limits.acceleration > 0))
    return std::unexpected(
        Error::InvalidArgument("Cannot verify the stage motion limits."));
  return limits;
}

Error CheckVelocityLimits(const std::string& serial_number, short channel,
                          VelocityParameters update) {
  auto limits = ReadStageMotionLimits(serial_number, channel);
  if (!limits) return limits.error();
  if (update.speed_mm_per_second && *update.speed_mm_per_second > limits->speed)
    return Error::InvalidArgument(std::format(
        "Speed exceeds the stage maximum of {} mm/s.", limits->speed));
  if (update.acceleration_mm_per_second_squared &&
      *update.acceleration_mm_per_second_squared > limits->acceleration)
    return Error::InvalidArgument(
        std::format("Acceleration exceeds the stage maximum of {} mm/s².",
                    limits->acceleration));
  return Error::Ok();
}

Error ApplyVelocity(const std::string& serial_number, short channel,
                    VelocityParameters update, bool jog) {
  if (!update.speed_mm_per_second && !update.acceleration_mm_per_second_squared)
    return Error::Ok();
  auto limits = CheckVelocityLimits(serial_number, channel, update);
  if (!limits.ok()) return limits;
  std::optional<int> speed, acceleration;
  if (update.speed_mm_per_second) {
    auto converted = ConvertMotionParameter(
        serial_number, channel, *update.speed_mm_per_second, kUnitTypeSpeed);
    if (!converted) return converted.error();
    speed = *converted;
  }
  if (update.acceleration_mm_per_second_squared) {
    auto converted = ConvertMotionParameter(
        serial_number, channel, *update.acceleration_mm_per_second_squared,
        kUnitTypeAcceleration);
    if (!converted) return converted.error();
    acceleration = *converted;
  }
  int device_speed = 0, device_acceleration = 0;
  const char* serial = serial_number.c_str();
  auto status = Error::FromKinesis(
      jog ? SBC_GetJogVelParams(serial, channel, &device_acceleration,
                                &device_speed)
          : SBC_GetVelParams(serial, channel, &device_acceleration,
                             &device_speed));
  if (!status.ok()) return status;
  // Explicit updates must reach the controller even if the SDK's cached
  // parameters already match. Preserve only fields omitted by the caller.
  return Error::FromKinesis(
      jog ? SBC_SetJogVelParams(serial, channel,
                                acceleration.value_or(device_acceleration),
                                speed.value_or(device_speed))
          : SBC_SetVelParams(serial, channel,
                             acceleration.value_or(device_acceleration),
                             speed.value_or(device_speed)));
}

}  // namespace

NRT100::PositionResult NRT100::GetPosition() {
  auto connection = CheckConnection();
  if (!connection.ok()) return std::unexpected(connection);
  int device_unit = SBC_GetPosition(serial_number_.c_str(), channel_);

  double position_real;
  Error conversion_status = Error::FromKinesis(SBC_GetRealValueFromDeviceUnit(
      serial_number_.c_str(), channel_, device_unit, &position_real,
      kUnitTypeDistance));

  if (!conversion_status.ok()) {
    return std::unexpected(conversion_status);
  }

  return position_real;
}

Error NRT100::SetMoveVelocity(VelocityParameters parameters) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  if (parameters.speed_mm_per_second ||
      parameters.acceleration_mm_per_second_squared) {
    auto restored = RestoreMoveProfile();
    if (!restored.ok()) return restored;
  }
  return ApplyVelocity(serial_number_, channel_, parameters, false);
}

std::expected<MotorConfiguration, Error> NRT100::GetConfiguration() {
  auto status = CheckConnection();
  if (!status.ok()) return std::unexpected(status);
  int acceleration = 0, speed = 0;
  status = Error::FromKinesis(SBC_GetVelParams(serial_number_.c_str(), channel_,
                                               &acceleration, &speed));
  if (!status.ok()) return std::unexpected(status);
  MotorConfiguration configuration{};
  auto limits = ReadStageMotionLimits(serial_number_, channel_);
  if (!limits) return std::unexpected(limits.error());
  const auto read = [this](int units, int type, double& value,
                           std::optional<double> maximum = std::nullopt) {
    auto result = Error::FromKinesis(SBC_GetRealValueFromDeviceUnit(
        serial_number_.c_str(), channel_, units, &value, type));
    // Quantizing the exact maximum can yield a readback slightly above it.
    // Display the maximum if it reproduces the controller's exact count.
    if (result.ok() && maximum && value > *maximum) {
      auto maximum_units =
          ConvertMotionParameter(serial_number_, channel_, *maximum, type);
      if (!maximum_units) return maximum_units.error();
      if (units == *maximum_units) value = *maximum;
    }
    return result;
  };
  status =
      read(original_profile_ ? original_profile_->speed : speed, kUnitTypeSpeed,
           configuration.move_speed_mm_per_second, limits->speed);
  if (!status.ok()) return std::unexpected(status);
  status = read(SBC_GetJogStepSize(serial_number_.c_str(), channel_),
                kUnitTypeDistance, configuration.jog_step_mm);
  if (!status.ok()) return std::unexpected(status);
  status = read(
      original_profile_ ? original_profile_->acceleration : acceleration,
      kUnitTypeAcceleration, configuration.acceleration_mm_per_second_squared,
      limits->acceleration);
  if (!status.ok()) return std::unexpected(status);
  status = read(SBC_GetHomingVelocity(serial_number_.c_str(), channel_),
                kUnitTypeSpeed, configuration.homing_speed_mm_per_second,
                limits->speed);
  if (!status.ok()) return std::unexpected(status);
  status = read(configured_backlash_.value_or(
                    SBC_GetBacklash(serial_number_.c_str(), channel_)),
                kUnitTypeDistance, configuration.backlash_mm);
  if (!status.ok()) return std::unexpected(status);
  return configuration;
}

NRT100::PositionResult NRT100::GetDistanceResolution() {
  auto connection = CheckConnection();
  if (!connection.ok()) return std::unexpected(connection);
  // The rounded mm readback of a single count is too coarse for this ratio.
  auto units =
      ConvertMotionParameter(serial_number_, channel_, 1.0, kUnitTypeDistance);
  if (!units) return std::unexpected(units.error());
  return 1.0 / *units;
}

NRT100::PositionResult NRT100::GetApproachDistance() {
  int acceleration = 0, speed = 0;
  auto status = Error::FromKinesis(SBC_GetVelParams(
      serial_number_.c_str(), channel_, &acceleration, &speed));
  if (!status.ok()) return std::unexpected(status);
  double mm_per_second = 0;
  status = Error::FromKinesis(SBC_GetRealValueFromDeviceUnit(
      serial_number_.c_str(), channel_,
      original_profile_ ? original_profile_->speed : speed, &mm_per_second,
      kUnitTypeSpeed));
  if (!status.ok()) return std::unexpected(status);
  return mm_per_second * approach_time_s_;
}

Error NRT100::SetTravelLimits(TravelLimits limits) {
  if (!std::isfinite(limits.minimum_mm) || !std::isfinite(limits.maximum_mm) ||
      limits.minimum_mm >= limits.maximum_mm) {
    return Error::InvalidArgument("Minimum must be less than maximum.");
  }
  if (limits.minimum_mm < 0 || limits.maximum_mm > GetTravelMm()) {
    return Error::InvalidArgument(
        "Software travel limits must be within the stage travel.");
  }
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  const char* serial = serial_number_.c_str();
  auto convert = [&](double mm, int& counts) {
    return Error::FromKinesis(SBC_GetDeviceUnitFromRealValue(
        serial, channel_, mm, &counts, kUnitTypeDistance));
  };
  int low = 0, high = 0, counts_per_mm = 0;
  auto status = convert(limits.minimum_mm, low);
  if (!status.ok()) return status;
  status = convert(limits.maximum_mm, high);
  if (!status.ok()) return status;
  status = convert(1.0, counts_per_mm);
  if (!status.ok()) return status;
  if (counts_per_mm <= 0) return Error::FromKinesis(FT_InvalidParameter);
  auto speed_scale =
      ConvertMotionParameter(serial_number_, channel_, 1.0, kUnitTypeSpeed);
  if (!speed_scale) return speed_scale.error();
  auto acceleration_scale = ConvertMotionParameter(serial_number_, channel_,
                                                   1.0, kUnitTypeAcceleration);
  if (!acceleration_scale) return acceleration_scale.error();
  // The supported NRT stages have an integral number of counts per mm.
  // Correct outward rounding when an endpoint is between two counts.
  if (low < limits.minimum_mm * counts_per_mm) ++low;
  if (high > limits.maximum_mm * counts_per_mm) --high;
  if (low >= high)
    return Error::InvalidArgument(
        "Software travel limits are too close for the stage resolution.");
  MOT_LimitSwitchParameters switches{};
  status = Error::FromKinesis(
      SBC_GetLimitSwitchParamsBlock(serial, channel_, &switches));
  if (!status.ok()) return status.WithContext("Read limit switches");
  // Preserve the stage profile's physical switch configuration. The SDK's
  // channel API expresses both software switch positions in device units.
  switches.anticlockwisePosition = static_cast<DWORD>(low);
  switches.clockwisePosition = static_cast<DWORD>(high);
  switches.softLimitMode = MOT_LimitSwitchStopImmediate;
  status = Error::FromKinesis(
      SBC_SetLimitSwitchParamsBlock(serial, channel_, &switches));
  if (!status.ok()) {
    limits_.reset();
    return status.WithContext("Set software limit switches");
  }
  MOT_LimitSwitchParameters confirmed{};
  status = Error::FromKinesis(
      SBC_GetLimitSwitchParamsBlock(serial, channel_, &confirmed));
  if (!status.ok() ||
      confirmed.anticlockwisePosition != switches.anticlockwisePosition ||
      confirmed.clockwisePosition != switches.clockwisePosition ||
      confirmed.softLimitMode != switches.softLimitMode ||
      confirmed.clockwiseHardwareLimit != switches.clockwiseHardwareLimit ||
      confirmed.anticlockwiseHardwareLimit !=
          switches.anticlockwiseHardwareLimit) {
    limits_.reset();
    return Error::InvalidArgument(
        "Kinesis did not confirm the limit switch settings. Reapply the limits "
        "before moving.");
  }
  SBC_SetLimitsSoftwareApproachPolicy(serial, channel_, DisallowIllegalMoves);
  auto result =
      Error::FromKinesis(SBC_SetStageAxisLimits(serial, channel_, low, high));
  const int actual_low = SBC_GetStageAxisMinPos(serial, channel_);
  const int actual_high = SBC_GetStageAxisMaxPos(serial, channel_);
  const bool rejecting =
      SBC_GetSoftLimitMode(serial, channel_) == DisallowIllegalMoves;
  if (!result.ok() || !rejecting || actual_low != low || actual_high != high) {
    limits_.reset();
    if (!result.ok()) return result.WithContext("Set software travel limits");
    return Error::InvalidArgument(
        "Kinesis did not confirm the software travel limits. Reapply them "
        "before moving.");
  }
  minimum_units_ = low;
  maximum_units_ = high;
  // Position entries and the readout resolve 0.001 mm. Keep boundary targets
  // one quarter of that increment inside the limit to allow final settling.
  constexpr double kEndpointMarginMm = 0.00025;
  endpoint_margin_units_ =
      std::min(static_cast<int>(std::ceil(counts_per_mm * kEndpointMarginMm)),
               (high - low - 1) / 2);
  velocity_units_per_count_per_second_ =
      static_cast<double>(*speed_scale) / counts_per_mm;
  acceleration_units_per_count_per_second_squared_ =
      static_cast<double>(*acceleration_scale) / counts_per_mm;
  limits_ = limits;
  return Error::Ok();
}

Error NRT100::CheckReferenced() const {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  if (!limits_)
    return Error::InvalidArgument(
        "Apply software travel limits before moving.");
  if ((SBC_GetStatusBits(serial_number_.c_str(), channel_) & 0x00000400u) == 0)
    return Error::InvalidArgument(
        "Home the axis before moving within the software travel limits.");
  return Error::Ok();
}

std::expected<int, Error> NRT100::TargetUnits(double position_mm) {
  auto status = CheckReferenced();
  if (!status.ok()) return std::unexpected(status);
  if (!limits_->Contains(position_mm))
    return std::unexpected(Error::InvalidArgument(
        "Target is outside the applied software travel limits."));
  int units = 0;
  status = Error::FromKinesis(
      SBC_GetDeviceUnitFromRealValue(serial_number_.c_str(), channel_,
                                     position_mm, &units, kUnitTypeDistance));
  if (!status.ok()) return std::unexpected(status);
  return std::clamp(units, minimum_units_ + endpoint_margin_units_,
                    maximum_units_ - endpoint_margin_units_);
}

std::expected<bool, Error> NRT100::IsAtPosition(double position_mm) {
  auto target = TargetUnits(position_mm);
  if (!target) return std::unexpected(target.error());
  return SBC_GetPosition(serial_number_.c_str(), channel_) == *target;
}

Error NRT100::SetJogVelocity(VelocityParameters parameters) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  return ApplyVelocity(serial_number_, channel_, parameters, true);
}

Error NRT100::SetHomingSpeed(double speed) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  auto limits = CheckVelocityLimits(serial_number_, channel_,
                                    {.speed_mm_per_second = speed});
  if (!limits.ok()) return limits;
  auto converted =
      ConvertMotionParameter(serial_number_, channel_, speed, kUnitTypeSpeed);
  if (!converted) {
    return converted.error();
  }

  return Error::FromKinesis(SBC_SetHomingVelocity(
      serial_number_.c_str(), channel_, static_cast<unsigned int>(*converted)));
}

Error NRT100::SetBacklash(double distance_mm) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  if (!std::isfinite(distance_mm) || distance_mm < 0.0) {
    return Error::InvalidArgument(
        "Backlash must be a finite, non-negative distance in mm.");
  }
  int units = 0;
  if (distance_mm > 0) {
    auto converted = ConvertMotionParameter(serial_number_, channel_,
                                            distance_mm, kUnitTypeDistance);
    if (!converted) return converted.error();
    units = *converted;
  }
  auto result = Error::FromKinesis(
      SBC_SetBacklash(serial_number_.c_str(), channel_, units));
  if (result.ok()) configured_backlash_.reset();
  return result;
}

Error NRT100::LimitBacklashForTarget(int target_units) {
  const char* serial = serial_number_.c_str();
  const long current = SBC_GetBacklash(serial, channel_);
  const long requested = configured_backlash_.value_or(current);
  // A compensated move may pass its target and then approach it again.
  const long allowed =
      std::clamp<long>(requested, (target_units - maximum_units_) / 2,
                       (target_units - minimum_units_) / 2);
  if (current != allowed) {
    auto result =
        Error::FromKinesis(SBC_SetBacklash(serial, channel_, allowed));
    if (!result.ok()) return result.WithContext("Limit backlash for move");
    configured_backlash_ = requested;
    if (SBC_GetBacklash(serial, channel_) != allowed)
      return Error::InvalidArgument(
          "Kinesis did not confirm the backlash correction for this move.");
  }
  if (allowed == requested) configured_backlash_.reset();
  return Error::Ok();
}

Error NRT100::SetJogStepSize(double step) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  if (step > GetTravelMm())
    return Error::InvalidArgument(
        "Jog step exceeds the selected stage travel.");
  auto converted =
      ConvertMotionParameter(serial_number_, channel_, step, kUnitTypeDistance);
  if (!converted) {
    return converted.error();
  }
  return Error::FromKinesis(SBC_SetJogStepSize(
      serial_number_.c_str(), channel_, static_cast<unsigned int>(*converted)));
}

Error NRT100::SetJogMode(JogMode mode, StopMode stop_mode) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  if ((mode != JogMode::kSingleStep && mode != JogMode::kContinuous) ||
      (stop_mode != StopMode::kProfiled && stop_mode != StopMode::kImmediate)) {
    return Error::FromKinesis(FT_InvalidParameter);
  }
  return Error::FromKinesis(SBC_SetJogMode(
      serial_number_.c_str(), channel_,
      mode == JogMode::kSingleStep ? MOT_JogModes::MOT_SingleStep
                                   : MOT_JogModes::MOT_Continuous,
      stop_mode == StopMode::kProfiled ? MOT_StopModes::MOT_Profiled
                                       : MOT_StopModes::MOT_Immediate));
}

Error NRT100::StartHome() {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  auto restored = RestoreMoveProfile();
  if (!restored.ok()) return restored;
  return Error::FromKinesis(SBC_Home(serial_number_.c_str(), channel_));
}

Error NRT100::StartJog(Direction dir) {
  auto connection = CheckReferenced();
  if (!connection.ok()) return connection;
  if (dir != Direction::kForward && dir != Direction::kBackward)
    return Error::FromKinesis(FT_InvalidParameter);
  const char* serial = serial_number_.c_str();
  MOT_JogModes mode;
  MOT_StopModes stop;
  auto status =
      Error::FromKinesis(SBC_GetJogMode(serial, channel_, &mode, &stop));
  if (!status.ok()) return status;
  if (mode != MOT_SingleStep)
    return Error::InvalidArgument(
        "Jog requires single-step mode with software travel limits.");
  const std::int64_t step = SBC_GetJogStepSize(serial, channel_);
  const std::int64_t current = SBC_GetPosition(serial, channel_);
  const bool forward = dir == Direction::kForward;
  const std::int64_t target = current + (forward ? step : -step);
  // The position and jog step are rounded separately. At a fractional-mm
  // boundary the nominal endpoint can differ by one count.
  const auto bounded =
      std::clamp<std::int64_t>(target, minimum_units_, maximum_units_);
  if (step == 0 || std::abs(bounded - target) > 1 || bounded == current)
    return Error::InvalidArgument(
        "Jog target is outside the applied software travel limits.");

  const int destination = std::clamp(static_cast<int>(bounded),
                                     minimum_units_ + endpoint_margin_units_,
                                     maximum_units_ - endpoint_margin_units_);
  if (forward ? destination <= current : destination >= current)
    return Error::InvalidArgument("Jog is at the software travel limit.");
  return StartPositionedMove(destination, true);
}

Error NRT100::StartDrive(Direction dir) {
  auto connection = CheckReferenced();
  if (!connection.ok()) return connection;
  if (dir != Direction::kForward && dir != Direction::kBackward)
    return Error::FromKinesis(FT_InvalidParameter);

  const int current = SBC_GetPosition(serial_number_.c_str(), channel_);
  const bool forward = dir == Direction::kForward;
  if (forward ? current >= maximum_units_ - endpoint_margin_units_
              : current <= minimum_units_ + endpoint_margin_units_)
    return Error::InvalidArgument(
        "Drive is at or beyond the software travel limit in this direction.");
  return StartMoveAbsolute(forward ? limits_->maximum_mm : limits_->minimum_mm);
}

Error NRT100::Stop(StopMode stop_mode) {
  auto connection = CheckConnection();
  if (!connection.ok()) return connection;
  if (stop_mode != StopMode::kProfiled && stop_mode != StopMode::kImmediate)
    return Error::InvalidArgument("Invalid stop mode.");
  approach_.reset();
  if (stop_mode == StopMode::kProfiled) {
    return Error::FromKinesis(
        SBC_StopProfiled(serial_number_.c_str(), channel_));
  }

  return Error::FromKinesis(
      SBC_StopImmediate(serial_number_.c_str(), channel_));
}

Error NRT100::StartMoveAbsolute(double position) {
  auto target = TargetUnits(position);
  if (!target) return target.error();
  return StartPositionedMove(*target);
}

Error NRT100::RestoreMoveProfile() {
  if (!original_profile_) return Error::Ok();
  auto result = Error::FromKinesis(SBC_SetVelParams(
      serial_number_.c_str(), channel_, original_profile_->acceleration,
      original_profile_->speed));
  if (result.ok()) original_profile_.reset();
  return result;
}

Error NRT100::StartPositionedMove(int target, bool jog) {
  auto result = RestoreMoveProfile();
  if (!result.ok()) return result;
  const char* serial = serial_number_.c_str();
  VelocityProfile profile{};
  result = Error::FromKinesis(
      jog ? SBC_GetJogVelParams(serial, channel_, &profile.acceleration,
                                &profile.speed)
          : SBC_GetVelParams(serial, channel_, &profile.acceleration,
                             &profile.speed));
  if (!result.ok()) return result;
  if (profile.speed <= 0 || profile.acceleration <= 0)
    return Error::InvalidArgument(
        "Motion requires a positive speed and acceleration.");
  const int current = SBC_GetPosition(serial, channel_);
  approach_ =
      Approach{target, current, profile, std::min(current, minimum_units_),
               std::max(current, maximum_units_)};
  result = StartApproachSegment(current, jog);
  if (!result.ok()) approach_.reset();
  return result;
}

Error NRT100::StartApproachSegment(int from, bool jog) {
  const char* serial = serial_number_.c_str();
  const int target = approach_->target;
  const int sign = target >= from ? 1 : -1;
  const int edge = sign > 0 ? maximum_units_ : minimum_units_;
  const std::int64_t remaining =
      std::abs(static_cast<std::int64_t>(edge) - from);
  const auto profile = approach_->profile;
  // Leave room for following error during deceleration. Only subdivide a
  // move if the requested target is closer to the edge than that allowance.
  const double guard =
      profile.speed / velocity_units_per_count_per_second_ * approach_time_s_;
  // A one-unit acceleration is the slowest ramp the controller can express.
  // Below its ramp distance, finish directly using that minimum ramp.
  const double ramp_counts = approach_time_s_ * approach_time_s_ /
                             acceleration_units_per_count_per_second_squared_;
  const int reserve =
      remaining <= 4 * ramp_counts
          ? 0
          : static_cast<int>(std::min<double>(guard, remaining / 4));
  // A newly applied range may exclude the current position. Even the first
  // waypoint used to re-enter that range must be a legal destination.
  const int waypoint =
      std::clamp(edge - sign * std::max(std::abs(edge - target), reserve),
                 minimum_units_, maximum_units_);
  const int clearance = std::abs(edge - waypoint);
  const double max_speed = std::max<double>(ramp_counts, clearance) *
                           velocity_units_per_count_per_second_ /
                           approach_time_s_;
  const int speed =
      std::max(1, static_cast<int>(std::min<double>(profile.speed, max_speed)));
  const int acceleration =
      speed == profile.speed
          ? profile.acceleration
          : std::max(1,
                     static_cast<int>(std::min<double>(
                         profile.acceleration,
                         speed / velocity_units_per_count_per_second_ *
                             acceleration_units_per_count_per_second_squared_ /
                             approach_time_s_)));
  approach_->waypoint = waypoint;
  auto result = LimitBacklashForTarget(waypoint);
  if (!result.ok()) return result;
  if (jog || speed != profile.speed || original_profile_) {
    if (!original_profile_) {
      VelocityProfile previous{};
      result = Error::FromKinesis(SBC_GetVelParams(
          serial, channel_, &previous.acceleration, &previous.speed));
      if (!result.ok()) return result;
      original_profile_ = previous;
    }
    result = Error::FromKinesis(
        SBC_SetVelParams(serial, channel_, acceleration, speed));
    if (!result.ok()) return result;
  }
  return Error::FromKinesis(SBC_MoveToPosition(serial, channel_, waypoint));
}
Error NRT100::StartMoveRelative(double distance) {
  auto connection = CheckReferenced();
  if (!connection.ok()) return connection;
  if (!std::isfinite(distance) || distance < -GetTravelMm() ||
      distance > GetTravelMm()) {
    return Error::FromKinesis(FT_InvalidParameter);
  }

  auto position = GetPosition();
  if (!position) return position.error();
  const auto target = TargetUnits(*position + distance);
  if (!target) return target.error();
  const int current = SBC_GetPosition(serial_number_.c_str(), channel_);
  if (distance > 0 ? *target <= current : distance == 0 || *target >= current)
    return Error::MotionBelowResolution(serial_number_);
  return StartPositionedMove(*target);
}

std::expected<MotorStatus, Error> NRT100::GetStatus() {
  auto status = CheckConnection();
  if (!status.ok()) {
    return std::unexpected(status);
  }

  const auto bits = SBC_GetStatusBits(serial_number_.c_str(), channel_);
  MotorStatus status_flags;
  status_flags.forward_limit_switch = (bits & 0x00000001u) != 0;
  status_flags.reverse_limit_switch = (bits & 0x00000002u) != 0;
  status_flags.moving_forward = (bits & 0x00000010u) != 0;
  status_flags.moving_reverse = (bits & 0x00000020u) != 0;
  status_flags.jogging_forward = (bits & 0x00000040u) != 0;
  status_flags.jogging_reverse = (bits & 0x00000080u) != 0;
  status_flags.homing = (bits & 0x00000200u) != 0;
  status_flags.homed = (bits & 0x00000400u) != 0;
  status_flags.active = (bits & 0x20000000u) != 0;
  status_flags.channel_enabled = (bits & 0x80000000u) != 0;
  return status_flags;
}

Error NRT100::CheckConnection() const {
  if (!connected_ || !SBC_CheckConnection(serial_number_.c_str())) {
    return Error::NotConnected(serial_number_);
  }
  return Error::Ok();
}

NRT100::EventResult NRT100::GetNextEvent() {
  auto status = CheckConnection();
  if (!status.ok()) {
    return std::unexpected(status);
  }
  if (approach_) {
    const int position = SBC_GetPosition(serial_number_.c_str(), channel_);
    if (position < approach_->observed_minimum ||
        position > approach_->observed_maximum) {
      auto stopped = Stop(StopMode::kImmediate);
      if (!stopped.ok())
        return std::unexpected(
            stopped.WithContext("Travel limit crossed; immediate stop failed"));
      double mm = 0;
      const short converted = SBC_GetRealValueFromDeviceUnit(
          serial_number_.c_str(), channel_, position, &mm, kUnitTypeDistance);
      const std::string readback = converted == 0
                                       ? std::to_string(mm) + " mm"
                                       : std::to_string(position) + " counts";
      return std::unexpected(Error::InvalidArgument(
          "Position crossed a software travel limit at " + readback +
          ". An immediate stop was requested."));
    }
    if (position >= minimum_units_)
      approach_->observed_minimum = minimum_units_;
    if (position <= maximum_units_)
      approach_->observed_maximum = maximum_units_;
  }

  WORD type = 0;
  WORD id = 0;
  DWORD data = 0;
  if (!SBC_GetNextMessage(serial_number_.c_str(), channel_, &type, &id,
                          &data)) {
    status = CheckConnection();
    if (!status.ok()) {
      return std::unexpected(status);
    }
    return std::optional<MotorEvent>{};
  }

  constexpr WORD kGenericMotorMessageType = 2;
  constexpr WORD kHomedMessageId = 0;
  constexpr WORD kMovedMessageId = 1;
  constexpr WORD kStoppedMessageId = 2;

  if (type == kGenericMotorMessageType) {
    switch (id) {
      case kHomedMessageId:
        return MotorEvent::kHomed;
      case kMovedMessageId:
        if (approach_ && approach_->waypoint != approach_->target) {
          const auto next = StartApproachSegment(approach_->waypoint, false);
          if (!next.ok()) {
            approach_.reset();
            return std::unexpected(next);
          }
          return MotorEvent::kOther;
        }
        approach_.reset();
        status = RestoreMoveProfile();
        if (!status.ok()) return std::unexpected(status);
        return MotorEvent::kMoveCompleted;
      case kStoppedMessageId:
        approach_.reset();
        status = RestoreMoveProfile();
        if (!status.ok()) return std::unexpected(status);
        return MotorEvent::kStopped;
    }
  }
  return MotorEvent::kOther;
}

Error NRT100::ClearMessageQueue() {
  auto status = CheckConnection();
  if (!status.ok()) {
    return status;
  }
  SBC_ClearMessageQueue(serial_number_.c_str(), channel_);
  return Error::Ok();
}

}  // namespace thorlabs

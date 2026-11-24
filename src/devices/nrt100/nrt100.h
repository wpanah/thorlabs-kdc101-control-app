#ifndef NRT100_H_
#define NRT100_H_

#include <expected>  // Requires C++23
#include <memory>
#include <optional>
#include <string>

#include "error/error.h"

namespace thorlabs {
using errors::Error;
class KinesisSimulation;

struct TravelLimits {
  double minimum_mm;
  double maximum_mm;
  bool Contains(double position_mm) const {
    return minimum_mm <= position_mm && position_mm <= maximum_mm;
  }
};

// Installation notes (2026-10-26):
// - Kinesis shows controller 70871422 with channels 1, 2 and 3.
// - User-confirmed travel: X = 150 mm, Y = 100 mm, Z = 150 mm.
// - Channel 1 has the 100 mm profile, so it is presumed to be Y.
// - Channels 2 and 3 have 150 mm profiles; which is X/Z is NOT confirmed.
//   Do not treat their display order as a verified axis mapping.
// Connections with the same controller serial share one SDK open/close
// lifetime. Each channel may have only one owner. X/Z channel assignments must
// be selected explicitly in the UI; HIGH/LOW laser assumptions are in the laser
// header.
//
// NRT100/M or NRT150/M stage on a BSC benchtop stepper controller (SBC API).
// Motion is non-blocking; positions and distances are in millimetres.
class NRT100 {
 public:
  enum class StageModel { kNrt100M, kNrt150M };
  enum class Axis { kX, kY, kZ };

  // User-confirmed stage travel; controller channel mapping is independent.
  [[nodiscard]] static constexpr StageModel StageForAxis(Axis axis) {
    return axis == Axis::kY ? StageModel::kNrt100M : StageModel::kNrt150M;
  }
  [[nodiscard]] static constexpr double TravelForStage(StageModel stage) {
    return stage == StageModel::kNrt150M ? 150.0 : 100.0;
  }
  [[nodiscard]] double GetTravelMm() const { return TravelForStage(stage_); }
  [[nodiscard]] std::optional<TravelLimits> GetTravelLimits() const {
    return limits_;
  }
  [[nodiscard]] Error SetTravelLimits(TravelLimits limits);
  [[nodiscard]] std::expected<bool, Error> IsAtPosition(double position_mm);
  enum class Direction { kForward, kBackward };

  enum class StopMode { kProfiled, kImmediate };

  enum class JogMode { kSingleStep, kContinuous };

  // Missing fields preserve the current device values
  struct VelocityParameters {
    std::optional<double> speed_mm_per_second;
    std::optional<double> acceleration_mm_per_second_squared;
  };

  struct MotorConfiguration {
    double move_speed_mm_per_second;
    double jog_step_mm;
    double acceleration_mm_per_second_squared;
    double homing_speed_mm_per_second;
    double backlash_mm;
  };

  // Represents device messages from the message queue
  enum class MotorEvent { kHomed, kMoveCompleted, kStopped, kOther };

  // Represents device information from polling
  struct MotorStatus {
    bool forward_limit_switch =
        false;  // Forward hardware travel limit triggered
    bool reverse_limit_switch =
        false;  // Reverse hardware travel limit triggered
    bool moving_forward = false;
    bool moving_reverse = false;
    bool jogging_forward = false;
    bool jogging_reverse = false;
    bool homing = false;
    bool homed = false;
    bool active = false;
    bool channel_enabled = false;
  };

  using CreateResult = std::expected<std::unique_ptr<NRT100>, Error>;
  using PositionResult = std::expected<double, Error>;
  using EventResult = std::expected<std::optional<MotorEvent>, Error>;

  NRT100() = delete;
  ~NRT100();

  // nrt100 motor should not be copyable
  NRT100(const NRT100&) = delete;
  NRT100& operator=(const NRT100&) = delete;

  // nrt100 motor not movable
  NRT100(NRT100&&) = delete;
  NRT100& operator=(NRT100&&) = delete;

  [[nodiscard]] static CreateResult CreateMotor(
      std::string serial_number, short channel = 1,
      int polling_interval_ms = 200,
      std::shared_ptr<const KinesisSimulation> simulation = nullptr,
      StageModel stage = StageModel::kNrt100M,
      std::optional<TravelLimits> limits = std::nullopt);

  [[nodiscard]] PositionResult GetPosition();
  [[nodiscard]] PositionResult GetApproachDistance();
  [[nodiscard]] PositionResult GetDistanceResolution();
  [[nodiscard]] std::expected<MotorStatus, Error> GetStatus();
  [[nodiscard]] std::expected<MotorConfiguration, Error> GetConfiguration();

  // Configuration only, doesn't start movement
  [[nodiscard]] Error SetMoveVelocity(VelocityParameters parameters);
  [[nodiscard]] Error SetJogVelocity(VelocityParameters parameters);
  [[nodiscard]] Error SetHomingSpeed(double speed_mm_per_second);
  [[nodiscard]] Error SetJogStepSize(double step_mm);
  [[nodiscard]] Error SetJogMode(JogMode mode, StopMode stop_mode);
  [[nodiscard]] Error SetBacklash(double distance_mm);

  // Methods to start a motor operation (non-blocking)
  [[nodiscard]] Error StartHome();
  [[nodiscard]] Error StartJog(Direction dir);
  [[nodiscard]] Error StartDrive(Direction dir);
  [[nodiscard]] Error Stop(StopMode stop_mode = StopMode::kProfiled);
  [[nodiscard]] Error StartMoveAbsolute(double position);
  [[nodiscard]] Error StartMoveRelative(double distance);

  [[nodiscard]] EventResult GetNextEvent();
  [[nodiscard]] Error CheckConnection() const;

  [[nodiscard]] Error ClearMessageQueue();

 private:
  [[nodiscard]] Error CheckReferenced() const;
  [[nodiscard]] std::expected<int, Error> TargetUnits(double position_mm);
  [[nodiscard]] Error LimitBacklashForTarget(int target_units);
  [[nodiscard]] Error StartPositionedMove(int target, bool jog = false);
  [[nodiscard]] Error StartApproachSegment(int from, bool jog);
  [[nodiscard]] Error RestoreMoveProfile();
  struct VelocityProfile {
    int acceleration;
    int speed;
  };
  struct Approach {
    int target;
    int waypoint;
    VelocityProfile profile;
    int observed_minimum;
    int observed_maximum;
  };
  NRT100(std::string serial_number, short channel,
         std::shared_ptr<const KinesisSimulation> simulation, StageModel stage);

  const std::string serial_number_;
  const short channel_;
  const StageModel stage_;
  std::optional<TravelLimits> limits_;
  int minimum_units_ = 0;
  int maximum_units_ = 0;
  int endpoint_margin_units_ = 0;
  double velocity_units_per_count_per_second_ = 0;
  double acceleration_units_per_count_per_second_squared_ = 0;
  double approach_time_s_ = 0;
  std::optional<Approach> approach_;
  std::optional<VelocityProfile> original_profile_;
  // Original correction while a move near a boundary has limited clearance.
  std::optional<long> configured_backlash_;
  std::shared_ptr<const KinesisSimulation> simulation_;
  bool connected_;
  bool polling_;
  bool channel_enabled_;
};

using Direction = NRT100::Direction;
using StopMode = NRT100::StopMode;
using JogMode = NRT100::JogMode;
using VelocityParameters = NRT100::VelocityParameters;
using MotorConfiguration = NRT100::MotorConfiguration;
using MotorStatus = NRT100::MotorStatus;
using MotorEvent = NRT100::MotorEvent;
inline constexpr int kDefaultPollingIntervalMs = 200;
inline constexpr double kDefaultMoveSpeedMmPerSecond = 30.0;
inline constexpr double kDefaultJogStepMm = 0.5;

}  // namespace thorlabs

#endif  // NRT100_H_

#pragma once
#include "simulation.hpp"
#include "skeet/lagcomp/history.hpp"
#include "skeet/client/commands.hpp"

namespace skeet {
// A bounded inverse-physics fit. No entity pointers or shared mutable state.
struct ExtrapolationMotionModel {
    float yaw = 0.0f;
    float yaw_rate = 0.0f; // degrees per second
    float wish_speed = 0.0f;
    float position_error = 0.0f;
    float velocity_error = 0.0f;
    float turn_error = 0.0f;
    float validation_position_error = 0.0f;
    float validation_velocity_error = 0.0f;
    float interval = 0.0f;
    float speed = 0.0f;
    std::uint32_t buttons = 0;
    std::int32_t observed_ticks = 0;
    bool valid = false;
    bool accepts(std::int32_t ticks) const noexcept;
    client::Command command(float seconds_from_anchor) const noexcept;
};

ExtrapolationMotionModel fit_extrapolation_motion(const lagcomp::RecordRing&,
                                                  const lagcomp::LagRecordHeader& anchor,
                                                  const MovementSimulation&,
                                                  const MovementPhysics&) noexcept;
// Initialize time-dependent movement fields from the selected record.
void restore_extrapolation_movement(MovementSimulation&, const lagcomp::LagRecordHeader&) noexcept;
}

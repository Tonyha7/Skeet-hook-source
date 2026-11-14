#include "skeet/movement/extrapolation_model.hpp"
#include "skeet/movement/simulation_velocity.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace skeet {
namespace {
constexpr float degrees = 57.29577951308232f;
float square(float x) noexcept {
    return x * x;
}
float length(float x, float y) noexcept {
    return std::hypot(x, y);
}
float yaw(float x, float y) noexcept {
    return std::atan2(y, x) * degrees;
}
float wrap(float angle) noexcept {
    return std::remainder(angle, 360.0f);
}
bool finite(const lagcomp::LagRecordHeader& h) noexcept {
    for (float v : h.origin)
        if (!std::isfinite(v))
            return false;
    for (float v : h.velocity)
        if (!std::isfinite(v))
            return false;
    return std::isfinite(h.duck_amount) && h.duck_amount >= 0 && h.duck_amount <= 1 &&
           std::isfinite(h.collision_max_z) && h.collision_max_z >= 36 && h.collision_max_z <= 82;
}
struct Sample {
    const lagcomp::LagRecordHeader* older;
    const lagcomp::LagRecordHeader* newer;
    int ticks;
    float weight;
};
struct Error {
    float loss = 0, position = 0, velocity = 0;
};
Error evaluate(const ExtrapolationMotionModel& model, const Sample* samples, int count,
               int anchor_tick, const MovementSimulation& seed,
               const MovementPhysics& physics) noexcept {
    Error error;
    float weights = 0;
    for (int i = 0; i < count; ++i) {
        const auto& s = samples[i];
        auto state = seed;
        restore_extrapolation_movement(state, *s.older);
        auto p = physics;
        p.player_flags = state.player_flags;
        p.duck_amount = state.duck_amount;
        p.ducking = state.ducking != 0;
        for (int tick = 1; tick <= s.ticks; ++tick) {
            const auto command = model.command(
                float(s.older->simulation_tick - anchor_tick + tick) * model.interval);
            prepare_movement_simulation_command(state, state.player, p.player_speed_limit,
                                                p.player_max_speed, command.movement);
            prepare_movement_simulation_velocity(state, p, command.movement,
                                                 {0, command.angles[1], 0}, false);
            // Fit horizontal free motion; the final forecast uses full hull traces,
            // steps, gravity and collisions. Contacts inconsistent with this fit
            // raise the residual instead of being interpreted as player input.
            state.position.x += state.velocity.x * model.interval;
            state.position.y += state.velocity.y * model.interval;
        }
        const auto pe = square(state.position.x - s.newer->origin[0]) +
                        square(state.position.y - s.newer->origin[1]);
        const auto ve = square(state.velocity.x - s.newer->velocity[0]) +
                        square(state.velocity.y - s.newer->velocity[1]);
        error.position += s.weight * pe;
        error.velocity += s.weight * ve;
        weights += s.weight;
    }
    error.position /= weights;
    error.velocity /= weights;
    // Velocity error is expressed as displacement over 50 ms for comparison.
    error.loss = error.position + 0.0025f * error.velocity + 0.00000001f * square(model.yaw_rate) +
                 0.00000001f * square(model.wish_speed);
    if (!std::isfinite(error.loss))
        error.loss = std::numeric_limits<float>::infinity();
    return error;
}
}

void restore_extrapolation_movement(MovementSimulation& state,
                                    const lagcomp::LagRecordHeader& h) noexcept {
    state.position = {h.origin[0], h.origin[1], h.origin[2]};
    state.velocity = {h.velocity[0], h.velocity[1], h.velocity[2]};
    state.player_flags = h.player_flags;
    state.duck_amount = h.duck_amount;
    state.ducking = (h.player_flags & 2U) != 0;
    state.hull_maximum.z = h.collision_max_z;
    state.mode = std::uint32_t(h.simulation_tick) + 1U;
    state.simulation_flags = 0;
}
client::Command ExtrapolationMotionModel::command(float time) const noexcept {
    client::Command result{};
    result.angles[1] = wrap(yaw + yaw_rate * time);
    result.movement.forward_move = wish_speed;
    result.movement.buttons = buttons;
    return result;
}
bool ExtrapolationMotionModel::accepts(std::int32_t ticks) const noexcept {
    if (!valid || ticks < 0 || ticks > 16 || ticks > observed_ticks * 2 + 2)
        return false;
    const float time = ticks * interval;
    if (time > 0.2f)
        return false;
    // A residual-based cutoff, not a guaranteed error bound or a probability.
    const float uncertainty = position_error + (velocity_error + 2.0f) * time +
                              0.5f * speed * (turn_error / degrees) * time * time;
    return std::isfinite(uncertainty) && uncertainty <= 8.0f;
}
static ExtrapolationMotionModel fit_motion(const lagcomp::RecordRing& history,
                                           const lagcomp::LagRecordHeader& anchor,
                                           const MovementSimulation& seed,
                                           const MovementPhysics& physics) noexcept {
    ExtrapolationMotionModel model;
    model.interval = seed.rules.interval_per_tick;
    if (!finite(anchor) || (anchor.flags & 0x200U) || history.count > 32 ||
        !(model.interval > 0 && model.interval <= 0.1f) || seed.movement_type != 2 ||
        !(physics.player_speed_limit > 0 && physics.player_max_speed > 0) ||
        !std::isfinite(physics.player_speed_limit) || !std::isfinite(physics.player_max_speed) ||
        !std::isfinite(seed.surface_friction))
        return model;
    unsigned start = 0;
    while (start < history.count &&
           history.at(start).header.simulation_tick != anchor.simulation_tick)
        ++start;
    if (start >= history.count)
        return model;
    const auto& source = history.at(start).header;
    if (length(source.origin[0] - anchor.origin[0], source.origin[1] - anchor.origin[1]) > 0.01f)
        return model;
    Sample samples[3]{};
    float turns[3]{};
    int count = 0;
    for (unsigned age = start; age + 1 < history.count && count < 3; ++age) {
        const auto& newer = history.at(age).header;
        const auto& older = history.at(age + 1).header;
        const auto gap = std::int64_t(newer.simulation_tick) - older.simulation_tick;
        if (!finite(newer) || !finite(older) || (newer.flags & 0x200U) || (older.flags & 0x200U) ||
            gap < 1 || gap > 16 || ((newer.player_flags ^ older.player_flags) & 3U) ||
            std::abs(newer.duck_amount - older.duck_amount) > 0.05f)
            break;
        const float dt = float(gap) * model.interval;
        const float dx = newer.origin[0] - older.origin[0], dy = newer.origin[1] - older.origin[1];
        const float speed = std::max(length(newer.velocity[0], newer.velocity[1]),
                                     length(older.velocity[0], older.velocity[1]));
        if (length(dx, dy) > 64 || length(dx, dy) > speed * dt + 8.0f)
            break;
        samples[count] = {&older, &newer, int(gap), std::ldexp(1.0f, -count)};
        turns[count] = speed > 20 ? wrap(yaw(newer.velocity[0], newer.velocity[1]) -
                                         yaw(older.velocity[0], older.velocity[1])) /
                                        dt
                                  : 0;
        model.observed_ticks += int(gap);
        ++count;
    }
    if (!count)
        return model;
    model.speed = length(anchor.velocity[0], anchor.velocity[1]);
    model.yaw = yaw(anchor.velocity[0], anchor.velocity[1]);
    model.buttons = (anchor.player_flags & 2U) ? duck_button : 0;
    const float limit = std::min({450.0f, physics.player_max_speed, physics.player_speed_limit});
    model.wish_speed = std::min(model.speed, limit);
    auto best = evaluate(model, samples, count, anchor.simulation_tick, seed, physics);
    const auto consider = [&](ExtrapolationMotionModel candidate) {
        candidate.wish_speed = std::clamp(candidate.wish_speed, 0.0f, limit);
        candidate.yaw = wrap(candidate.yaw);
        candidate.yaw_rate = std::clamp(candidate.yaw_rate, -360.0f, 360.0f);
        if (count < 2)
            candidate.yaw_rate = 0;
        const auto error =
            evaluate(candidate, samples, count, anchor.simulation_tick, seed, physics);
        if (error.loss < best.loss) {
            model = candidate;
            best = error;
        }
    };
    std::array<float, 3> input_headings{}, input_magnitudes{};
    for (int i = 0; i < count; ++i) {
        auto inertial = seed;
        restore_extrapolation_movement(inertial, *samples[i].older);
        auto p = physics;
        p.player_flags = inertial.player_flags;
        p.duck_amount = inertial.duck_amount;
        p.ducking = inertial.ducking != 0;
        for (int tick = 0; tick < samples[i].ticks; ++tick)
            prepare_movement_simulation_velocity(inertial, p, {}, {}, false);
        const auto& v = samples[i].newer->velocity;
        input_headings[i] = yaw(v[0] - inertial.velocity.x, v[1] - inertial.velocity.y);
        input_magnitudes[i] = length(v[0] - inertial.velocity.x, v[1] - inertial.velocity.y);
    }
    // The velocity heading lags steering under acceleration. Recover the turn
    // of the acceleration vector at interval midpoints as a separate seed.
    const float input_turn =
        count >= 2 && input_magnitudes[0] > 0.5f && input_magnitudes[1] > 0.5f
            ? std::clamp(wrap(input_headings[0] - input_headings[1]) /
                             (0.5f * float(samples[0].ticks + samples[1].ticks) * model.interval),
                         -360.0f, 360.0f)
            : 0.0f;
    const float input_yaw =
        wrap(input_headings[0] + input_turn * float(samples[0].ticks) * model.interval * 0.5f);
    const float heading = yaw(anchor.velocity[0], anchor.velocity[1]);
    const float turn = count >= 2 ? std::clamp((turns[0] * 2 + turns[1]) / 3, -360.0f, 360.0f) : 0;
    for (float heading_seed : {heading, input_yaw, wrap(heading + 180)})
        for (float speed_seed : {0.0f, std::min(model.speed, limit), limit})
            for (float rate : {0.0f, turn, input_turn}) {
                auto candidate = model;
                candidate.yaw = heading_seed;
                candidate.wish_speed = speed_seed;
                candidate.yaw_rate = rate;
                consider(candidate);
            }
    // Deterministic coordinate refinement keeps the per-record work bounded.
    float weighted_time = 0, weight_sum = 0;
    for (int i = 0; i < count; ++i) {
        weighted_time += samples[i].weight *
                         (float(samples[i].older->simulation_tick - anchor.simulation_tick) +
                          0.5f * float(samples[i].ticks + 1)) *
                         model.interval;
        weight_sum += samples[i].weight;
    }
    const float pivot_time = weighted_time / weight_sum;
    for (int pass = 0; pass < 6; ++pass) {
        const float scale = std::ldexp(1.0f, -pass);
        for (int axis = 0; axis < 3; ++axis) {
            const auto center = model;
            for (float sign : {-1.0f, 1.0f}) {
                auto candidate = center;
                if (axis == 0)
                    candidate.yaw += sign * 22.5f * scale;
                if (axis == 1)
                    candidate.wish_speed += sign * limit * 0.25f * scale;
                if (axis == 2) {
                    const float change = sign * 90.0f * scale;
                    candidate.yaw_rate += change;
                    candidate.yaw -= change * pivot_time;
                }
                consider(candidate);
            }
        }
    }
    if (anchor.player_flags & 1U) {
        auto candidate = model;
        candidate.buttons |= speed_button;
        consider(candidate);
    }
    model.position_error = std::sqrt(best.position);
    model.velocity_error = std::sqrt(best.velocity);
    model.turn_error = count > 1 ? std::abs(turns[0] - turns[1]) : 90.0f;
    model.valid =
        std::isfinite(best.loss) && model.position_error <= 4.0f && model.velocity_error <= 40.0f;
    return model;
}
ExtrapolationMotionModel fit_extrapolation_motion(const lagcomp::RecordRing& history,
                                                  const lagcomp::LagRecordHeader& anchor,
                                                  const MovementSimulation& seed,
                                                  const MovementPhysics& physics) noexcept {
    auto model = fit_motion(history, anchor, seed, physics);
    if (!model.valid)
        return model;
    unsigned start = 0;
    while (start < history.count &&
           history.at(start).header.simulation_tick != anchor.simulation_tick)
        ++start;
    if (start + 2 >= history.count) {
        model.valid = false;
        return model;
    }
    const auto& previous = history.at(start + 1).header;
    const auto gap = std::int64_t(anchor.simulation_tick) - previous.simulation_tick;
    if (gap < 1 || gap > 16) {
        model.valid = false;
        return model;
    }
    // Stateless holdout: reconstruct input from records preceding the latest
    // observation, then predict that observation without using it in the fit.
    // No learned state, per-player training or cross-match accumulation.
    const auto prior = fit_motion(history, previous, seed, physics);
    if (!prior.valid) {
        model.valid = false;
        return model;
    }
    const Sample held_out{&previous, &anchor, int(gap), 1.0f};
    const auto validation = evaluate(prior, &held_out, 1, previous.simulation_tick, seed, physics);
    model.validation_position_error = std::sqrt(validation.position);
    model.validation_velocity_error = std::sqrt(validation.velocity);
    model.position_error = std::max(model.position_error, model.validation_position_error);
    // Convert held-out displacement error to an observed error rate rather
    // than treating a good in-sample fit as evidence of future accuracy.
    model.velocity_error =
        std::max({model.velocity_error, model.validation_velocity_error,
                  model.validation_position_error / (float(gap) * model.interval)});
    model.valid = std::isfinite(validation.loss) && model.position_error <= 4.0f &&
                  model.velocity_error <= 40.0f;
    return model;
}
}

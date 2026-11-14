#include "skeet/lagcomp/history_resolver_update.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include "skeet/client/player_state.hpp"
#include "skeet/lagcomp/prediction_scope.hpp"
#include "skeet/resolver.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
float absolute(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) & 0x7FFFFFFFu);
}
float negative(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) ^ 0x80000000u);
}
float normalize(float value) noexcept {
    if (std::isnan(value))
        return 0.0f;
    if (absolute(value) > 180.0f) {
        const float turns = round_half_away_from_zero(absolute(value / 360.0f)) * 360.0f;
        value = value < 0.0f ? value + turns : value - turns;
    }
    return value;
}
std::int32_t truncate(float value) noexcept {
    return _mm_cvttss_si32(_mm_set_ss(value));
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(a) +
                                       std::bit_cast<std::uint32_t>(b));
}
std::int32_t subtract(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(a) -
                                       std::bit_cast<std::uint32_t>(b));
}
float clamp_unit(float value) noexcept {
    return _mm_cvtss_f32(
        _mm_min_ss(_mm_max_ss(_mm_set_ss(value), _mm_setzero_ps()), _mm_set_ss(1.0f)));
}
float convar(const void* value) noexcept {
    return std::bit_cast<float>(read_prediction_convar(const_cast<void*>(value)));
}
float local_time(const HistoryResolverUpdateBindings& bindings) noexcept {
    if (!bindings.local_time_entity || !*bindings.local_time_entity)
        return 0.0f;
    return read<float>(*bindings.local_time_entity, bindings.fields.local_time_9f8e7f71);
}
struct Suppression {
    std::uint8_t* target;
    std::uint8_t saved = 0;
    explicit Suppression(std::uint8_t* value) : target(value) {
        if (target) {
            saved = *target;
            *target = 0;
        }
    }
    void restore() {
        if (target) {
            *target = saved;
            target = nullptr;
        }
    }
    ~Suppression() {
        restore();
    }
};
resolver_runtime::LagRecordHistory history_view(client::PlayerState& owner) noexcept {
    return {owner.history.records, owner.history.first, owner.history.count};
}
//43412A95..434137FE: this caller uses model descriptors0/3 without the
// nineteen-hitbox cap used by the independent aiming-model reader.
bool feedback_from_environment(client::PlayerState& owner, void* player, std::uint8_t mode,
                               std::uint8_t previous_attempt, float clock,
                               HistoryResolverScratch& scratch,
                               const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    Suppression suppression(bindings.environment.suppression_flag);
    const auto origin = read<RageVector>(player, bindings.entity.offsets.origin);
    const auto probe =
        resolver_runtime::probe_environment(bindings.local_origin, origin, bindings.environment);
    if (!probe.selected)
        return false;
    RageVector direction;
    if (probe.damage[0] == 0 && probe.damage[1] == 0)
        for (unsigned axis = 0; axis < 3; ++axis)
            direction[axis] =
                engine::read_opaque_native_float(scratch.probe_direction.data() + axis);
    else {
        direction = probe.direction;
        scratch.probe_direction = direction;
    }
    const auto* wrapper = read<const void*>(player, bindings.fields.studio_wrapper);
    if (!wrapper)
        return false;
    const auto* header = read<const std::byte*>(wrapper, 0);
    if (!header)
        return false;
    const auto* set =
        header + read<std::int32_t>(header, 0xB0) +
        std::ptrdiff_t(read<std::int32_t>(player, bindings.fields.hitbox_set_c2b6fb10)) * 12;
    if (read<std::int32_t>(set, 4) < (bindings.configuration.reworked() ? 4 : 1))
        return false;
    const auto* descriptors = set + read<std::int32_t>(set, 8);
    std::array<resolver_runtime::EnvironmentCapsule, 2> hitboxes;
    constexpr std::size_t indices[]{0, 3};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* box = descriptors + indices[i] * 0x44;
        const auto radius = read<float>(box, 0x30);
        if (!(radius > 0.0f))
            return false;
        hitboxes[i] = {std::uint32_t(indices[i]), read<RageVector>(box, 8),
                       read<RageVector>(box, 0x14), radius};
    }
    const auto& source = owner.history.at(0);
    const float yaw_delta =
        state.reference_yaw - std::bit_cast<float>(source.header.player_history_1ac);
    const auto& old_origin = source.header.origin;
    const bool rebase = yaw_delta != 0.0f || old_origin[0] != origin[0] ||
                        old_origin[1] != origin[1] || old_origin[2] != origin[2];
    for (auto& axis : direction)
        axis = negative(axis);
    const RageVector eye{bindings.local_origin[0], bindings.local_origin[1],
                         bindings.local_origin[2] + 64.0f};
    const auto selection =
        rebase ? resolver_runtime::score_environment_hypotheses(
                     source, hitboxes, direction, eye, {yaw_delta, old_origin, origin},
                     bindings.environment.trigonometry, bindings.configuration.reworked())
               : resolver_runtime::score_environment_hypotheses(source, hitboxes, direction, eye,
                                                                bindings.configuration.reworked());
    if (!selection.valid())
        return false;
    const auto attempt = resolver_runtime::classify_winning_pose(selection.pose_yaw);
    state.resolver_slots[mode].attempt = attempt;
    state.probe_time = clock;
    state.last_update_tick =
        corrected_command_tick(bindings.entity.command_tick, bindings.entity.previous_command,
                               bindings.entity.current_command);
    suppression.restore();
    state.resolver_status = 1;
    if (mode == 1)
        state.resolver_slots[2].attempt = attempt;
    else if (mode == 2)
        state.resolver_slots[1].attempt = attempt;
    if (previous_attempt != attempt) {
        auto history = history_view(owner);
        resolver_runtime::propagate_attempt(history, mode, previous_attempt, attempt);
    }
    return true;
}
//43411CBD..434138D9, before the selected mode's animation-yaw policy.
void update_inline_feedback(client::PlayerState& owner, void* player, std::uint8_t& mode,
                            SimulationTickSpan span, std::uint32_t& flags,
                            HistoryResolverScratch& scratch,
                            const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(player) + 8, 10);
    if (!bindings.player_table[std::size_t(index) * 0x20 + 6])
        return;
    if (!(state.flags & 0x40u) && read<float>(&state, 4) == local_time(bindings)) {
        state.flags |= 0x40u;
        const auto delta = normalize(state.reference_yaw - read<float>(&state, 8));
        if (absolute(delta) < 60.0f) {
            mode = std::uint8_t(!(state.flags & 4u)) + 1;
            state.resolver_slots[mode].attempt = resolver_runtime::classify_winning_pose(delta);
            state.resolver_status = 3;
            return;
        }
    }
    if (state.resolver_status == 3 || (flags & 1u))
        return;
    const auto selected = bindings.selected_entity;
    if (selected == -1 || engine::invoke<void*>(bindings.entity.entity_list, 4, selected) != player)
        return;
    if (span.count <= 1 &&
        (owner.history.count <= 1 || subtract(owner.history.at(0).header.simulation_tick,
                                              owner.history.at(1).header.simulation_tick) <= 1))
        return;
    const auto selected_index = selected == -1 ? 0x1FFFu : std::uint32_t(std::uint16_t(selected));
    const auto pending_shots = std::span<const HistoryEvaluation>(bindings.pending_shots);
    if (resolver_runtime::has_pending_environment_shot(pending_shots, selected_index))
        return;
    if (mode != 1 && mode != 2)
        return;
    const auto previous = state.resolver_slots[mode].attempt;
    const auto clock = local_time(bindings);
    if (clock == state.probe_time &&
        (state.probe_count >= 2 || absolute(bindings.current_time - state.probe_stamp) < 5.0f))
        return;
    if (!owner.history.count)
        return;
    if (clock == state.reference_time && state.resolver_status == 2) {
        const auto origin = read<RageVector>(player, bindings.entity.offsets.origin);
        const float y = state.reference_origin[1] - origin[1],
                    x = state.reference_origin[0] - origin[0],
                    z = state.reference_origin[2] - origin[2];
        if (10000.0f > (y * y + x * x) + z * z)
            return;
    }
    if (!(owner.history.at(0).header.flags & 0x80u))
        return;
    if (feedback_from_environment(owner, player, mode, previous, clock, scratch, bindings))
        flags |= 0x10u;
}
void choose_resolver_yaw(client::PlayerState& owner, void* player, void* animation_state,
                         SimulationTickSpan span, std::int32_t shot_tick, std::uint32_t& flags,
                         const HistoryResolverInput& input, HistoryResolverScratch& scratch,
                         const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    const auto angles = bindings.entity.offsets.eye_angles;
    const auto* table = bindings.player_table + input.player_table_offset;
    const float previous_animation_yaw = read<float>(animation_state, 0x80);
    if (table[5])
        write(animation_state, 0x80, state.reference_yaw - float(read<std::int32_t>(table, 8)));
    else {
        if (table[6]) {
            if (!table[4] && bindings.configuration.prediction_available) {
                float pitch = read<float>(player, angles);
                const auto delta =
                    absolute(normalize((read<float>(player, bindings.fields.recoil_pitch_66146802) +
                                        read<float>(&state, 0x1A8)) -
                                       180.0f));
                if (delta < 0.1f) {
                    pitch = -180.0f;
                    if (read<std::uint8_t>(&state, 0xA4) &&
                        absolute(read<float>(&state, 0x108)) == 180.0f)
                        pitch = read<float>(&state, 0x108);
                    else if (read<std::uint16_t>(&state, 0xA6) % 11 == 10)
                        pitch = 180.0f;
                }
                write(player, angles, pitch);
            }
            std::uint8_t mode = 0, attempt = 0;
            if (span.count > 1 || read<std::uint8_t>(&state, 0xA3) > 1)
                mode = (flags & 4u) && bindings.shot_angles_active
                           ? 3
                           : std::uint8_t(!(state.flags & 4u)) + 1;
            update_inline_feedback(owner, player, mode, span, flags, scratch, bindings);
            if (mode == 0) {
                mode = input.previous_resolver_index;
                attempt = read<std::uint8_t>(&state, 0xA2);
            } else if (mode < 3) {
                const auto index =
                    engine::invoke<std::int32_t>(static_cast<std::byte*>(player) + 8, 10);
                attempt = state.resolver_slots[mode].attempt % 3u;
                if (attempt == 0) {
                    if (bindings.player_table[std::size_t(index) * 0x20 + 6] ||
                        read<std::int32_t>(&state, std::size_t(mode) * 0x24 + 0x20) > 1 ||
                        absolute(bindings.current_time -
                                 read<float>(&state, std::size_t(mode) * 0x24 + 0x1C)) <= 2.0f)
                        write(animation_state, 0x80, state.reference_yaw);
                    else if (input.previous_resolver_index != mode ||
                             read<std::uint8_t>(&state, 0xA2) != 0)
                        write(animation_state, 0x80, std::bit_cast<float>(state.capture_field_180));
                } else if (attempt == 1) {
                    write(animation_state, 0x80, state.reference_yaw - input.maximum_yaw);
                    write(player, angles + 4, normalize(state.reference_yaw - 120.0f));
                } else if (attempt == 2) {
                    write(animation_state, 0x80, state.reference_yaw + input.maximum_yaw);
                    write(player, angles + 4, normalize(state.reference_yaw + 120.0f));
                }
            } else if (mode == 3) {
                bool retain_center = false;
                if (span.count == 1 || (bindings.shot_angles_active && (flags & 4u) &&
                                        shot_tick == add(span.first, 1))) {
                    const auto difference = state.reference_yaw - read<float>(&state, 0x114);
                    retain_center =
                        std::isnan(difference) || absolute(normalize(difference)) < 120.0f;
                }
                if (!retain_center) {
                    const auto delta = normalize(state.reference_yaw -
                                                 std::bit_cast<float>(state.capture_field_180));
                    attempt = read<std::uint8_t>(&state, 0x7C) % 3u;
                    if (attempt == 2)
                        write(animation_state, 0x80, state.reference_yaw);
                    else if ((attempt == 0 && delta > 0.0f) || (attempt == 1 && !(delta > 0.0f))) {
                        write(animation_state, 0x80, state.reference_yaw + input.maximum_yaw);
                        write(player, angles + 4, normalize(state.reference_yaw + 120.0f));
                    } else {
                        write(animation_state, 0x80, state.reference_yaw - input.maximum_yaw);
                        write(player, angles + 4, normalize(state.reference_yaw - 120.0f));
                    }
                }
            }
            write(&state, 0xA2, attempt);
            write(&state, 0xA3, std::uint8_t(span.count));
            state.resolver_index = mode;
        }
        if (read<float>(&state, 0x524) > 0.0f)
            write(player, bindings.entity.offsets.field_a2a8247f, read<float>(player, angles + 4));
    }
    const auto yaw = read<float>(animation_state, 0x80);
    if (previous_animation_yaw != yaw)
        write(animation_state, 0x80, normalize(yaw));
}

struct StepFields {
    std::uint32_t flags;
    RageVector angles;
    float recoil, lower_body_yaw, duck;
    Vector velocity, absolute_velocity;
};
StepFields capture_step_fields(void* player,
                               const HistoryResolverUpdateBindings& bindings) noexcept {
    const auto& o = bindings.entity.offsets;
    return {read<std::uint32_t>(player, o.player_flags),
            read<RageVector>(player, o.eye_angles),
            read<float>(player, bindings.fields.recoil_pitch_66146802),
            read<float>(player, o.field_a2a8247f),
            read<float>(player, o.duck_amount),
            read<Vector>(player, o.velocity),
            read<Vector>(player, bindings.fields.absolute_velocity_4e66bb34)};
}
void publish_step_fields(void* player, const StepFields& value,
                         const HistoryResolverUpdateBindings& bindings) noexcept {
    const auto& o = bindings.entity.offsets;
    write(player, o.player_flags, value.flags);
    write(player, o.eye_angles, value.angles);
    write(player, bindings.fields.recoil_pitch_66146802, value.recoil);
    write(player, o.field_a2a8247f, value.lower_body_yaw);
    write(player, o.duck_amount, value.duck);
    write(player, o.velocity, value.velocity);
    write(player, bindings.fields.absolute_velocity_4e66bb34, value.absolute_velocity);
}
std::int32_t animation_step_count(const void* state, float interval) noexcept {
    const auto current = read<float>(state, 0x5B4);
    auto cycle = read<float>(state, 0x448), rate = read<float>(state, 0x444);
    const float delta = current - cycle;
    std::int32_t ticks = 0;
    // COMISS zero,delta / JBE includes an unordered delta in this arm.
    if (!(delta < 0.0f))
        ticks = truncate((delta / rate) / interval + 0.5f);
    else
        for (std::int32_t i = 0; i < 16; ++i) {
            ticks = add(ticks, 1);
            if (rate * interval + cycle >= 0x1.fffcp-1f)
                rate = read<float>(state, 0x5B0);
            cycle = rate * interval + cycle;
            const auto integer = truncate(cycle);
            if (integer > 0) {
                const auto rest = truncate(
                    ((current - (cycle - float(integer))) / read<float>(state, 0x5B0)) / interval +
                    0.5f);
                ticks = add(ticks, rest);
                break;
            }
        }
    return ticks < 1 ? 1 : ticks > 16 ? 16 : ticks;
}
void step_history_animation(client::PlayerState& owner, void* player, void* animation_state,
                            SimulationTickSpan span, std::int32_t shot_tick, std::uint32_t flags,
                            HistoryResolverUpdateResult& result,
                            const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    const auto& o = bindings.entity.offsets;
    const bool reworked = bindings.configuration.reworked();
    if (reworked && (span.count < 1 || span.count > 16 || !(bindings.entity.interval > 0.0f) ||
                     !std::isfinite(bindings.entity.interval)))
        return;
    auto* descriptor = static_cast<std::byte*>(player) + o.animation_layers;
    const auto count = read<std::int32_t>(descriptor, 0xC);
    const auto copy_count = count > 13 ? 13 : count;
    for (std::int32_t i = 0; i < copy_count; ++i)
        std::memcpy(static_cast<std::byte*>(read<void*>(descriptor, 0)) + std::size_t(i) * 0x38 +
                        0x14,
                    reinterpret_cast<const std::byte*>(&state) + (reworked ? 0x2FC : 0x3A4) +
                        std::size_t(i) * 0x1C,
                    0x1C);
    constexpr std::size_t animation_offsets[]{0x98, 0x19C, 0x190, 0x198, 0x180};
    constexpr std::size_t owner_offsets[]{0x3BC, 0x3C4, 0x3CC, 0x3D8, 0x458};
    for (std::size_t i = 0; i < 5; ++i)
        write(animation_state, animation_offsets[i], read<std::uint32_t>(&state, owner_offsets[i]));
    for (std::int32_t i = 0; i < copy_count; ++i)
        write(read<void*>(descriptor, 0), std::size_t(i) * 0x38 + 0x18,
              read<std::uint32_t>(&state, 0x46C + std::size_t(i) * 0x1C));
    const auto saved = capture_step_fields(player, bindings);
    auto current = saved;
    // The pre-network capture is the previous velocity, not the newly
    // received endpoint. Starting both endpoints at saved made delta zero.
    if (reworked)
        current.velocity = read<Vector>(&state, 0x120);
    current.recoil = read<float>(&state, 0x150);
    current.duck = read<float>(&state, 0x104);
    if (read<std::uint32_t>(&state, 0x238)) {
        const auto previous = reworked && read<std::uint32_t>(&state, 0x238) > 1 ? 1u : 0u;
        const auto sample = ((read<std::uint32_t>(&state, 0x234) + previous) % 6u) * 0x20;
        current.flags = read<std::uint32_t>(&state, 0x250 + sample);
        current.absolute_velocity = read<Vector>(&state, 0x240 + sample);
    }
    // Network simulation ticks define the interval. Layer cycles may locate
    // events inside it, but a zero playback rate cannot shorten the interval.
    const auto ticks =
        reworked ? span.count : animation_step_count(&state, bindings.entity.interval);
    const auto start_position = current.absolute_velocity;
    std::int32_t jump_tick = -1, land_tick = -1, pre_shot_tick = -1, duck_full_tick = -1,
                 duck_zero_tick = -1;
    auto shot_cutoff = INT32_MAX;
    bool interpolate_duck = current.duck != saved.duck;
    const auto duck_delta = (saved.duck - current.duck) / float(ticks);
    const float inverse = 1.0f / float(ticks);
    const Vector velocity_delta{(saved.velocity[0] - current.velocity[0]) * inverse,
                                (saved.velocity[1] - current.velocity[1]) * inverse,
                                (saved.velocity[2] - current.velocity[2]) * inverse};
    if (shot_tick != -1 && (flags & 4u)) {
        if (bindings.shot_angles_active)
            shot_cutoff = shot_tick;
        else
            pre_shot_tick = shot_tick;
    }
    bool physical_air = false;
    const auto event_tick = [&](float cycle, float rate) {
        if (reworked &&
            (!std::isfinite(cycle) || !std::isfinite(rate) || cycle < 0.0f || !(rate > 0.0f)))
            return -1;
        const auto age = truncate((cycle / rate) / bindings.entity.interval + 0.5f);
        if (reworked && (age < 0 || age > ticks))
            return -1;
        return add(span.last, subtract(1, age));
    };
    if (!(saved.flags & 1u)) {
        const auto cycle = read<float>(&state, 0x4F0), old_cycle = read<float>(&state, 0x384);
        if (!((cycle == old_cycle &&
               read<std::int32_t>(&state, 0x4DC) == read<std::int32_t>(&state, 0x370)) ||
              old_cycle <= cycle || !bindings.velocity.classify)) {
            if (bindings.velocity.classify(player, read<std::int32_t>(&state, 0x4DC)) == 0x3D9) {
                jump_tick = event_tick(cycle, read<float>(&state, 0x4EC));
                const char* name =
                    bindings.velocity.sequence_name
                        ? bindings.velocity.sequence_name(player, read<std::int32_t>(&state, 0x4DC))
                        : "";
                const auto comparison = bindings.velocity.compare_sequence_name(
                    name, bindings.velocity.jump_sequence_name);
                if (comparison == 0 && saved.duck >= 1.0f)
                    duck_full_tick = add(jump_tick, 1);
                else if (comparison != 0 && !(saved.duck > 0.0f))
                    duck_zero_tick = add(jump_tick, 1);
                interpolate_duck = false;
            }
            physical_air = jump_tick != -1 && owner.history.count != 0;
        }
    } else if (owner.history.count && !(owner.history.at(0).header.player_flags & 1u)) {
        if (read<float>(&state, 0x500) != 0.0f && read<float>(&state, 0x394) == 0.0f &&
            bindings.velocity.classify) {
            const auto activity =
                bindings.velocity.classify(player, read<std::int32_t>(&state, 0x4F8));
            if (activity == 0x3DC || activity == 0x3DD) {
                land_tick = event_tick(read<float>(&state, 0x50C), read<float>(&state, 0x508));
                physical_air = land_tick != -1 && owner.history.count != 0;
            }
        }
    } else
        physical_air = jump_tick != -1 && owner.history.count != 0;
    bool override_animation =
        read<std::uint8_t>(player, bindings.fields.animation_override_00a5bd68) == 0 &&
        read<float>(&state, 0x538) > 0.5f &&
        read<float>(&state, 0x538) == read<float>(&state, 0x3CC);
    const auto first = subtract(span.last, ticks);
    if (reworked) {
        const auto in_span = [&](std::int32_t tick) { return tick > first && tick <= span.last; };
        if (!in_span(jump_tick))
            jump_tick = -1;
        if (!in_span(land_tick))
            land_tick = -1;
        if (jump_tick == -1) {
            duck_full_tick = -1;
            duck_zero_tick = -1;
            interpolate_duck = current.duck != saved.duck;
        }
        physical_air = physical_air && (jump_tick != -1 || land_tick != -1);
    }
    write(animation_state, 0x6C, float(first) * bindings.entity.interval);
    auto vertical = current.velocity[2];
    for (auto tick = add(first, 1); tick < span.last; tick = add(tick, 1)) {
        bindings.animation_time = float(tick) * bindings.entity.interval;
        if (reworked) {
            const auto fraction = float(subtract(tick, first)) / float(ticks);
            for (unsigned axis = 0; axis < 3; ++axis)
                if (axis < 2 || !physical_air)
                    current.absolute_velocity[axis] =
                        start_position[axis] +
                        (saved.absolute_velocity[axis] - start_position[axis]) * fraction;
        }
        if (physical_air) {
            if (!(current.flags & 1u)) {
                const auto half_gravity =
                    convar(bindings.velocity.cvars.gravity) * bindings.entity.interval * 0.5f;
                const auto half_velocity = vertical - half_gravity;
                vertical = half_velocity - half_gravity;
                current.absolute_velocity[2] =
                    half_velocity * bindings.entity.interval + current.absolute_velocity[2];
                current.velocity[2] = vertical;
            } else {
                vertical = 0.0f;
                current.velocity[2] = 0.0f;
            }
        }
        if (tick == land_tick) {
            current.flags |= 1u;
            auto* live = read<void*>(descriptor, 0);
            write(live, 0x144, std::uint32_t(0));
            write(live, 0x140, read<std::uint32_t>(&state, 0x508));
            land_tick = -1;
        }
        if (tick == subtract(jump_tick, 1))
            current.flags |= 1u;
        if (tick == jump_tick) {
            const auto previous_flags = current.flags;
            current.flags &= ~1u;
            auto* live = read<void*>(descriptor, 0);
            write(live, 0x10C, std::uint32_t(0));
            write(live, 0x108, read<std::uint32_t>(&state, 0x4EC));
            if (physical_air) {
                const auto impulse = convar(bindings.velocity.cvars.jump_impulse);
                vertical = previous_flags & 2u ? impulse : vertical + impulse;
                current.absolute_velocity[2] =
                    vertical * bindings.entity.interval + current.absolute_velocity[2];
                vertical -=
                    convar(bindings.velocity.cvars.gravity) * bindings.entity.interval * 0.5f;
                current.velocity[2] = vertical;
            }
        }
        if (tick >= shot_cutoff) {
            current.angles[0] = read<float>(&state, 0x1A8);
            current.lower_body_yaw = read<float>(&state, 0x118);
            current.angles[1] = read<float>(&state, 0x1AC);
        } else if (tick == shot_tick && tick == pre_shot_tick) {
            const auto old_pitch =
                read<float>(&state, 0x1C4) -
                convar(bindings.recoil_scale_convar) * read<float>(&state, 0x150);
            const auto pitch_delta = read<float>(&state, 0x1A8) - old_pitch;
            bool replace = !std::isnan(pitch_delta) && absolute(normalize(pitch_delta)) > 10.0f;
            if (!replace) {
                const auto yaw_delta = read<float>(&state, 0x1AC) - read<float>(&state, 0x1BC);
                replace = !std::isnan(yaw_delta) && absolute(normalize(yaw_delta)) > 10.0f;
            }
            if (replace) {
                current.angles[0] = old_pitch;
                current.angles[1] = read<float>(&state, 0x1BC);
            }
        }
        if (tick == duck_full_tick || tick == duck_zero_tick)
            current.duck = saved.duck;
        if (interpolate_duck)
            current.duck = clamp_unit(duck_delta + current.duck);
        current.velocity[0] = velocity_delta[0] + current.velocity[0];
        current.velocity[1] = velocity_delta[1] + current.velocity[1];
        if (!reworked || !physical_air)
            vertical = velocity_delta[2] + vertical;
        current.velocity[2] = vertical;
        publish_step_fields(player, current, bindings);
        if (override_animation)
            write(player, bindings.fields.animation_override_00a5bd68, std::uint8_t(1));
        animation::update_animation_state(animation_state, current.angles,
                                          {bindings.animation_frame, bindings.animation_time,
                                           bindings.entity.interval, bindings.animation_reset_byte},
                                          bindings.animation);
        if (override_animation) {
            override_animation = false;
            write(player, bindings.fields.animation_override_00a5bd68, std::uint8_t(0));
        }
        result.speed_fraction = read<float>(animation_state, 0x11C);
        if (tick == shot_tick)
            current.recoil = saved.recoil;
        current.lower_body_yaw = read<float>(&state, 0x118);
    }
    publish_step_fields(player, saved, bindings);
    result.animation_publish_layer_weight = read<float>(read<void*>(descriptor, 0), 0x138);
}
} // namespace

HistoryResolverUpdateResult
update_history_resolver(client::PlayerState& owner, void* player, void* animation_state,
                        const HistoryAnimationSeed& seed, SimulationTickSpan span,
                        std::int32_t shot_tick, std::uint32_t& flags,
                        const HistoryResolverInput& input, HistoryResolverScratch& scratch,
                        const HistoryResolverUpdateBindings& bindings) {
    HistoryResolverUpdateResult result{read<float>(&owner.native, 0x394),
                                       input.initial_speed_fraction};
    const auto* enabled = bindings.configuration.resolver_enabled;
    if (*enabled) {
        const auto angles = bindings.entity.offsets.eye_angles;
        const auto saved_yaw = read<float>(player, angles + 4);
        const auto saved_lower_body = read<float>(player, bindings.entity.offsets.field_a2a8247f);
        choose_resolver_yaw(owner, player, animation_state, span, shot_tick, flags, input, scratch,
                            bindings);
        const auto yaw = read<float>(player, angles + 4);
        if (saved_yaw != yaw)
            write(player, angles + 4, normalize(yaw));
        if (!(flags & 1u) && span.count > 1)
            step_history_animation(owner, player, animation_state, span, shot_tick, flags, result,
                                   bindings);
        if (seed.active_air_layer && (!bindings.configuration.reworked() ||
                                      (std::isfinite(read<float>(&owner.native, 0x4EC)) &&
                                       read<float>(&owner.native, 0x4EC) > 0.0f &&
                                       std::isfinite(read<float>(&owner.native, 0x4F0)) &&
                                       read<float>(&owner.native, 0x4F0) >= 0.0f))) {
            const auto& state = owner.native;
            const auto age = truncate((read<float>(&state, 0x4F0) / read<float>(&state, 0x4EC)) /
                                          bindings.entity.interval +
                                      0.5f);
            const auto age_time = float(age) * bindings.entity.interval;
            const auto start = float(span.last) * bindings.entity.interval - age_time;
            if ((flags & 1u) && seed.previous_update_time < start) {
                write(animation_state, 0x108, std::uint16_t(0));
                write(player, bindings.fields.pose_parameters_1caaf0a8 + 0x18, std::uint32_t(0));
                write(animation_state, 0x110, std::uint32_t(0));
                write(animation_state, 0x6C, start);
            }
            write(animation_state, 0x110, age_time - bindings.entity.interval);
        }
        write(player, bindings.entity.offsets.field_a2a8247f, saved_lower_body);
        write(player, angles + 4, saved_yaw);
    }
    write(player, bindings.entity.offsets.velocity, seed.velocity);
    return result;
}
} // namespace skeet::lagcomp

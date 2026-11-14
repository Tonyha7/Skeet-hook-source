#include "skeet/esp/extrapolation_preview.hpp"
#include <algorithm>
#include <cmath>

namespace skeet::esp {
void ExtrapolationPreview::clear() {
    const std::lock_guard lock(mutex_);
    box_.reset();
}
void ExtrapolationPreview::publish(const lagcomp::LagRecord& record, Clock::time_point now) {
    const auto& h = record.header;
    if (!(h.flags & 4U)) {
        clear();
        return;
    }
    publish(ExtrapolationPreviewBox{h.origin, h.collision_max_z}, now);
}
void ExtrapolationPreview::publish(ExtrapolationPreviewBox box, Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    box_.reset();
    if (!std::isfinite(box.height) || box.height <= 0 || box.height > 128)
        return;
    for (const auto coordinate : box.origin)
        if (!std::isfinite(coordinate))
            return;
    box_ = box;
    updated_ = now;
}
std::optional<ExtrapolationPreviewBox> ExtrapolationPreview::snapshot(Clock::time_point now) const {
    const std::lock_guard lock(mutex_);
    if (now < updated_ || now - updated_ > std::chrono::milliseconds(200))
        return {};
    return box_;
}
namespace {
struct ClipPoint {
    float x, y, w;
};
ClipPoint transform(const Projection& p, const lagcomp::Vector& v) {
    const auto& m = p.matrix;
    return {v[0] * m[0] + v[1] * m[1] + v[2] * m[2] + m[3],
            v[0] * m[4] + v[1] * m[5] + v[2] * m[6] + m[7],
            v[0] * m[12] + v[1] * m[13] + v[2] * m[14] + m[15]};
}
bool clip(ClipPoint& a, ClipPoint& b) {
    for (const auto v : {a.x, a.y, a.w, b.x, b.y, b.w})
        if (!std::isfinite(v))
            return false;
    // Clip before perspective division, including edges crossing the camera.
    const std::array<float, 5> first{a.w - 0.001f, a.w + a.x, a.w - a.x, a.w + a.y, a.w - a.y};
    const std::array<float, 5> second{b.w - 0.001f, b.w + b.x, b.w - b.x, b.w + b.y, b.w - b.y};
    float enter = 0, leave = 1;
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (first[i] < 0 && second[i] < 0)
            return false;
        if ((first[i] < 0) != (second[i] < 0)) {
            const float t = first[i] / (first[i] - second[i]);
            if (first[i] < 0)
                enter = std::max(enter, t);
            else
                leave = std::min(leave, t);
        }
    }
    if (enter > leave)
        return false;
    const auto original = a;
    const ClipPoint delta{b.x - a.x, b.y - a.y, b.w - a.w};
    a = {original.x + enter * delta.x, original.y + enter * delta.y, original.w + enter * delta.w};
    b = {original.x + leave * delta.x, original.y + leave * delta.y, original.w + leave * delta.w};
    return a.w > 0 && b.w > 0;
}
}
void draw_extrapolation_preview(ui::RenderQueue& queue, const Projection& projection,
                                const ExtrapolationPreviewBox& box) {
    if (!std::isfinite(projection.horizontal_scale) || !std::isfinite(projection.vertical_scale) ||
        projection.horizontal_scale <= 0 || projection.vertical_scale <= 0 ||
        projection.horizontal_scale > 32768 || projection.vertical_scale > 32768)
        return;
    std::array<ClipPoint, 8> corners;
    for (unsigned i = 0; i < corners.size(); ++i) {
        // Source player hull: 32 units across, with the selected record's
        // collision height so crouched forecasts retain their actual bounds.
        const lagcomp::Vector point{box.origin[0] + ((i & 1) ? 16.0f : -16.0f),
                                    box.origin[1] + ((i & 2) ? 16.0f : -16.0f),
                                    box.origin[2] + ((i & 4) ? box.height : 0.0f)};
        corners[i] = transform(projection, point);
    }
    for (unsigned i = 0; i < corners.size(); ++i)
        for (const unsigned bit : {1U, 2U, 4U}) {
            if (i & bit)
                continue;
            auto a = corners[i], b = corners[i | bit];
            if (!clip(a, b))
                continue;
            const auto screen = [&](ClipPoint p) {
                return Point{std::int32_t(std::lround((std::clamp(p.x / p.w, -1.0f, 1.0f) + 1) *
                                                      projection.horizontal_scale)),
                             std::int32_t(std::lround((1 - std::clamp(p.y / p.w, -1.0f, 1.0f)) *
                                                      projection.vertical_scale))};
            };
            const auto from = screen(a), to = screen(b);
            queue.line({from.x, from.y, to.x, to.y, 0xE64040FFu});
        }
}
}

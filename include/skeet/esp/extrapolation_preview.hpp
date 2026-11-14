#pragma once
#include "projection.hpp"
#include "skeet/lagcomp/records.hpp"
#include "skeet/render_queue.hpp"
#include <chrono>
#include <mutex>
#include <optional>

namespace skeet::esp {
struct ExtrapolationPreviewBox {
    lagcomp::Vector origin;
    float height;
};
class ExtrapolationPreview {
public:
    using Clock = std::chrono::steady_clock;
    void clear();
    void publish(const lagcomp::LagRecord&, Clock::time_point now = Clock::now());
    void publish(ExtrapolationPreviewBox, Clock::time_point now = Clock::now());
    std::optional<ExtrapolationPreviewBox> snapshot(Clock::time_point now = Clock::now()) const;

private:
    mutable std::mutex mutex_;
    std::optional<ExtrapolationPreviewBox> box_;
    Clock::time_point updated_{};
};
void draw_extrapolation_preview(ui::RenderQueue&, const Projection&,
                                const ExtrapolationPreviewBox&);
}

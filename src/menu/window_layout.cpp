#include "skeet/menu_window.hpp"
#include <bit>

namespace skeet::ui {
namespace {
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
}
void MenuWindow::update_dpi(float scale) { // 0x433B23FC
    geometry.size = scale_size(default_size, scale);
    input.scale = scale;
    input.slider_metrics = {round_layout_coordinate(40.0f * scale),
                            round_layout_coordinate(200.0f * scale),
                            round_layout_coordinate(100.0f * scale)};
    auto y = 20;
    for (auto*& tab : std::span(tabs)) {
        tab->dpi_changed(scale, input.slider_metrics);
        tab->geometry.bounds.y = y;
        tab->geometry.bounds.x = add(tab->geometry.horizontal_offset, 6);
        y = add(y, tab->geometry.bounds.height);
    }
    for (auto*& tab : std::span(tabs)) {
        tab->layout_categories();
        tab->reflow_categories();
        tab->resize(input.slider_metrics);
    }
}
void GuiDpiState::refresh(bool force) { // 0x4339F0AD
    if (setting_widget)
        setting_widget->config_loaded();
    const auto requested = scales.at(selection);
    if (!force && requested == active_scale)
        return;
    active_scale = requested;
    if (font_resources.context)
        font_resources.refresh(font_resources.context);
    for (auto* window = windows; window; window = window->next)
        window->update_dpi(active_scale);
}
}

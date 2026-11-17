#include "skeet/script/flags.hpp"
#include "detail.hpp"
#include <array>
#include <bit>
#include <cwchar>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace skeet::script {
using namespace detail;

void draw_flags(Bridge& bridge, ReferenceStrings& strings, ui::RenderQueue& queue,
                std::int32_t entity_index, Point& cursor, std::int32_t line_height) {
    if (!bridge.state || strings.entries.empty())
        return;
    const auto& api = *bridge.api;
    void* const state = bridge.state;
    CallbackFrame guard{};
    guard.active.flags = 0x67736c6a;
    guard.saved_top = api.gettop(state);
    RtlPushFrame(&guard.active);
    auto invoke = [&] {
        for (std::size_t index = 0; index < strings.entries.size(); ++index) {
            api.rawgeti(state, -10000, strings.entries[index].reference);
            api.pushinteger(state, entity_index);
            if (api.pcall(state, 1, 2, 0) != 0) {
                report_lua_error(bridge);
                continue;
            }
            if (api.toboolean(state, -2)) {
                bool advance = true;
                const auto color = strings.entries[index].color;
                if (api.isstring(state, -1)) {
                    std::size_t length = 0;
                    const auto* text = api.tolstring(state, -1, &length);
                    if (!text || length == 0) {
                        advance = false;
                    } else {
                        // Lua strings are UTF-8. msvcrt's L"%hs" decodes them
                        // through the process ANSI code page instead, which
                        // turns every Chinese character into two garbage
                        // units, so decode the bytes as UTF-8 directly.
                        std::array<wchar_t, 1026> buffer{};
                        const auto converted = MultiByteToWideChar(
                            CP_UTF8, 0, text, static_cast<int>(length), buffer.data(),
                            static_cast<int>(buffer.size() - 1));
                        if (converted > 0) {
                            static_assert(sizeof(wchar_t) == sizeof(char16_t));
                            queue.text(cursor.x, cursor.y, color, 4, 0,
                                       {reinterpret_cast<const char16_t*>(buffer.data()),
                                        std::size_t(converted)});
                        }
                    }
                } else {
                    queue.text(cursor.x, cursor.y, color, 4, 0, strings.entries[index].text);
                }
                if (advance) {
                    const auto sum = std::uint32_t(cursor.y) + std::uint32_t(line_height);
                    cursor.y = std::bit_cast<std::int32_t>(sum);
                }
            }
            api.settop(state, -3);
        }
    };
    protected_region(invoke, guard);
    api.settop(state, guard.saved_top);
    RtlPopFrame(&guard.active);
}

} // namespace skeet::script

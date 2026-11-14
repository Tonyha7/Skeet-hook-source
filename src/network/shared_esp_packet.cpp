#include "skeet/network/shared_esp.hpp"
#include <bit>
#include <cstring>

namespace skeet::network {
namespace {
// This is the native433D5F55/5FCA/6065 arithmetic. When the fractional-presence
// bit is set it returns fraction+1/32, rather than inventing a different wire
// interpretation. Original guest execution confirms this ordering.
float coordinate(std::uint32_t value) noexcept {
    const auto whole_present = value & 1u;
    const auto fraction_present = (value >> 1) & 1u;
    if (!whole_present && !fraction_present)
        return 0.0f;
    const auto whole = whole_present ? ((value >> 3) & 0x3fffu) + 1u : 0u;
    const auto magnitude = fraction_present ? ((value >> 17) & 31u) : whole;
    auto result = static_cast<float>(double(magnitude) + double(float(fraction_present)) * 0.03125);
    if (value & 4u)
        result = std::bit_cast<float>(std::bit_cast<std::uint32_t>(result) ^ 0x80000000u);
    return result;
}
}
void decrypt_shared_esp_packet(SharedEspPacket& packet, std::uint32_t key) noexcept {
    std::array<std::uint16_t, 11> words{};
    std::memcpy(words.data(), packet.bytes.data(), packet.bytes.size());
    words[10] = static_cast<std::uint16_t>(packet.sender_index);
    std::uint16_t previous_left = 0, previous_right = 0;
    for (std::size_t index = 1; index < 10; index += 2) {
        const auto saved_left = words[index], saved_right = words[index + 1];
        auto left = saved_left, right = saved_right;
        auto round_key = std::rotl(key, 1);
        for (unsigned round = 0; round < 15; ++round) {
            right = static_cast<std::uint16_t>(
                std::rotr(static_cast<std::uint16_t>(right - round_key), left & 15u) ^ left);
            round_key = std::rotl(round_key, 1);
            left = static_cast<std::uint16_t>(
                std::rotr(static_cast<std::uint16_t>(left - round_key), right & 15u) ^ right);
            round_key = std::rotl(round_key, 1);
        }
        left = static_cast<std::uint16_t>((left - std::rotl(round_key, 1)) ^ previous_left);
        right = static_cast<std::uint16_t>((right - round_key) ^ previous_right);
        words[index] = left;
        words[index + 1] = right;
        previous_left = saved_left;
        previous_right = saved_right;
    }
    std::memcpy(packet.bytes.data(), words.data(), packet.bytes.size());
    packet.sender_index =
        std::bit_cast<std::int32_t>((std::uint32_t(packet.sender_index) & 0xffff0000u) | words[10]);
}
SharedEspFields decode_shared_esp_packet(const SharedEspPacket& packet) noexcept {
    std::array<std::uint32_t, 5> words;
    std::memcpy(words.data(), packet.bytes.data(), packet.bytes.size());
    const auto x = (words[2] >> 16) | ((words[3] & 63u) << 16);
    const auto y = (words[3] >> 6) & 0x3fffffu;
    const auto z = (words[3] >> 28) | ((words[4] & 0x3ffffu) << 4);
    return {static_cast<std::uint16_t>(words[0] >> 16),
            std::bit_cast<std::int32_t>(words[1]),
            words[2] & 0x7fu,
            (words[2] >> 7) & 0x1ffu,
            {coordinate(x), coordinate(y), coordinate(z)}};
}
} // namespace skeet::network

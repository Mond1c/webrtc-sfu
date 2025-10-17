#pragma once
#include <rtc/rtc.hpp>
#include <memory>
#include <array>
#include <atomic>
#include <span>
#include <utils.h>

namespace sfu {
    struct SubscriberInfo {
        static constexpr std::size_t PADDING = 32;
        std::unique_ptr<rtc::PeerConnection> pc;
        std::array<rtc::Track *, MAX_TRACKS_PER_PUBLISHER> tracks{nullptr, nullptr};
        std::size_t publisher_index{0};
        alignas(PADDING) std::atomic<bool> active{true}; // 64 bytes is a good size

        SubscriberInfo() noexcept = default;

        explicit SubscriberInfo(std::unique_ptr<rtc::PeerConnection> pc, std::size_t pub_idx) noexcept;

        SubscriberInfo(SubscriberInfo &&) noexcept = delete;
        SubscriberInfo &operator=(SubscriberInfo &&) noexcept = delete;
        SubscriberInfo(const SubscriberInfo &) = delete;
        SubscriberInfo &operator=(const SubscriberInfo &) = delete;

        [[nodiscard]] std::span<rtc::Track* const> get_valid_tracks() const noexcept;
    };
}  // namespace sfu

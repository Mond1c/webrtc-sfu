#pragma once
#include <rtc/rtc.hpp>
#include <memory>
#include <span>
#include <utils.h>
#include <track_broadcaster.h>


namespace sfu {
    struct PublisherInfo {
        static constexpr std::size_t PADDING = 32;

        std::unique_ptr<rtc::PeerConnection> pc;
        std::string publisher_id;
        std::string stream_type;

        std::array<TrackBroadcaster *, MAX_TRACKS_PER_PUBLISHER> broadcasters{nullptr, nullptr};
        alignas(PADDING) std::atomic<std::size_t> subscriber_count{0};
        alignas(PADDING) std::atomic<bool> running{true};
        std::mutex mutex; // 3 cache lines... Can be better I think

        PublisherInfo() noexcept = default;

        explicit PublisherInfo(std::unique_ptr<rtc::PeerConnection> pc, std::string id, std::string type);

        PublisherInfo(PublisherInfo &&) = delete;
        PublisherInfo &operator=(PublisherInfo &&) = delete;
        PublisherInfo(const PublisherInfo &) = delete;
        PublisherInfo &operator=(const PublisherInfo &) = delete;

        [[nodiscard]] std::span<TrackBroadcaster * const> get_valid_broadcasters() const noexcept;
    };
}  // namespace sfu

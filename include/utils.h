#pragma once
#include <rtc/rtc.hpp>
#include <memory>

namespace sfu {
    constexpr std::size_t MAX_TRACKS_PER_PUBLISHER = 2;
    constexpr std::size_t MAX_SUBSCRIBERS_PER_TRACK = 256;
    constexpr std::size_t RTP_PACKET_SIZE = 1500;

    enum class SFUError {
        PublisherNotFound,
        NoTracksAvailable,
        SubscriberNotFound,
        ConnectionFailed,
    };

    void close_peer_connection(std::unique_ptr<rtc::PeerConnection> pc);
}

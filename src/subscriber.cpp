#include <subscriber.h>

sfu::SubscriberInfo::SubscriberInfo(std::unique_ptr<rtc::PeerConnection> pc, std::size_t pub_idx) noexcept
    : pc(std::move(pc)), publisher_index(pub_idx) {}

[[nodiscard]] std::span<rtc::Track* const> sfu::SubscriberInfo::get_valid_tracks() const noexcept {
    std::size_t count = 0;
    if (tracks[0] != nullptr) {
        ++count;
    }
    if (tracks[1] != nullptr) {
        ++count;
    }
    return {tracks.data(), count};
}

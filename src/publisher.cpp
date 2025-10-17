#include <publisher.h>

sfu::PublisherInfo::PublisherInfo(std::unique_ptr<rtc::PeerConnection> pc, std::string id, std::string type)
    : pc(std::move(pc)), publisher_id(std::move(id)), stream_type(std::move(type)) {}

[[nodiscard]] std::span<sfu::TrackBroadcaster * const> sfu::PublisherInfo::get_valid_broadcasters() const noexcept {
    std::size_t count = 0;
    if (broadcasters[0] != nullptr) {
        ++count;
    }
    if (broadcasters[1] != nullptr) {
        ++count;
    }
    return {broadcasters.data(), count};
}
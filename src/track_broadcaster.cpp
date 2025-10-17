#include <rtc/common.hpp>
#include <track_broadcaster.h>
#include <algorithm>

sfu::TrackBroadcaster::TrackBroadcaster(rtc::Track *remote_track, std::string_view publisher_id) noexcept
    : remote_track(remote_track), running(true) {

    std::println("TrackBroadcaster created for: {}", publisher_id);

    remote_track->onMessage([this](rtc::message_variant message) {
        if (!running.load(std::memory_order_relaxed)) [[unlikely]] {
            return;
        }

        if (!std::holds_alternative<rtc::binary>(message)) {
            return;
        }

        const auto& binary_data = std::get<rtc::binary>(message);

        std::lock_guard lock(mutex);
        for (auto &sub: subscribers) {
            if (sub.is_valid()) [[likely]] {
                try {
                    sub.track->send(binary_data);
                } catch (...) {
                    sub.active.store(false, std::memory_order_relaxed);
                }
            }
        }
    });
}

sfu::TrackBroadcaster::~TrackBroadcaster() noexcept {
    stop();
}

void sfu::TrackBroadcaster::stop() noexcept {
    running = false;
    std::lock_guard lock(mutex);
    subscribers.clear();
}

void sfu::TrackBroadcaster::add_subscriber(rtc::Track* track) {
    std::lock_guard lock(mutex);
    subscribers.emplace_back(track);
    std::println("Subscriber track added, total: {}", subscribers.size());
}

void sfu::TrackBroadcaster::remove_subscriber_track(const rtc::Track *track) noexcept {
    std::lock_guard lock(mutex);

    const auto sub_it = std::ranges::find_if(
        subscribers,
        [&](const SubscriberTrackRef &sub) {
            return sub.track == track;
        }
    );

    if (sub_it != subscribers.end()) {
        sub_it->active.store(false, std::memory_order_relaxed);
        subscribers.erase(sub_it);
    }

    std::println("Subscriber track removed, remaining: {}", subscribers.size());
}

[[nodiscard]] rtc::Description::Media sfu::TrackBroadcaster::get_media_description() const noexcept {
    return remote_track->description();
}

[[nodiscard]] std::size_t sfu::TrackBroadcaster::subscriber_count() const noexcept {
    std::lock_guard lock(mutex);
    return subscribers.size();
}

sfu::TrackBroadcaster* sfu::BroadcasterPool::acquire(rtc::Track* track, std::string_view publisher_id) {
    std::unique_lock lock(mutex);
    broadcasters.push_back(
        std::make_unique<TrackBroadcaster>(track, publisher_id)
    );
    return broadcasters.back().get();
}

void sfu::BroadcasterPool::release(sfu::TrackBroadcaster* broadcaster) {
    std::unique_lock lock(mutex);
    std::erase_if(broadcasters, [broadcaster](const auto &item) {
        return item.get() == broadcaster;
    });
}

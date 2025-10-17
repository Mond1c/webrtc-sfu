#pragma once
#include <rtc/rtc.hpp>
#include <string_view>
#include <list>
#include <atomic>
#include <mutex>

namespace sfu {

    struct SubscriberTrackRef {
        rtc::Track *track{nullptr};
        std::atomic<bool> active{true};

        constexpr SubscriberTrackRef() noexcept = default;

        explicit SubscriberTrackRef(rtc::Track *track) noexcept
            : track(track) {
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return track != nullptr && active.load(std::memory_order_relaxed);
        }
    };

    class TrackBroadcaster {
    public:
        TrackBroadcaster(rtc::Track *remote_track, std::string_view publisher_id) noexcept;

        TrackBroadcaster(const TrackBroadcaster &) = delete;
        TrackBroadcaster &operator=(const TrackBroadcaster &) = delete;
        TrackBroadcaster(TrackBroadcaster &&) = delete;
        TrackBroadcaster &operator=(TrackBroadcaster &&) = delete;

        ~TrackBroadcaster() noexcept;

        void stop() noexcept;

        void add_subscriber(rtc::Track *track);
        void remove_subscriber_track(const rtc::Track *track) noexcept;
        [[nodiscard]] rtc::Description::Media get_media_description() const noexcept;
        [[nodiscard]] std::size_t subscriber_count() const noexcept;
    private:
        static constexpr std::size_t PADDING = 32;
        rtc::Track *remote_track;
        mutable std::mutex mutex;
        std::list<SubscriberTrackRef> subscribers;
        alignas(PADDING) std::atomic<bool> running;
        // Now class is 128 bytes, so it takes two cache lines, and I do not have false sharing problems here
    };

    struct BroadcasterPool {
        std::list<std::unique_ptr<TrackBroadcaster> > broadcasters;
        mutable std::mutex mutex; // 64 bytes is a good size

        TrackBroadcaster *acquire(rtc::Track *track, std::string_view publisher_id);

        void release(TrackBroadcaster *broadcaster);
    };
}  // namespace sfu

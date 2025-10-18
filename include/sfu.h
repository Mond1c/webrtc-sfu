#pragma once
#include <rtc/rtc.hpp>
#include <memory>
#include <string_view>
#include <vector>
#include <mutex>
#include <expected>
#include <utils.h>
#include <track_broadcaster.h>
#include <publisher.h>
#include <subscriber.h>

namespace sfu {
    class SFU {
    public:
        SFU();
        ~SFU() noexcept;

        [[nodiscard]] std::expected<std::size_t, SFUError>
        add_publisher(std::string_view publisher_id, std::string_view stream_type);


        [[nodiscard]] std::expected<std::size_t, SFUError> add_subscriber(
            std::string_view subscriber_id,
            std::string_view publisher_id,
            std::string_view stream_type
        );

        void remove_subscriber(std::size_t subscriber_index) noexcept;

        void remove_publisher(std::string_view publisher_id, std::string_view stream_type) noexcept;
    private:
        void cleanup_publisher(std::size_t publisher_index) noexcept;
        void cleanup() noexcept;
        void setup_ice(PublisherInfo* publisher, std::size_t publisher_index) noexcept;
        void setup_ice_subscriber(SubscriberInfo* subscriber, std::size_t subscriber_index, std::string_view subscriber_id) noexcept;
        void setup_track(sfu::PublisherInfo* publisher, rtc::Description::Media& media, std::size_t broadcaster_index) noexcept;
        static void setup_track(sfu::SubscriberInfo* subscriber, sfu::TrackBroadcaster* broadcaster) noexcept;
        [[nodiscard]] std::expected<std::size_t, sfu::SFUError> find_publisher(std::string_view publisher_id, std::string_view stream_type) const noexcept;

        rtc::Configuration config;
        mutable std::mutex publisher_mutex;
        std::mutex subscriber_mutex;
        std::vector<std::unique_ptr<PublisherInfo> > publishers;
        std::vector<std::unique_ptr<SubscriberInfo> > subscribers;
        BroadcasterPool broadcaster_pool;
    };
} // namespace sfu

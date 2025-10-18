#include "publisher.h"
#include "subscriber.h"
#include "track_broadcaster.h"
#include "utils.h"
#include <memory>
#include <print>
#include <rtc/description.hpp>
#include <sfu.h>
#include <thread>

sfu::SFU::SFU() {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    this->config = std::move(config);
}

sfu::SFU::~SFU() noexcept {
    cleanup();
}

[[nodiscard]] std::expected<std::size_t, sfu::SFUError>
    sfu::SFU::add_publisher(std::string_view publisher_id, std::string_view stream_type){

    {
        std::lock_guard lock(publisher_mutex);
        for (const auto& publisher : publishers) {
            if (publisher->publisher_id == publisher_id && publisher->stream_type == stream_type) {
                std::println("Publisher already exists: {}", publisher_id);
                return std::unexpected(SFUError::ConnectionFailed); // error?
            }
        }
    }

    try {
        auto pc = std::make_unique<rtc::PeerConnection>(config);
        std::size_t publisher_index = 0;

        {
            std::lock_guard lock(publisher_mutex);
            publisher_index = publishers.size();
            publishers.emplace_back(
                std::make_unique<sfu::PublisherInfo>(
                    std::move(pc),
                    std::string(publisher_id),
                    std::string(stream_type)
                )
            );
        }

        auto* publisher = publishers[publisher_index].get();
        setup_ice(publisher, publisher_index);
        rtc::Description::Video videoMedia("video", rtc::Description::Direction::RecvOnly);
        videoMedia.addH264Codec(96);
        videoMedia.setBitrate(3000);

        setup_track(publisher, videoMedia, 0);

        if (stream_type == "webcam") {
            rtc::Description::Audio audio_media("audio", rtc::Description::Direction::RecvOnly);
            audio_media.addOpusCodec(111);
            audio_media.setBitrate(128);

            setup_track(publisher, audio_media, 1);
        }

        publisher->pc->setLocalDescription();

        std::println("Publisher added: {}_{}", publisher_id, stream_type);
        return publisher_index;
    } catch (const std::exception &e) {
        std::println(stderr, "Failed to create publisher: {}", e.what());
        return std::unexpected(SFUError::ConnectionFailed);
    }
}

[[nodiscard]] std::expected<std::size_t, sfu::SFUError> sfu::SFU::add_subscriber(
    std::string_view subscriber_id,
    std::string_view publisher_id,
    std::string_view stream_type
) {
    auto publisher_index = find_publisher(publisher_id, stream_type);
    if (!publisher_index.has_value()) {
        return publisher_index;
    }

    // Wait for tracks
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto *publisher = publishers[publisher_index.value()].get();
    auto broadcasters = publisher->get_valid_broadcasters();

    if (broadcasters.empty()) {
        std::println(stderr, "No tracks available");
        return std::unexpected(SFUError::NoTracksAvailable);
    }

    std::println("Found {} tracks", broadcasters.size());

    try {
        auto pc = std::make_unique<rtc::PeerConnection>(config);

        std::size_t subscriber_index{0}; {
            std::unique_lock lock(subscriber_mutex);
            subscriber_index = subscribers.size();
            subscribers.emplace_back(std::make_unique<SubscriberInfo>(std::move(pc), publisher_index.value()));
        }

        auto *subscriber = subscribers[subscriber_index].get();

        setup_ice_subscriber(subscriber, subscriber_index, subscriber_id);

        for (auto* broadcaster : broadcasters) {
            sfu::SFU::setup_track(subscriber, broadcaster);
        }

        publisher->subscriber_count.fetch_add(1, std::memory_order_relaxed);
        subscriber->pc->setLocalDescription();

        std::println("Subscriber added: {}", subscriber_id);
        std::println("Total subscribers: {}", publisher->subscriber_count.load());

        return subscriber_index;
    } catch (const std::exception &e) {
        std::println(stderr, "Failed to create subscriber: {}", e.what());
        return std::unexpected(SFUError::ConnectionFailed);
    }
}

void sfu::SFU::remove_subscriber(std::size_t subscriber_index) noexcept {
    if (subscriber_index >= subscribers.size()) {
        return;
    }

    auto *subscriber = subscribers[subscriber_index].get();

    if (!subscriber->active.exchange(false, std::memory_order_acquire)) {
        return;
    }

    auto *publisher = publishers[subscriber->publisher_index].get();

    for (std::size_t i = 0; i < MAX_TRACKS_PER_PUBLISHER; ++i) {
        if (subscriber->tracks[i] != nullptr &&
            publisher->broadcasters[i] != nullptr) {
            publisher->broadcasters[i]->remove_subscriber_track(subscriber->tracks[i]);
        }
    }

    publisher->subscriber_count.fetch_sub(1, std::memory_order_release);

    sfu::close_peer_connection(std::move(subscriber->pc)); // so i can move it? because it closed?
    subscriber->tracks.fill(nullptr);

    std::println("Subscriber removed, remaining: {}", publisher->subscriber_count.load());
}

void sfu::SFU::remove_publisher(std::string_view publisher_id, std::string_view stream_type) noexcept {
    std::lock_guard lock(publisher_mutex);

    for (std::size_t i = 0; i < publishers.size(); ++i) {
        if (publishers[i]->publisher_id == publisher_id &&
            publishers[i]->stream_type == stream_type) {
            cleanup_publisher(i);
            return;
        }
    }
}

void sfu::SFU::cleanup_publisher(std::size_t publisher_index) noexcept {
    if (publisher_index >= publishers.size()) {
        return;
    }

    auto *publisher = publishers[publisher_index].get();

    if (!publisher->running.exchange(false, std::memory_order_relaxed)) {
        return;
    }

    std::println("Cleaning up publisher: {}", publisher->publisher_id);

    for (auto *broadcaster: publisher->broadcasters) {
        if (broadcaster != nullptr) {
            broadcaster->stop();
            broadcaster_pool.release(broadcaster);
        }
    }

    publisher->broadcasters.fill(nullptr); {
        std::unique_lock lock(subscriber_mutex);
        for (auto &subscriber: subscribers) {
            if (subscriber->publisher_index == publisher_index &&
                subscriber->active.load(std::memory_order_relaxed)) {
                sfu::close_peer_connection(std::move(subscriber->pc)); // the same? move?
                subscriber->active.store(false, std::memory_order_relaxed);
            }
        }
    }

    sfu::close_peer_connection(std::move(publisher->pc)); // the same? move?
}

void sfu::SFU::cleanup() noexcept {
    {
        std::lock_guard lock(publisher_mutex);
        for (auto &publisher: publishers) {
            for (auto *broadcaster: publisher->broadcasters) {
                if (broadcaster != nullptr) {
                    broadcaster->stop();
                }
            }
            sfu::close_peer_connection(std::move(publisher->pc));
        }
    }
    {
        std::lock_guard lock(subscriber_mutex);
        for (auto &subscriber: subscribers) {
            sfu::close_peer_connection(std::move(subscriber->pc));
        }
    }
}

void sfu::SFU::setup_track(sfu::SubscriberInfo* subscriber, sfu::TrackBroadcaster* broadcaster) noexcept {
    auto media_desc = broadcaster->get_media_description();

    rtc::Description::Media send_media(
        media_desc.description(),
        media_desc.mid(),
        rtc::Description::Direction::SendOnly
    );

    if (media_desc.type() == "video") {
        send_media.addRtxCodec(96, 96, 90000);
        send_media.setBitrate(3000);
    } else if (media_desc.type() == "audio") {
        send_media.addRtxCodec(111, 111, 48000);
        send_media.setBitrate(128);
    }

    auto track = subscriber->pc->addTrack(send_media);

    auto *track_ptr = track.get();
    if (subscriber->tracks[0] != nullptr) {
        subscriber->tracks[1] = track_ptr;
    } else {
        subscriber->tracks[0] = track_ptr;
    }
    track->onOpen([track_ptr]() {
        track_ptr->requestKeyframe();
    });

    broadcaster->add_subscriber(track_ptr);

    std::println("Added {} track", media_desc.type());
}

void sfu::SFU::setup_ice_subscriber(sfu::SubscriberInfo* subscriber, std::size_t subscriber_index, std::string_view subscriber_id) noexcept {
    subscriber->pc->onStateChange([this, subscriber_index](rtc::PeerConnection::State state) {
        std::println("Subscriber state: {}", static_cast<int>(state));

        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            remove_subscriber(subscriber_index);
        }
    });

    subscriber->pc->onLocalCandidate([id = std::string(subscriber_id)](const rtc::Candidate &candidate) {
        std::println("Subscriber {} ICE: {}", id, std::string(candidate));
    });
}

[[nodiscard]] std::expected<std::size_t, sfu::SFUError> sfu::SFU::find_publisher(std::string_view publisher_id, std::string_view stream_type) const noexcept {
    std::size_t publisher_index = SIZE_MAX;
    {
        std::lock_guard lock(publisher_mutex);
        for (std::size_t i = 0; i < publishers.size(); ++i) {
            if (publishers[i]->publisher_id == publisher_id &&
                publishers[i]->stream_type == stream_type) {
                publisher_index = i;
                break;
            }
        }
    }

    if (publisher_index == SIZE_MAX) {
        std::println(stderr, "Publisher not found: {}", publisher_id);
        return std::unexpected(SFUError::PublisherNotFound);
    }

    return publisher_index;
}

void sfu::SFU::setup_ice(sfu::PublisherInfo* publisher, std::size_t publisher_index) noexcept {
    publisher->pc->onStateChange([this, publisher_index](rtc::PeerConnection::State state) {
        std::println("Publisher state: {}", static_cast<int>(state));
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            cleanup_publisher(publisher_index);
        }
    });

    publisher->pc->onLocalCandidate([id = publisher->publisher_id](const rtc::Candidate &candidate) {
        std::println("Publisher {} ICE: {}", id, std::string(candidate));
    });
}

void sfu::SFU::setup_track(sfu::PublisherInfo* publisher, rtc::Description::Media& media, std::size_t broadcaster_index) noexcept {
    auto track = publisher->pc->addTrack(media.description());
    track->setMediaHandler(std::make_shared<rtc::MediaHandler>());
    auto *track_ptr = track.get();

    track->onMessage(
        [this, publisher, track_ptr, broadcaster_index](const rtc::binary &message) {
            std::lock_guard lock(publisher_mutex);

            if (publisher->broadcasters[broadcaster_index] == nullptr) {
                publisher->broadcasters[broadcaster_index] = broadcaster_pool.acquire(
                    track_ptr, publisher->publisher_id);
                std::println("Video broadcaster created");
            }
        }, nullptr);
}

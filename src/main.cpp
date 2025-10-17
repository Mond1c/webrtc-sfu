#include <rtc/rtc.hpp>
#include <memory>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstring>
#include <expected>
#include <print>
#include <utils.h>
#include <track_broadcaster.h>
#include <publisher.h>
#include <subscriber.h>

using namespace sfu;

class PeerManager {
    // bad structure very heavy
public:
    PeerManager() {
        rtc::Configuration c;
        c.iceServers.emplace_back("stun:stun.l.google.com:19302");
        config = std::move(c);
    }

    ~PeerManager() noexcept {
        cleanup();
    }

    [[nodiscard]] std::expected<std::size_t, SFUError>
    add_publisher(std::string_view publisher_id, std::string_view stream_type) { {
            std::lock_guard lock(publisher_mutex);
            for (const auto &pub: publishers) {
                if (pub->publisher_id == publisher_id && pub->stream_type == stream_type) {
                    std::println("Publisher already exists: {}", publisher_id);
                    return std::unexpected(SFUError::ConnectionFailed); // error?
                }
            }
        }

        try {
            auto pc = std::make_unique<rtc::PeerConnection>(config);

            std::size_t publisher_index{0}; {
                std::unique_lock lock(publisher_mutex);
                publisher_index = publishers.size();
                publishers.emplace_back(
                    std::make_unique<PublisherInfo>(
                        std::move(pc),
                        std::string(publisher_id),
                        std::string(stream_type)
                    )
                );
            }

            auto *publisher = publishers[publisher_index].get();
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

            rtc::Description::Video videoMedia("video", rtc::Description::Direction::RecvOnly);
            videoMedia.addH264Codec(96);
            videoMedia.setBitrate(3000);

            auto video_track = publisher->pc->addTrack(videoMedia.description());

            video_track->setMediaHandler(std::make_shared<rtc::MediaHandler>());

            auto *video_track_ptr = video_track.get();

            video_track->onMessage(
                [this, publisher_index, video_track_ptr](const rtc::binary &message) {
                    auto *pub = publishers[publisher_index].get();
                    std::lock_guard lock(publisher_mutex);

                    if (pub->broadcasters[0] == nullptr) {
                        pub->broadcasters[0] = broadcaster_pool.acquire(
                            video_track_ptr, pub->publisher_id);
                        std::println("Video broadcaster created");
                    }
                }, nullptr);

            if (stream_type == "webcam") {
                rtc::Description::Audio audio_media("audio", rtc::Description::Direction::RecvOnly);
                audio_media.addOpusCodec(111);
                audio_media.setBitrate(128);

                auto audio_track = publisher->pc->addTrack(audio_media.description());
                audio_track->setMediaHandler(std::make_shared<rtc::MediaHandler>());

                auto *audio_track_ptr = audio_track.get();

                audio_track->onMessage(
                    [this, publisher_index, audio_track_ptr](const rtc::binary &message) {
                        auto *pub = publishers[publisher_index].get();
                        std::lock_guard lock(pub->mutex);

                        if (pub->broadcasters[1] == nullptr) {
                            pub->broadcasters[1] = broadcaster_pool.acquire(
                                audio_track_ptr, pub->publisher_id);
                            std::println("Audio broadcaster created");
                        }
                    },
                    nullptr
                );
            }

            publisher->pc->setLocalDescription();

            std::println("Publisher added: {}_{}", publisher_id, stream_type);
            return publisher_index;
        } catch (const std::exception &e) {
            std::println(stderr, "Failed to create publisher: {}", e.what());
            return std::unexpected(SFUError::ConnectionFailed);
        }
    }

    [[nodiscard]] std::expected<std::size_t, SFUError> addSubscriber(
        std::string_view subscriber_id,
        std::string_view publisher_id,
        std::string_view stream_type) {
        std::size_t publisher_index = SIZE_MAX; {
            std::unique_lock lock(publisher_mutex);
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

        // Wait for tracks
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto *publisher = publishers[publisher_index].get();

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
                subscribers.emplace_back(std::make_unique<SubscriberInfo>(std::move(pc), publisher_index));
            }

            auto *subscriber = subscribers[subscriber_index].get();

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

            for (std::size_t i = 0; i < broadcasters.size(); ++i) {
                auto *broadcaster = broadcasters[i];
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
                subscriber->tracks[i] = track_ptr;

                track->onOpen([track_ptr]() {
                    track_ptr->requestKeyframe();
                });

                broadcaster->add_subscriber(track_ptr);

                std::println("Added {} track", media_desc.type());
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

    void remove_subscriber(std::size_t subscriber_index) noexcept {
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

        try {
            subscriber->pc->close();
        } catch (...) {
        }

        subscriber->tracks.fill(nullptr);

        std::println("Subscriber removed, remaining: {}", publisher->subscriber_count.load());
    }

    void remove_publisher(std::string_view publisher_id, std::string_view stream_type) noexcept {
        std::unique_lock lock(publisher_mutex);

        for (std::size_t i = 0; i < publishers.size(); ++i) {
            if (publishers[i]->publisher_id == publisher_id &&
                publishers[i]->stream_type == stream_type) {
                cleanup_publisher(i);
                return;
            }
        }
    }

private:
    void cleanup_publisher(std::size_t publisher_index) noexcept {
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
                    try {
                        subscriber->pc->close();
                    } catch (...) {
                    }
                    subscriber->active.store(false, std::memory_order_relaxed);
                }
            }
        }

        try {
            publisher->pc->close();
        } catch (...) {
        }
    }

    void cleanup() noexcept { {
            std::unique_lock lock(publisher_mutex);
            for (auto &publisher: publishers) {
                for (auto *broadcaster: publisher->broadcasters) {
                    if (broadcaster != nullptr) {
                        broadcaster->stop();
                    }
                }
                try {
                    publisher->pc->close();
                } catch (...) {
                }
            }
        } {
            std::unique_lock lock(subscriber_mutex);
            for (auto &subscriber: subscribers) {
                try {
                    subscriber->pc->close();
                } catch (...) {
                }
            }
        }
    }

    rtc::Configuration config;
    std::mutex publisher_mutex;
    std::mutex subscriber_mutex;
    std::vector<std::unique_ptr<PublisherInfo> > publishers;
    std::vector<std::unique_ptr<SubscriberInfo> > subscribers;
    BroadcasterPool broadcaster_pool;
};

int main() {
    return 0;
}

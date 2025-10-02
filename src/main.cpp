#include <rtc/rtc.hpp>
#include <memory>
#include <utility>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <array>
#include <list>
#include <cstring>
#include <span>
#include <expected>
#include <print>

constexpr std::size_t MAX_TRACKS_PER_PUBLISHER = 2;
constexpr std::size_t MAX_SUBSCRIBERS_PER_TRACK = 256;
constexpr std::size_t RTP_PACKET_SIZE = 1500;

enum class SFUError {
    PublisherNotFound,
    NoTracksAvailable,
    SubscriberNotFound,
    ConnectionFailed,
};

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
    TrackBroadcaster(rtc::Track *remote_track, std::string_view publisher_id)
        : remote_track(remote_track), running(true) {
        std::println("TrackBroadcaster created for: {}", publisher_id);

        remote_track->onMessage(
            [this](const rtc::binary &message) {
                if (!running.load(std::memory_order_relaxed)) [[unlikely]] {
                    return;
                }

                std::lock_guard lock(mutex);
                for (auto &sub: subscribers) {
                    if (sub.is_valid()) [[likely]] {
                        try {
                            sub.track->send(message);
                        } catch (...) {
                            sub.active.store(false, std::memory_order_relaxed);
                        }
                    }
                }
            },
            nullptr
        );
    }

    TrackBroadcaster(const TrackBroadcaster &) = delete;

    TrackBroadcaster &operator=(const TrackBroadcaster &) = delete;

    TrackBroadcaster(TrackBroadcaster &&) = delete;

    TrackBroadcaster &operator=(TrackBroadcaster &&) = delete;

    ~TrackBroadcaster() noexcept {
        stop();
    }

    void stop() noexcept {
        running = false;
        std::lock_guard lock(mutex);
        subscribers.clear();
    }

    void add_subscriber(rtc::Track *track) {
        std::lock_guard lock(mutex);
        subscribers.emplace_back(track);
        std::println("Subscriber track added, total: {}", subscribers.size());
    }

    void remove_subscriber_track(const rtc::Track *track) noexcept {
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

    [[nodiscard]] rtc::Description::Media get_media_description() const noexcept {
        return remote_track->description();
    }

    [[nodiscard]] std::size_t subscriber_count() const noexcept {
        std::lock_guard lock(mutex);
        return subscribers.size();
    }

private:
    rtc::Track *remote_track;
    mutable std::mutex mutex;
    std::list<SubscriberTrackRef> subscribers;
    alignas(32) std::atomic<bool> running;
    // Now class is 128 bytes, so it takes two cache lines, and I do not have false sharing problems here
};

struct BroadcasterPool {
    std::list<std::unique_ptr<TrackBroadcaster> > broadcasters;
    mutable std::mutex mutex; // 64 bytes is a good size

    TrackBroadcaster *acquire(rtc::Track *track, std::string_view publisher_id) {
        std::unique_lock lock(mutex);
        broadcasters.push_back(
            std::make_unique<TrackBroadcaster>(track, publisher_id)
        );
        return broadcasters.back().get();
    }

    void release(TrackBroadcaster *broadcaster) {
        std::unique_lock lock(mutex);
        std::erase_if(broadcasters, [broadcaster](const auto &item) {
            return item.get() == broadcaster;
        });
    }
};

struct PublisherInfo {
    std::unique_ptr<rtc::PeerConnection> pc;
    std::string publisher_id;
    std::string stream_type;

    std::array<TrackBroadcaster *, MAX_TRACKS_PER_PUBLISHER> broadcasters{nullptr, nullptr};
    alignas(32) std::atomic<std::size_t> subscriber_count{0};
    alignas(32) std::atomic<bool> running{true};
    std::mutex mutex; // 3 cache lines... Can be better I think

    PublisherInfo() noexcept = default;

    explicit PublisherInfo(std::unique_ptr<rtc::PeerConnection> pc, std::string id, std::string type)
        : pc(std::move(pc)), publisher_id(std::move(id)), stream_type(std::move(type)) {
    }

    PublisherInfo(PublisherInfo &&) = delete;

    PublisherInfo &operator=(PublisherInfo &&) = delete;

    PublisherInfo(const PublisherInfo &) = delete;

    PublisherInfo &operator=(const PublisherInfo &) = delete;

    [[nodiscard]] std::span<TrackBroadcaster * const> get_valid_broadcasters() const noexcept {
        std::size_t count = 0;
        if (broadcasters[0] != nullptr) {
            ++count;
        }
        if (broadcasters[1] != nullptr) {
            ++count;
        }
        return {broadcasters.data(), count};
    }
};

struct SubscriberInfo {
    std::unique_ptr<rtc::PeerConnection> pc;
    std::array<rtc::Track *, MAX_TRACKS_PER_PUBLISHER> tracks{nullptr, nullptr};
    std::size_t publisher_index{0};
    alignas(32) std::atomic<bool> active{true}; // 64 bytes is a good size

    SubscriberInfo() noexcept = default;

    explicit SubscriberInfo(std::unique_ptr<rtc::PeerConnection> p, std::size_t pub_idx) noexcept
        : pc(std::move(p)), publisher_index(pub_idx) {
    }

    SubscriberInfo(SubscriberInfo &&) noexcept = delete;

    SubscriberInfo &operator=(SubscriberInfo &&) noexcept = delete;

    SubscriberInfo(const SubscriberInfo &) = delete;

    SubscriberInfo &operator=(const SubscriberInfo &) = delete;

    [[nodiscard]] std::span<rtc::Track * const> get_valid_tracks() const noexcept {
        std::size_t count = 0;
        if (tracks[0] != nullptr) {
            ++count;
        }
        if (tracks[1] != nullptr) {
            ++count;
        }
        return {tracks.data(), count};
    }
};

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

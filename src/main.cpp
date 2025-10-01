#include <rtc/rtc.hpp>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>

class TrackBroadcaster {
public:
    TrackBroadcaster(rtc::Track* remote_track, std::string publisher_id)
        : remote_track(remote_track), publisher_id(std::move(publisher_id)), running(true) {
        std::cout << "TrackBroadcaster created for publisher: " << publisher_id << std::endl;

        remote_track->onMessage(
            [this](rtc::binary message) {
                if (!running) return;

                std::lock_guard lock(mutex);
                for (auto* track : subscribers_tracks) {
                    try {
                        if (track && track->isOpen()) {
                            track->send(message);
                        }
                    } catch (const std::exception& e) {
                        std::cerr
                            << "Error forwarding packet: "
                            << e.what()
                            << std::endl;
                    }
                }
            },
            nullptr
        );
    }

    ~TrackBroadcaster() noexcept {
        stop();
    }

    void stop() noexcept {
        running = false;
        std::lock_guard lock(mutex);
        subscribers_tracks.clear();
    }

    void add_subscriber(rtc::Track* track) {
        std::lock_guard lock(mutex);
        subscribers_tracks.push_back(track);
        std::cout << "Added subscriber to track, total: " << subscribers_tracks.size() << std::endl;
    }

    void remove_subscriber(rtc::Track* track) {
        std::lock_guard lock(mutex);
        std::erase(subscribers_tracks, track);
        std::cout << "Removed subscriber from track, total: " << subscribers_tracks.size() << std::endl;
    }

    [[nodiscard]] rtc::Description::Media get_media_description() const noexcept {
        return remote_track->description();
    }

    rtc::Track* get_remote_track() noexcept {
        return remote_track;
    }
private:
    rtc::Track* remote_track;
    std::string publisher_id;
    std::vector<rtc::Track*> subscribers_tracks;
    std::mutex mutex;
    std::atomic<bool> running;
};

int main() {
    return 0;
}
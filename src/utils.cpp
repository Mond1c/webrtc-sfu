#include <utils.h>

void sfu::close_peer_connection(std::unique_ptr<rtc::PeerConnection> pc) {
    try {
        pc->close();
    } catch (...) {
    }
}

#include "levelup/webrtc_probe.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
static_assert(MBEDTLS_VERSION_MAJOR >= 3,
              "LevelUp Switch 1 build must use MbedTLS 3.x headers, not portlibs 2.x");
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <cstring>
#include <string>

namespace levelup {

[[gnu::used]] bool verifyRtcStackLinkage(bool instantiate) {
    if (!instantiate) {
        return false;
    }

    rtc::InitLogger(rtc::LogLevel::Warning);

    rtc::Configuration cfg;
    cfg.disableAutoNegotiation = true;
    auto pc = std::make_shared<rtc::PeerConnection>(cfg);
    auto dc = pc->createDataChannel("control");
    if (dc && dc->isOpen()) {
        dc->send("probe");
    }

    auto track = pc->addTrack(rtc::Description::Video("0", rtc::Description::Direction::RecvOnly));
    auto rtcpSession = std::make_shared<rtc::RtcpReceivingSession>();
    track->setMediaHandler(rtcpSession);

    rtc::WebSocket::Configuration wsCfg;
    wsCfg.disableTlsVerification = true;
    auto ws = std::make_shared<rtc::WebSocket>(wsCfg);
    (void)ws;

    mbedtls_ssl_context sslCtx;
    mbedtls_ssl_init(&sslCtx);
    mbedtls_ssl_free(&sslCtx);

    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_cleanup(curl);
    }

    return true;
}

void logWebRtcBuildProbe() {
    volatile bool forceLinkRef = false;
    if (forceLinkRef) {
        (void)verifyRtcStackLinkage(true);
    }

    rtc::Description::Video video("0", rtc::Description::Direction::RecvOnly);
    video.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640032");
    if (auto* rtpMap = video.rtpMap(96)) {
        rtpMap->addFeedback("nack");
        rtpMap->addFeedback("nack pli");
        rtpMap->addFeedback("ccm fir");
        rtpMap->addFeedback("goog-remb");
    }

    const std::string sdp = video.generateSdp();
    const bool hasHighProfile = sdp.find("640032") != std::string::npos;
    const bool hasNack = sdp.find("rtcp-fb:96 nack") != std::string::npos;
    const bool hasPli = sdp.find("rtcp-fb:96 nack pli") != std::string::npos;
    const bool hasFir = sdp.find("rtcp-fb:96 ccm fir") != std::string::npos;
    const bool hasRemb = sdp.find("rtcp-fb:96 goog-remb") != std::string::npos;

    nlohmann::json probeJson = {
        {"mbedtls", MBEDTLS_VERSION_STRING},
        {"curl", curl_version() ? curl_version() : "unknown"},
        {"h264HighProfile", hasHighProfile},
        {"rtcpFbNack", hasNack},
        {"rtcpFbPli", hasPli},
        {"rtcpFbFir", hasFir},
        {"rtcpFbRemb", hasRemb},
    };

    brls::Logger::info("LevelUp WebRTC M1 probe: {}", probeJson.dump());
}

} // namespace levelup

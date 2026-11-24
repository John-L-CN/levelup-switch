#pragma once

namespace levelup {

/// Logs MbedTLS / libcurl / libdatachannel / SDP rtcp-fb readiness at startup.
void logWebRtcBuildProbe();

/// Keeps core WebRTC, SCTP, ICE, SRTP, MbedTLS 3, and curl symbols linked in NRO.
[[gnu::used]] bool verifyRtcStackLinkage(bool instantiate);

} // namespace levelup

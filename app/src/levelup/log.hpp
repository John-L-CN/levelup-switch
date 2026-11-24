#pragma once

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

#ifdef LEVELUP_USE_BRLS_LOG
#include <borealis.hpp>
#endif

namespace levelup {

inline void logInfo(const std::string& msg) {
#ifdef LEVELUP_USE_BRLS_LOG
    brls::Logger::info("{}", msg);
#else
    std::fprintf(stderr, "[INFO] %s\n", msg.c_str());
#endif
}

inline void logWarn(const std::string& msg) {
#ifdef LEVELUP_USE_BRLS_LOG
    brls::Logger::warning("{}", msg);
#else
    std::fprintf(stderr, "[WARN] %s\n", msg.c_str());
#endif
}

inline void logError(const std::string& msg) {
#ifdef LEVELUP_USE_BRLS_LOG
    brls::Logger::error("{}", msg);
#else
    std::fprintf(stderr, "[ERROR] %s\n", msg.c_str());
#endif
}

inline void logDebug(const std::string& msg) {
#ifdef LEVELUP_USE_BRLS_LOG
    brls::Logger::debug("{}", msg);
#else
    std::fprintf(stderr, "[DEBUG] %s\n", msg.c_str());
#endif
}

} // namespace levelup

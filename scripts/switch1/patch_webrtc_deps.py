#!/usr/bin/env python3
"""Idempotent Horizon OS (Nintendo Switch / libnx) patcher for MbedTLS 3.6.2 and libdatachannel."""

import argparse
from pathlib import Path
import sys


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8", newline="\n")


def replace_once(path: Path, old: str, new: str, marker: str, label: str) -> None:
    text = read_text(path)
    if marker in text:
        print(f"[skip] {label}: already patched")
        return
    if old not in text:
        raise RuntimeError(f"[FAIL] {label}: anchor not found in {path}")
    write_text(path, text.replace(old, new, 1))
    print(f"[ok]   {label}")


def patch_mbedtls(mbedtls_dir: Path) -> None:
    cfg = mbedtls_dir / "include/mbedtls/mbedtls_config.h"
    text = read_text(cfg)
    changed = False
    for macro in ("MBEDTLS_SSL_DTLS_SRTP", "MBEDTLS_THREADING_PTHREAD", "MBEDTLS_THREADING_C"):
        commented = f"//#define {macro}"
        enabled = f"#define {macro}"
        if commented in text:
            text = text.replace(commented, enabled, 1)
            changed = True
        elif enabled not in text:
            raise RuntimeError(f"[FAIL] mbedtls_config.h missing {macro}")
    if changed:
        write_text(cfg, text)
        print("[ok]   mbedtls_config.h (DTLS-SRTP + pthread threading)")
    else:
        print("[skip] mbedtls_config.h: already patched")

    entropy = mbedtls_dir / "library/entropy_poll.c"
    replace_once(
        entropy,
        "#if !defined(unix) && !defined(__unix__) && !defined(__unix) && \\\n"
        "    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\\n"
        "    !defined(__HAIKU__) && !defined(__midipix__) && !defined(__MVS__)",
        "#if !defined(unix) && !defined(__unix__) && !defined(__unix) && \\\n"
        "    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\\n"
        "    !defined(__HAIKU__) && !defined(__midipix__) && !defined(__MVS__) && \\\n"
        "    !defined(__SWITCH__)",
        "!defined(__SWITCH__)",
        "mbedtls entropy_poll.c platform guard",
    )
    replace_once(
        entropy,
        "#if defined(_WIN32) && !defined(EFIX64) && !defined(EFI32)",
        "#if defined(__SWITCH__)\n"
        "#include <switch.h>\n"
        "int mbedtls_platform_entropy_poll(void *data, unsigned char *output, size_t len,\n"
        "                                  size_t *olen)\n"
        "{\n"
        "    ((void) data);\n"
        "    *olen = 0;\n"
        "    Result rc = csrngInitialize();\n"
        "    if (R_FAILED(rc)) {\n"
        "        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;\n"
        "    }\n"
        "    rc = csrngGetRandomBytes(output, len);\n"
        "    csrngExit();\n"
        "    if (R_FAILED(rc)) {\n"
        "        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;\n"
        "    }\n"
        "    *olen = len;\n"
        "    return 0;\n"
        "}\n"
        "#elif defined(_WIN32) && !defined(EFIX64) && !defined(EFI32)",
        "csrngGetRandomBytes(output, len)",
        "mbedtls entropy_poll.c Switch csrng implementation",
    )

    net_sockets = mbedtls_dir / "library/net_sockets.c"
    replace_once(
        net_sockets,
        "#if !defined(unix) && !defined(__unix__) && !defined(__unix) && \\\n"
        "    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\\n"
        "    !defined(__HAIKU__) && !defined(__midipix__)",
        "#if !defined(unix) && !defined(__unix__) && !defined(__unix) && \\\n"
        "    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\\n"
        "    !defined(__HAIKU__) && !defined(__midipix__) && \\\n"
        "    !defined(__SWITCH__)",
        "!defined(__SWITCH__)",
        "mbedtls net_sockets.c guard",
    )

    platform_util = mbedtls_dir / "library/platform_util.c"
    replace_once(
        platform_util,
        "#if (defined(_POSIX_VERSION) && _POSIX_VERSION >= 199309L) || defined(__HAIKU__)",
        "#if (defined(_POSIX_VERSION) && _POSIX_VERSION >= 199309L) || defined(__HAIKU__) || defined(__SWITCH__)",
        "defined(__SWITCH__)",
        "mbedtls platform_util.c mbedtls_ms_time guard",
    )

    timing = mbedtls_dir / "library/timing.c"
    replace_once(
        timing,
        "#if !defined(unix) && !defined(__unix__) && !defined(__unix) && \\\n"
        "    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\\n"
        "    !defined(__HAIKU__) && !defined(__midipix__)",
        "#if !defined(unix) && !defined(__unix__) && !defined(__unix) && \\\n"
        "    !defined(__APPLE__) && !defined(_WIN32) && !defined(__QNXNTO__) && \\\n"
        "    !defined(__HAIKU__) && !defined(__midipix__) && \\\n"
        "    !defined(__SWITCH__)",
        "!defined(__SWITCH__)",
        "mbedtls timing.c guard",
    )


def patch_libdatachannel(rtc_dir: Path) -> None:
    root_cmake = rtc_dir / "CMakeLists.txt"
    text = read_text(root_cmake)
    if "add_definitions(-D_GNU_SOURCE)" not in text:
        text = "add_definitions(-D_GNU_SOURCE)\n" + text
        write_text(root_cmake, text)
        print("[ok]   libdatachannel CMakeLists.txt (_GNU_SOURCE)")
    else:
        print("[skip] libdatachannel CMakeLists.txt (_GNU_SOURCE): already patched")

    replace_once(
        root_cmake,
        "elseif(USE_MBEDTLS)\n"
        "\tif(NOT TARGET MbedTLS::MbedTLS)\n"
        "\t\tfind_package(MbedTLS 3 REQUIRED)\n"
        "\tendif()",
        "elseif(USE_MBEDTLS)\n"
        "\tif(NOT TARGET MbedTLS::MbedTLS)\n"
        "\t\tfind_package(MbedTLS REQUIRED)\n"
        "\tendif()\n"
        "\tif(TARGET MbedTLS::mbedtls AND NOT TARGET MbedTLS::MbedTLS)\n"
        "\t\tadd_library(MbedTLS::MbedTLS INTERFACE IMPORTED)\n"
        "\t\ttarget_link_libraries(MbedTLS::MbedTLS INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto MbedTLS::p256m MbedTLS::everest)\n"
        "\tendif()",
        "TARGET MbedTLS::mbedtls AND NOT TARGET MbedTLS::MbedTLS",
        "libdatachannel CMakeLists.txt MbedTLS::MbedTLS alias",
    )

    poll_hpp = rtc_dir / "src/impl/pollinterrupter.hpp"
    replace_once(
        poll_hpp,
        "#ifdef _WIN32\n\tsocket_t mSock;",
        "#if defined(_WIN32) || defined(__SWITCH__)\n\tsocket_t mSock;",
        "defined(__SWITCH__)",
        "libdatachannel pollinterrupter.hpp",
    )

    poll_cpp = rtc_dir / "src/impl/pollinterrupter.cpp"
    text = read_text(poll_cpp)
    if "defined(__SWITCH__)" not in text:
        text = text.replace("#ifndef _WIN32\n#include <fcntl.h>", "#if !defined(_WIN32) && !defined(__SWITCH__)\n#include <fcntl.h>")
        text = text.replace("#ifdef _WIN32", "#if defined(_WIN32) || defined(__SWITCH__)")
        text = text.replace('getaddrinfo("localhost", "0"', 'getaddrinfo("127.0.0.1", "0"')
        write_text(poll_cpp, text)
        print("[ok]   libdatachannel pollinterrupter.cpp")
    else:
        print("[skip] libdatachannel pollinterrupter.cpp: already patched")

    sctp_cpp = rtc_dir / "src/impl/sctptransport.cpp"
    replace_once(
        sctp_cpp,
        '#include "sctptransport.hpp"',
        '#include "sctptransport.hpp"\n#include <arpa/inet.h>',
        "<arpa/inet.h>",
        "libdatachannel sctptransport.cpp",
    )

    sock_hpp = rtc_dir / "src/impl/socket.hpp"
    replace_once(
        sock_hpp,
        "#ifdef __ANDROID__\n#define NO_IFADDRS",
        "#if defined(__ANDROID__) || defined(__SWITCH__)\n#define NO_IFADDRS",
        "defined(__SWITCH__)",
        "libdatachannel socket.hpp (NO_IFADDRS)",
    )

    ws_cpp = rtc_dir / "src/impl/wstransport.cpp"
    replace_once(
        ws_cpp,
        '\tcase CLOSE: {\n\t\tPLOG_INFO << "WebSocket closed";',
        '\tcase CLOSE: {\n'
        '\t\t// SWITCH-WS-CLOSE-LOG: surface gateway close code/reason for diagnostics\n'
        '\t\tif (frame.length >= 2) {\n'
        '\t\t\tconst unsigned int closeCode =\n'
        '\t\t\t    (unsigned int)((unsigned char)frame.payload[0] << 8) |\n'
        '\t\t\t    (unsigned char)frame.payload[1];\n'
        '\t\t\tstd::string closeReason(\n'
        '\t\t\t    reinterpret_cast<const char *>(frame.payload) + 2, frame.length - 2);\n'
        '\t\t\tPLOG_WARNING << "WebSocket closed by peer: code=" << closeCode\n'
        '\t\t\t             << " reason=" << closeReason;\n'
        '\t\t} else {\n'
        '\t\t\tPLOG_WARNING << "WebSocket closed by peer without a code";\n'
        '\t\t}\n'
        '\t\tPLOG_INFO << "WebSocket closed";',
        "SWITCH-WS-CLOSE-LOG",
        "libdatachannel wstransport.cpp close code/reason log",
    )

    cand_cpp = rtc_dir / "src/candidate.cpp"
    replace_once(
        cand_cpp,
        "#else\n#include <netdb.h>",
        "#else\n// SWITCH-CANDIDATE-INCLUDES\n#include <arpa/inet.h>\n#include <netdb.h>",
        "SWITCH-CANDIDATE-INCLUDES",
        "libdatachannel candidate.cpp arpa/inet.h include",
    )
    replace_once(
        cand_cpp,
        "bool Candidate::resolve(ResolveMode mode) {\n"
        "\tPLOG_VERBOSE << \"Resolving candidate (mode=\"\n"
        "\t             << (mode == ResolveMode::Simple ? \"simple\" : \"lookup\") << \"): \" << mNode << ' '\n"
        "\t             << mService;\n\n"
        "\t// Try to resolve the node and service\n"
        "\tstruct addrinfo hints = {};\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\thints.ai_flags = AI_ADDRCONFIG;\n"
        "\tif (mTransportType == TransportType::Udp) {\n"
        "\t\thints.ai_socktype = SOCK_DGRAM;\n"
        "\t\thints.ai_protocol = IPPROTO_UDP;\n"
        "\t} else if (mTransportType != TransportType::Unknown) {\n"
        "\t\thints.ai_socktype = SOCK_STREAM;\n"
        "\t\thints.ai_protocol = IPPROTO_TCP;\n"
        "\t}\n\n"
        "\tif (mode == ResolveMode::Simple)\n"
        "\t\thints.ai_flags |= AI_NUMERICHOST;\n\n"
        "\tstruct addrinfo *result = nullptr;\n"
        "\tif (getaddrinfo(mNode.c_str(), mService.c_str(), &hints, &result) == 0) {\n"
        "\t\tfor (auto p = result; p; p = p->ai_next) {\n"
        "\t\t\tif (p->ai_family == AF_INET || p->ai_family == AF_INET6) {\n"
        "\t\t\t\tchar nodebuffer[MAX_NUMERICNODE_LEN];\n"
        "\t\t\t\tchar servbuffer[MAX_NUMERICSERV_LEN];\n"
        "\t\t\t\tif (getnameinfo(p->ai_addr, socklen_t(p->ai_addrlen), nodebuffer,\n"
        "\t\t\t\t                MAX_NUMERICNODE_LEN, servbuffer, MAX_NUMERICSERV_LEN,\n"
        "\t\t\t\t                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {\n"
        "\t\t\t\t\ttry {\n"
        "\t\t\t\t\t\tmPort = uint16_t(std::stoul(servbuffer));\n"
        "\t\t\t\t\t} catch (...) {\n"
        "\t\t\t\t\t\treturn false;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tmAddress = nodebuffer;\n"
        "\t\t\t\t\tmFamily = p->ai_family == AF_INET6 ? Family::Ipv6 : Family::Ipv4;\n"
        "\t\t\t\t\tPLOG_VERBOSE << \"Resolved candidate: \" << mAddress << ' ' << mPort;\n"
        "\t\t\t\t\tbreak;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n\n"
        "\t\tfreeaddrinfo(result);\n"
        "\t}\n\n"
        "\treturn mFamily != Family::Unresolved;\n"
        "}",
        "bool Candidate::resolve(ResolveMode mode) {\n"
        "\tPLOG_VERBOSE << \"Resolving candidate (mode=\"\n"
        "\t             << (mode == ResolveMode::Simple ? \"simple\" : \"lookup\") << \"): \" << mNode << ' '\n"
        "\t             << mService;\n\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-CANDIDATE-RESOLVE: Horizon sfdnsres getnameinfo() ignores NI_NUMERICHOST and\n"
        "\t// fails on numeric IPs, and AI_ADDRCONFIG rejects valid candidates.\n"
        "\tbool portValid = false;\n"
        "\tuint16_t parsedPort = 0;\n"
        "\tif (!mService.empty()) {\n"
        "\t\tbool allDigits = true;\n"
        "\t\tfor (unsigned char ch : mService) {\n"
        "\t\t\tif (!std::isdigit(ch)) {\n"
        "\t\t\t\tallDigits = false;\n"
        "\t\t\t\tbreak;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tif (allDigits) {\n"
        "\t\t\ttry {\n"
        "\t\t\t\tsize_t idx = 0;\n"
        "\t\t\t\tconst unsigned long val = std::stoul(mService, &idx, 10);\n"
        "\t\t\t\tif (idx == mService.size() && val <= 65535UL) {\n"
        "\t\t\t\t\tparsedPort = uint16_t(val);\n"
        "\t\t\t\t\tportValid = true;\n"
        "\t\t\t\t}\n"
        "\t\t\t} catch (...) {\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n\n"
        "\tif (portValid) {\n"
        "\t\tchar nodebuffer[INET6_ADDRSTRLEN];\n"
        "\t\tstruct in_addr in4;\n"
        "\t\tif (inet_pton(AF_INET, mNode.c_str(), &in4) == 1 &&\n"
        "\t\t    inet_ntop(AF_INET, &in4, nodebuffer, sizeof(nodebuffer)) != nullptr) {\n"
        "\t\t\tmPort = parsedPort;\n"
        "\t\t\tmAddress = nodebuffer;\n"
        "\t\t\tmFamily = Family::Ipv4;\n"
        "\t\t\tPLOG_VERBOSE << \"Resolved candidate: \" << mAddress << ' ' << mPort;\n"
        "\t\t\treturn true;\n"
        "\t\t}\n"
        "\t\tstruct in6_addr in6;\n"
        "\t\tif (inet_pton(AF_INET6, mNode.c_str(), &in6) == 1 &&\n"
        "\t\t    inet_ntop(AF_INET6, &in6, nodebuffer, sizeof(nodebuffer)) != nullptr) {\n"
        "\t\t\tmPort = parsedPort;\n"
        "\t\t\tmAddress = nodebuffer;\n"
        "\t\t\tmFamily = Family::Ipv6;\n"
        "\t\t\tPLOG_VERBOSE << \"Resolved candidate: \" << mAddress << ' ' << mPort;\n"
        "\t\t\treturn true;\n"
        "\t\t}\n"
        "\t}\n\n"
        "\tif (mode == ResolveMode::Simple)\n"
        "\t\treturn false;\n\n"
        "\tstruct addrinfo hints = {};\n"
        "\thints.ai_family = AF_INET;\n"
        "\tif (mTransportType == TransportType::Udp) {\n"
        "\t\thints.ai_socktype = SOCK_DGRAM;\n"
        "\t\thints.ai_protocol = IPPROTO_UDP;\n"
        "\t} else if (mTransportType != TransportType::Unknown) {\n"
        "\t\thints.ai_socktype = SOCK_STREAM;\n"
        "\t\thints.ai_protocol = IPPROTO_TCP;\n"
        "\t}\n\n"
        "\tstruct addrinfo *result = nullptr;\n"
        "\tif (getaddrinfo(mNode.c_str(), mService.c_str(), &hints, &result) == 0) {\n"
        "\t\tfor (auto p = result; p; p = p->ai_next) {\n"
        "\t\t\tchar nodebuffer[INET6_ADDRSTRLEN];\n"
        "\t\t\tif (p->ai_family == AF_INET) {\n"
        "\t\t\t\tconst auto *sin4 = reinterpret_cast<const struct sockaddr_in *>(p->ai_addr);\n"
        "\t\t\t\tif (inet_ntop(AF_INET, &sin4->sin_addr, nodebuffer, sizeof(nodebuffer))) {\n"
        "\t\t\t\t\tmPort = ntohs(sin4->sin_port);\n"
        "\t\t\t\t\tmAddress = nodebuffer;\n"
        "\t\t\t\t\tmFamily = Family::Ipv4;\n"
        "\t\t\t\t\tPLOG_VERBOSE << \"Resolved candidate: \" << mAddress << ' ' << mPort;\n"
        "\t\t\t\t\tbreak;\n"
        "\t\t\t\t}\n"
        "\t\t\t} else if (p->ai_family == AF_INET6) {\n"
        "\t\t\t\tconst auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(p->ai_addr);\n"
        "\t\t\t\tif (inet_ntop(AF_INET6, &sin6->sin6_addr, nodebuffer, sizeof(nodebuffer))) {\n"
        "\t\t\t\t\tmPort = ntohs(sin6->sin6_port);\n"
        "\t\t\t\t\tmAddress = nodebuffer;\n"
        "\t\t\t\t\tmFamily = Family::Ipv6;\n"
        "\t\t\t\t\tPLOG_VERBOSE << \"Resolved candidate: \" << mAddress << ' ' << mPort;\n"
        "\t\t\t\t\tbreak;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n\n"
        "\t\tfreeaddrinfo(result);\n"
        "\t}\n\n"
        "\treturn mFamily != Family::Unresolved;\n"
        "#else\n"
        "\t// Try to resolve the node and service\n"
        "\tstruct addrinfo hints = {};\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\thints.ai_flags = AI_ADDRCONFIG;\n"
        "\tif (mTransportType == TransportType::Udp) {\n"
        "\t\thints.ai_socktype = SOCK_DGRAM;\n"
        "\t\thints.ai_protocol = IPPROTO_UDP;\n"
        "\t} else if (mTransportType != TransportType::Unknown) {\n"
        "\t\thints.ai_socktype = SOCK_STREAM;\n"
        "\t\thints.ai_protocol = IPPROTO_TCP;\n"
        "\t}\n\n"
        "\tif (mode == ResolveMode::Simple)\n"
        "\t\thints.ai_flags |= AI_NUMERICHOST;\n\n"
        "\tstruct addrinfo *result = nullptr;\n"
        "\tif (getaddrinfo(mNode.c_str(), mService.c_str(), &hints, &result) == 0) {\n"
        "\t\tfor (auto p = result; p; p = p->ai_next) {\n"
        "\t\t\tif (p->ai_family == AF_INET || p->ai_family == AF_INET6) {\n"
        "\t\t\t\tchar nodebuffer[MAX_NUMERICNODE_LEN];\n"
        "\t\t\t\tchar servbuffer[MAX_NUMERICSERV_LEN];\n"
        "\t\t\t\tif (getnameinfo(p->ai_addr, socklen_t(p->ai_addrlen), nodebuffer,\n"
        "\t\t\t\t                MAX_NUMERICNODE_LEN, servbuffer, MAX_NUMERICSERV_LEN,\n"
        "\t\t\t\t                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {\n"
        "\t\t\t\t\ttry {\n"
        "\t\t\t\t\t\tmPort = uint16_t(std::stoul(servbuffer));\n"
        "\t\t\t\t\t} catch (...) {\n"
        "\t\t\t\t\t\treturn false;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tmAddress = nodebuffer;\n"
        "\t\t\t\t\tmFamily = p->ai_family == AF_INET6 ? Family::Ipv6 : Family::Ipv4;\n"
        "\t\t\t\t\tPLOG_VERBOSE << \"Resolved candidate: \" << mAddress << ' ' << mPort;\n"
        "\t\t\t\t\tbreak;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n\n"
        "\t\tfreeaddrinfo(result);\n"
        "\t}\n\n"
        "\treturn mFamily != Family::Unresolved;\n"
        "#endif\n"
        "}",
        "SWITCH-CANDIDATE-RESOLVE",
        "libdatachannel candidate.cpp resolve without getnameinfo/AI_ADDRCONFIG",
    )

    tcp_cpp = rtc_dir / "src/impl/tcptransport.cpp"
    replace_once(
        tcp_cpp,
        "#ifndef _WIN32\n#include <fcntl.h>\n#include <unistd.h>\n#endif",
        "#ifndef _WIN32\n// SWITCH-TCP-INCLUDES\n#include <arpa/inet.h>\n#include <cstdio>\n#include <fcntl.h>\n#include <unistd.h>\n#endif",
        "SWITCH-TCP-INCLUDES",
        "libdatachannel tcptransport.cpp includes",
    )
    replace_once(
        tcp_cpp,
        "\tchar node[MAX_NUMERICNODE_LEN];\n"
        "\tchar serv[MAX_NUMERICSERV_LEN];\n"
        "\tif (::getnameinfo(reinterpret_cast<struct sockaddr *>(&addr), addrlen, node,\n"
        "\t                  MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,\n"
        "\t                  NI_NUMERICHOST | NI_NUMERICSERV) != 0)\n"
        "\t\tthrow std::runtime_error(\"getnameinfo failed\");",
        "\tchar node[MAX_NUMERICNODE_LEN];\n"
        "\tchar serv[MAX_NUMERICSERV_LEN];\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-TCP-GETNAMEINFO-1\n"
        "\tif (addr.ss_family == AF_INET) {\n"
        "\t\tconst auto *sin4 = reinterpret_cast<const struct sockaddr_in *>(&addr);\n"
        "\t\tif (!inet_ntop(AF_INET, &sin4->sin_addr, node, MAX_NUMERICNODE_LEN))\n"
        "\t\t\tthrow std::runtime_error(\"inet_ntop failed\");\n"
        "\t\tstd::snprintf(serv, MAX_NUMERICSERV_LEN, \"%u\", (unsigned)ntohs(sin4->sin_port));\n"
        "\t} else if (addr.ss_family == AF_INET6) {\n"
        "\t\tconst auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(&addr);\n"
        "\t\tif (!inet_ntop(AF_INET6, &sin6->sin6_addr, node, MAX_NUMERICNODE_LEN))\n"
        "\t\t\tthrow std::runtime_error(\"inet_ntop failed\");\n"
        "\t\tstd::snprintf(serv, MAX_NUMERICSERV_LEN, \"%u\", (unsigned)ntohs(sin6->sin6_port));\n"
        "\t} else {\n"
        "\t\tthrow std::runtime_error(\"Unsupported address family\");\n"
        "\t}\n"
        "#else\n"
        "\tif (::getnameinfo(reinterpret_cast<struct sockaddr *>(&addr), addrlen, node,\n"
        "\t                  MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,\n"
        "\t                  NI_NUMERICHOST | NI_NUMERICSERV) != 0)\n"
        "\t\tthrow std::runtime_error(\"getnameinfo failed\");\n"
        "#endif",
        "SWITCH-TCP-GETNAMEINFO-1",
        "libdatachannel tcptransport.cpp getnameinfo #1",
    )
    replace_once(
        tcp_cpp,
        "\t\thints.ai_flags = AI_ADDRCONFIG;",
        "#if defined(__SWITCH__)\n"
        "\t\t// SWITCH-TCP-NO-ADDRCONFIG\n"
        "\t\thints.ai_flags = 0;\n"
        "#else\n"
        "\t\thints.ai_flags = AI_ADDRCONFIG;\n"
        "#endif",
        "SWITCH-TCP-NO-ADDRCONFIG",
        "libdatachannel tcptransport.cpp strip AI_ADDRCONFIG",
    )
    replace_once(
        tcp_cpp,
        "\t\tchar node[MAX_NUMERICNODE_LEN];\n"
        "\t\tchar serv[MAX_NUMERICSERV_LEN];\n"
        "\t\tif (getnameinfo(addr, addrlen, node, MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,\n"
        "\t\t                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {\n"
        "\t\t\tPLOG_DEBUG << \"Trying address \" << node << \":\" << serv;\n"
        "\t\t}",
        "\t\tchar node[MAX_NUMERICNODE_LEN];\n"
        "\t\tchar serv[MAX_NUMERICSERV_LEN];\n"
        "#if defined(__SWITCH__)\n"
        "\t\t// SWITCH-TCP-GETNAMEINFO-2\n"
        "\t\tbool formatted = false;\n"
        "\t\tif (addr->sa_family == AF_INET) {\n"
        "\t\t\tconst auto *sin4 = reinterpret_cast<const struct sockaddr_in *>(addr);\n"
        "\t\t\tif (inet_ntop(AF_INET, &sin4->sin_addr, node, MAX_NUMERICNODE_LEN)) {\n"
        "\t\t\t\tstd::snprintf(serv, MAX_NUMERICSERV_LEN, \"%u\", (unsigned)ntohs(sin4->sin_port));\n"
        "\t\t\t\tformatted = true;\n"
        "\t\t\t}\n"
        "\t\t} else if (addr->sa_family == AF_INET6) {\n"
        "\t\t\tconst auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(addr);\n"
        "\t\t\tif (inet_ntop(AF_INET6, &sin6->sin6_addr, node, MAX_NUMERICNODE_LEN)) {\n"
        "\t\t\t\tstd::snprintf(serv, MAX_NUMERICSERV_LEN, \"%u\", (unsigned)ntohs(sin6->sin6_port));\n"
        "\t\t\t\tformatted = true;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tif (formatted) {\n"
        "\t\t\tPLOG_DEBUG << \"Trying address \" << node << \":\" << serv;\n"
        "\t\t}\n"
        "#else\n"
        "\t\tif (getnameinfo(addr, addrlen, node, MAX_NUMERICNODE_LEN, serv, MAX_NUMERICSERV_LEN,\n"
        "\t\t                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {\n"
        "\t\t\tPLOG_DEBUG << \"Trying address \" << node << \":\" << serv;\n"
        "\t\t}\n"
        "#endif",
        "SWITCH-TCP-GETNAMEINFO-2",
        "libdatachannel tcptransport.cpp getnameinfo #2",
    )


def patch_libjuice(juice_dir: Path) -> None:
    sock_h = juice_dir / "src/socket.h"
    replace_once(
        sock_h,
        "#ifdef __ANDROID__\n#define NO_IFADDRS",
        "#if defined(__ANDROID__) || defined(__SWITCH__)\n#define NO_IFADDRS",
        "defined(__SWITCH__)",
        "libjuice socket.h (NO_IFADDRS)",
    )

    conn_poll = juice_dir / "src/conn_poll.c"
    text = read_text(conn_poll)
    if "defined(__SWITCH__)" not in text:
        text = text.replace("#ifdef _WIN32", "#if defined(_WIN32) || defined(__SWITCH__)")
        text = text.replace("#ifndef _WIN32", "#if !defined(_WIN32) && !defined(__SWITCH__)")
        text = text.replace('interrupt_config.bind_address = "localhost";', 'interrupt_config.bind_address = "127.0.0.1";')
        old_enotsock = (
            "#if defined(_WIN32) || defined(__SWITCH__)\n"
            "\t\t\tif (sockerrno == WSAENOTSOCK)\n"
            "\t\t\t\tcontinue; // prepare again as the fd has been removed\n"
            "#endif"
        )
        new_enotsock = (
            "#ifdef _WIN32\n"
            "\t\t\tif (sockerrno == WSAENOTSOCK)\n"
            "\t\t\t\tcontinue; // prepare again as the fd has been removed\n"
            "#elif defined(ENOTSOCK)\n"
            "\t\t\tif (sockerrno == ENOTSOCK)\n"
            "\t\t\t\tcontinue; // prepare again as the fd has been removed\n"
            "#endif"
        )
        if old_enotsock not in text:
            raise RuntimeError("[FAIL] libjuice conn_poll.c WSAENOTSOCK block not found")
        text = text.replace(old_enotsock, new_enotsock, 1)
        write_text(conn_poll, text)
        print("[ok]   libjuice conn_poll.c")
    else:
        print("[skip] libjuice conn_poll.c: already patched")

    udp_c = juice_dir / "src/udp.c"
    helper_anchor = (
        "\taddr_set_port((struct sockaddr *)result, port);\n"
        "\tclosesocket(sock);\n"
        "\treturn 0;\n\n"
        "error:\n"
        "\tclosesocket(sock);\n"
        "\treturn -1;\n"
        "}\n"
        "#endif\n"
    )
    helper_code = (
        "\n#if defined(__SWITCH__)\n"
        "// SWITCH-DEFAULT-ROUTE-IPV4: Horizon (libnx) lacks getifaddrs() and its bsd service\n"
        "// does not reliably implement SIOCGIFCONF; probe the kernel default-route IPv4.\n"
        "static int get_local_default_inet4(uint16_t port, struct sockaddr_in *result) {\n"
        "\tconst char *dummy_host = \"223.5.5.5\";\n"
        "\tconst uint16_t dummy_port = 53;\n\n"
        "\tstruct sockaddr_in sin;\n"
        "\tmemset(&sin, 0, sizeof(sin));\n"
        "\tsin.sin_family = AF_INET;\n"
        "\tsin.sin_port = htons(dummy_port);\n"
        "\tif (inet_pton(AF_INET, dummy_host, &sin.sin_addr) != 1) {\n"
        "\t\tJLOG_ERROR(\"SWITCH-ADDR-DIAG inet_pton failed\");\n"
        "\t\treturn -1;\n"
        "\t}\n\n"
        "\tsocket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);\n"
        "\tif (sock == INVALID_SOCKET) {\n"
        "\t\tJLOG_ERROR(\"SWITCH-ADDR-DIAG socket() failed, errno=%d\", sockerrno);\n"
        "\t\treturn -1;\n"
        "\t}\n\n"
        "\tif (connect(sock, (const struct sockaddr *)&sin, sizeof(sin))) {\n"
        "\t\tJLOG_ERROR(\"SWITCH-ADDR-DIAG connect() failed, errno=%d\", sockerrno);\n"
        "\t\tgoto error;\n"
        "\t}\n\n"
        "\tsocklen_t result_len = sizeof(*result);\n"
        "\tmemset(result, 0, sizeof(*result));\n"
        "\tif (getsockname(sock, (struct sockaddr *)result, &result_len)) {\n"
        "\t\tJLOG_ERROR(\"SWITCH-ADDR-DIAG getsockname() failed, errno=%d\", sockerrno);\n"
        "\t\tgoto error;\n"
        "\t}\n\n"
        "\tresult->sin_family = AF_INET;\n"
        "\tif (result->sin_addr.s_addr == 0) {\n"
        "\t\tJLOG_ERROR(\"SWITCH-ADDR-DIAG route address is 0.0.0.0\");\n"
        "\t\tgoto error;\n"
        "\t}\n\n"
        "\taddr_set_port((struct sockaddr *)result, port);\n"
        "\tclosesocket(sock);\n"
        "\treturn 0;\n\n"
        "error:\n"
        "\tclosesocket(sock);\n"
        "\treturn -1;\n"
        "}\n"
        "#endif\n"
    )
    replace_once(udp_c, helper_anchor, helper_anchor + helper_code, "SWITCH-DEFAULT-ROUTE-IPV4", "libjuice udp.c helper")

    inject_anchor = (
        "\taddr_record_t *current = records;\n"
        "\taddr_record_t *end = records + count;\n"
        "\tint ret = 0;\n"
    )
    inject_code = (
        "\n#if defined(__SWITCH__)\n"
        "\t// SWITCH-DEFAULT-ROUTE-INJECT: gather default-route IPv4 host candidate\n"
        "\t{\n"
        "\t\tstruct sockaddr_in sin4;\n"
        "\t\tif (get_local_default_inet4(port, &sin4) == 0 &&\n"
        "\t\t    !addr_is_local((const struct sockaddr *)&sin4) &&\n"
        "\t\t    !has_duplicate_addr((struct sockaddr *)&sin4, records, current - records)) {\n"
        "\t\t\t++ret;\n"
        "\t\t\tif (current != end) {\n"
        "\t\t\t\tmemcpy(&current->addr, &sin4, sizeof(sin4));\n"
        "\t\t\t\tcurrent->len = sizeof(sin4);\n"
        "\t\t\t\tcurrent->socktype = SOCK_DGRAM;\n"
        "\t\t\t\t++current;\n"
        "\t\t\t}\n"
        "\t\t\t{\n"
        "\t\t\t\tchar dbgbuf[64];\n"
        "\t\t\t\tif (addr_to_string((const struct sockaddr *)&sin4, dbgbuf, sizeof(dbgbuf)) > 0)\n"
        "\t\t\t\t\tJLOG_INFO(\"SWITCH-ADDR-DIAG host candidate: %s\", dbgbuf);\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "#endif\n"
    )
    replace_once(udp_c, inject_anchor, inject_anchor + inject_code, "SWITCH-DEFAULT-ROUTE-INJECT", "libjuice udp.c inject")

    replace_once(
        udp_c,
        "#ifndef NO_IFADDRS\n\tstruct ifaddrs *ifas;",
        "#if defined(__SWITCH__)\n\t// SWITCH-SKIP-SIOCGIFCONF: default-route address already gathered\n#elif !defined(NO_IFADDRS)\n\tstruct ifaddrs *ifas;",
        "SWITCH-SKIP-SIOCGIFCONF",
        "libjuice udp.c skip SIOCGIFCONF",
    )

    replace_once(
        udp_c,
        "socket_t udp_create_socket(const udp_socket_config_t *config) {\n\t// Obtain local Address",
        "socket_t udp_create_socket(const udp_socket_config_t *config) {\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-UDP-BIND-IPV4: Horizon dual-stack IPv6 (IPV6_V6ONLY=0) and sfdnsres\n"
        "\t// loopback resolution are unreliable; bind directly on IPv4 without getaddrinfo().\n"
        "\t{\n"
        "\t\tstruct sockaddr_in sin4;\n"
        "\t\tmemset(&sin4, 0, sizeof(sin4));\n"
        "\t\tsin4.sin_family = AF_INET;\n"
        "\t\tbool matched = false;\n"
        "\t\tif (!config->bind_address || config->bind_address[0] == '\\0' ||\n"
        "\t\t    strcmp(config->bind_address, \"0.0.0.0\") == 0) {\n"
        "\t\t\tsin4.sin_addr.s_addr = htonl(INADDR_ANY);\n"
        "\t\t\tmatched = true;\n"
        "\t\t} else if (inet_pton(AF_INET, config->bind_address, &sin4.sin_addr) == 1) {\n"
        "\t\t\tmatched = true;\n"
        "\t\t}\n"
        "\t\tif (matched) {\n"
        "\t\t\tstruct addrinfo fake_ai;\n"
        "\t\t\tmemset(&fake_ai, 0, sizeof(fake_ai));\n"
        "\t\t\tfake_ai.ai_family = AF_INET;\n"
        "\t\t\tfake_ai.ai_socktype = SOCK_DGRAM;\n"
        "\t\t\tfake_ai.ai_protocol = IPPROTO_UDP;\n"
        "\t\t\tfake_ai.ai_addrlen = sizeof(sin4);\n"
        "\t\t\tfake_ai.ai_addr = (struct sockaddr *)&sin4;\n"
        "\t\t\tJLOG_DEBUG(\"Opening UDP socket for IPv4 family (Switch direct bind)\");\n"
        "\t\t\treturn create_socket_for_addrinfo(config, &fake_ai);\n"
        "\t\t}\n"
        "\t}\n"
        "#endif\n"
        "\t// Obtain local Address",
        "SWITCH-UDP-BIND-IPV4",
        "libjuice udp.c direct IPv4 bind",
    )

    addr_c = juice_dir / "src/addr.c"
    replace_once(
        addr_c,
        "\tchar host[ADDR_MAX_NUMERICHOST_LEN];\n"
        "\tchar service[ADDR_MAX_NUMERICSERV_LEN];\n"
        "\tif (getnameinfo(sa, salen, host, ADDR_MAX_NUMERICHOST_LEN, service, ADDR_MAX_NUMERICSERV_LEN,\n"
        "\t                NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM)) {\n"
        "\t\tJLOG_ERROR(\"getnameinfo failed, errno=%d\", sockerrno);\n"
        "\t\tgoto error;\n"
        "\t}\n",
        "\tchar host[ADDR_MAX_NUMERICHOST_LEN];\n"
        "\tchar service[ADDR_MAX_NUMERICSERV_LEN];\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-ADDR-STRING: newlib/libnx getnameinfo() ignores NI_NUMERICHOST and runs PTR lookup\n"
        "\tif (sa->sa_family == AF_INET) {\n"
        "\t\tconst struct sockaddr_in *sin4 = (const struct sockaddr_in *)sa;\n"
        "\t\tif (!inet_ntop(AF_INET, (const void *)&sin4->sin_addr, host, ADDR_MAX_NUMERICHOST_LEN))\n"
        "\t\t\tgoto error;\n"
        "\t\tsnprintf(service, ADDR_MAX_NUMERICSERV_LEN, \"%u\", (unsigned)ntohs(sin4->sin_port));\n"
        "\t} else if (sa->sa_family == AF_INET6) {\n"
        "\t\tconst struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;\n"
        "\t\tif (!inet_ntop(AF_INET6, (const void *)&sin6->sin6_addr, host, ADDR_MAX_NUMERICHOST_LEN))\n"
        "\t\t\tgoto error;\n"
        "\t\tsnprintf(service, ADDR_MAX_NUMERICSERV_LEN, \"%u\", (unsigned)ntohs(sin6->sin6_port));\n"
        "\t} else {\n"
        "\t\tgoto error;\n"
        "\t}\n"
        "#else\n"
        "\tif (getnameinfo(sa, salen, host, ADDR_MAX_NUMERICHOST_LEN, service, ADDR_MAX_NUMERICSERV_LEN,\n"
        "\t                NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM)) {\n"
        "\t\tJLOG_ERROR(\"getnameinfo failed, errno=%d\", sockerrno);\n"
        "\t\tgoto error;\n"
        "\t}\n"
        "#endif\n",
        "SWITCH-ADDR-STRING",
        "libjuice addr.c numeric formatting",
    )

    replace_once(
        addr_c,
        "\taddr_record_t *end = records + count;\n\n"
        "\tstruct addrinfo hints;\n"
        "\tmemset(&hints, 0, sizeof(hints));\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\thints.ai_socktype = socktype;\n"
        "\thints.ai_protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;\n"
        "#ifdef AI_ADDRCONFIG\n"
        "\thints.ai_flags = AI_ADDRCONFIG;\n"
        "#endif",
        "\taddr_record_t *end = records + count;\n\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-ADDR-RESOLVE: fast-path numeric IPs without sfdnsres IPC and avoid AI_ADDRCONFIG\n"
        "\tif (hostname && hostname[0] != '\\0') {\n"
        "\t\tunsigned long port_val = 0;\n"
        "\t\tbool port_ok = true;\n"
        "\t\tif (service && service[0] != '\\0') {\n"
        "\t\t\tfor (const char *p = service; *p; ++p) {\n"
        "\t\t\t\tif (*p < '0' || *p > '9') {\n"
        "\t\t\t\t\tport_ok = false;\n"
        "\t\t\t\t\tbreak;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tport_val = port_val * 10UL + (unsigned long)(*p - '0');\n"
        "\t\t\t\tif (port_val > 65535UL) {\n"
        "\t\t\t\t\tport_ok = false;\n"
        "\t\t\t\t\tbreak;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tif (port_ok) {\n"
        "\t\t\tstruct sockaddr_in sin4;\n"
        "\t\t\tmemset(&sin4, 0, sizeof(sin4));\n"
        "\t\t\tif (inet_pton(AF_INET, hostname, &sin4.sin_addr) == 1) {\n"
        "\t\t\t\tsin4.sin_family = AF_INET;\n"
        "\t\t\t\tsin4.sin_port = htons((uint16_t)port_val);\n"
        "\t\t\t\tif (records != end) {\n"
        "\t\t\t\t\tmemcpy(&records->addr, &sin4, sizeof(sin4));\n"
        "\t\t\t\t\trecords->len = (socklen_t)sizeof(sin4);\n"
        "\t\t\t\t\trecords->socktype = socktype;\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn 1;\n"
        "\t\t\t}\n"
        "\t\t\tstruct sockaddr_in6 sin6;\n"
        "\t\t\tmemset(&sin6, 0, sizeof(sin6));\n"
        "\t\t\tif (inet_pton(AF_INET6, hostname, &sin6.sin6_addr) == 1) {\n"
        "\t\t\t\tsin6.sin6_family = AF_INET6;\n"
        "\t\t\t\tsin6.sin6_port = htons((uint16_t)port_val);\n"
        "\t\t\t\tif (records != end) {\n"
        "\t\t\t\t\tmemcpy(&records->addr, &sin6, sizeof(sin6));\n"
        "\t\t\t\t\trecords->len = (socklen_t)sizeof(sin6);\n"
        "\t\t\t\t\trecords->socktype = socktype;\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn 1;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "#endif\n\n"
        "\tstruct addrinfo hints;\n"
        "\tmemset(&hints, 0, sizeof(hints));\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-ADDR-RESOLVE-HINTS\n"
        "\thints.ai_family = AF_INET;\n"
        "\thints.ai_socktype = socktype;\n"
        "\thints.ai_protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;\n"
        "#else\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\thints.ai_socktype = socktype;\n"
        "\thints.ai_protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;\n"
        "#ifdef AI_ADDRCONFIG\n"
        "\thints.ai_flags = AI_ADDRCONFIG;\n"
        "#endif\n"
        "#endif",
        "SWITCH-ADDR-RESOLVE",
        "libjuice addr.c addr_resolve fast-path and no AI_ADDRCONFIG",
    )

    replace_once(
        addr_c,
        "bool addr_is_numeric_hostname(const char *hostname) {\n"
        "\tstruct addrinfo hints;\n"
        "\tmemset(&hints, 0, sizeof(hints));\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\thints.ai_socktype = SOCK_DGRAM;\n"
        "\thints.ai_protocol = IPPROTO_UDP;\n"
        "\thints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;\n"
        "\tstruct addrinfo *ai_list = NULL;\n"
        "\tif (getaddrinfo(hostname, \"9\", &hints, &ai_list))\n"
        "\t\treturn false;\n\n"
        "\tfreeaddrinfo(ai_list);\n"
        "\treturn true;\n"
        "}",
        "bool addr_is_numeric_hostname(const char *hostname) {\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-ADDR-IS-NUMERIC: bypass broken sfdnsres AI_NUMERICHOST check\n"
        "\tif (!hostname)\n"
        "\t\treturn false;\n"
        "\tstruct in_addr in4;\n"
        "\tif (inet_pton(AF_INET, hostname, &in4) == 1)\n"
        "\t\treturn true;\n"
        "\tstruct in6_addr in6;\n"
        "\tif (inet_pton(AF_INET6, hostname, &in6) == 1)\n"
        "\t\treturn true;\n"
        "\treturn false;\n"
        "#else\n"
        "\tstruct addrinfo hints;\n"
        "\tmemset(&hints, 0, sizeof(hints));\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\thints.ai_socktype = SOCK_DGRAM;\n"
        "\thints.ai_protocol = IPPROTO_UDP;\n"
        "\thints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;\n"
        "\tstruct addrinfo *ai_list = NULL;\n"
        "\tif (getaddrinfo(hostname, \"9\", &hints, &ai_list))\n"
        "\t\treturn false;\n\n"
        "\tfreeaddrinfo(ai_list);\n"
        "\treturn true;\n"
        "#endif\n"
        "}",
        "SWITCH-ADDR-IS-NUMERIC",
        "libjuice addr.c addr_is_numeric_hostname inet_pton",
    )

    ice_c = juice_dir / "src/ice.c"
    replace_once(
        ice_c,
        "\tif (getnameinfo((struct sockaddr *)&record->addr, record->len, candidate->hostname, 256,\n"
        "\t                candidate->service, 32, NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM)) {\n"
        "\t\tJLOG_ERROR(\"getnameinfo failed, errno=%d\", sockerrno);\n"
        "\t\treturn -1;\n"
        "\t}\n",
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-CANDIDATE-NUMERIC: emit numeric IP literals in SDP candidate lines on Horizon\n"
        "\tif (record->addr.ss_family == AF_INET) {\n"
        "\t\tconst struct sockaddr_in *sin4 = (const struct sockaddr_in *)&record->addr;\n"
        "\t\tif (!inet_ntop(AF_INET, (const void *)&sin4->sin_addr, candidate->hostname, 256)) {\n"
        "\t\t\tJLOG_ERROR(\"inet_ntop failed, errno=%d\", sockerrno);\n"
        "\t\t\treturn -1;\n"
        "\t\t}\n"
        "\t\tsnprintf(candidate->service, 32, \"%u\", (unsigned)ntohs(sin4->sin_port));\n"
        "\t} else if (record->addr.ss_family == AF_INET6) {\n"
        "\t\tconst struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&record->addr;\n"
        "\t\tif (!inet_ntop(AF_INET6, (const void *)&sin6->sin6_addr, candidate->hostname, 256)) {\n"
        "\t\t\tJLOG_ERROR(\"inet_ntop failed, errno=%d\", sockerrno);\n"
        "\t\t\treturn -1;\n"
        "\t\t}\n"
        "\t\tsnprintf(candidate->service, 32, \"%u\", (unsigned)ntohs(sin6->sin6_port));\n"
        "\t} else {\n"
        "\t\tJLOG_ERROR(\"Unsupported address family for candidate\");\n"
        "\t\treturn -1;\n"
        "\t}\n"
        "\tJLOG_DEBUG(\"SWITCH-CANDIDATE-NUMERIC %s:%s\", candidate->hostname, candidate->service);\n"
        "#else\n"
        "\tif (getnameinfo((struct sockaddr *)&record->addr, record->len, candidate->hostname, 256,\n"
        "\t                candidate->service, 32, NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM)) {\n"
        "\t\tJLOG_ERROR(\"getnameinfo failed, errno=%d\", sockerrno);\n"
        "\t\treturn -1;\n"
        "\t}\n"
        "#endif\n",
        "SWITCH-CANDIDATE-NUMERIC",
        "libjuice ice.c numeric candidate formatting",
    )

    replace_once(
        ice_c,
        "int ice_resolve_candidate(ice_candidate_t *candidate, ice_resolve_mode_t mode) {\n"
        "\tstruct addrinfo hints;\n"
        "\tmemset(&hints, 0, sizeof(hints));\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "\tif (candidate->transport != ICE_CANDIDATE_TRANSPORT_UDP) {\n"
        "\t\thints.ai_socktype = SOCK_STREAM;\n"
        "\t\thints.ai_protocol = IPPROTO_TCP;\n"
        "\t} else {\n"
        "\t\thints.ai_socktype = SOCK_DGRAM;\n"
        "\t\thints.ai_protocol = IPPROTO_UDP;\n"
        "\t}\n"
        "#ifdef AI_ADDRCONFIG\n"
        "\thints.ai_flags = AI_ADDRCONFIG;\n"
        "#endif",
        "int ice_resolve_candidate(ice_candidate_t *candidate, ice_resolve_mode_t mode) {\n"
        "#if defined(__SWITCH__)\n"
        "\t// SWITCH-ICE-RESOLVE: resolve numeric candidates via inet_pton without sfdnsres IPC\n"
        "\t{\n"
        "\t\tconst int socktype =\n"
        "\t\t    (candidate->transport != ICE_CANDIDATE_TRANSPORT_UDP) ? SOCK_STREAM : SOCK_DGRAM;\n"
        "\t\tunsigned long port_val = 0;\n"
        "\t\tbool port_ok = (candidate->service[0] != '\\0');\n"
        "\t\tfor (const char *p = candidate->service; port_ok && *p; ++p) {\n"
        "\t\t\tif (*p < '0' || *p > '9') {\n"
        "\t\t\t\tport_ok = false;\n"
        "\t\t\t\tbreak;\n"
        "\t\t\t}\n"
        "\t\t\tport_val = port_val * 10UL + (unsigned long)(*p - '0');\n"
        "\t\t\tif (port_val > 65535UL) {\n"
        "\t\t\t\tport_ok = false;\n"
        "\t\t\t\tbreak;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tif (port_ok) {\n"
        "\t\t\tstruct sockaddr_in sin4;\n"
        "\t\t\tmemset(&sin4, 0, sizeof(sin4));\n"
        "\t\t\tif (inet_pton(AF_INET, candidate->hostname, &sin4.sin_addr) == 1) {\n"
        "\t\t\t\tsin4.sin_family = AF_INET;\n"
        "\t\t\t\tsin4.sin_port = htons((uint16_t)port_val);\n"
        "\t\t\t\tcandidate->resolved.len = (socklen_t)sizeof(sin4);\n"
        "\t\t\t\tmemcpy(&candidate->resolved.addr, &sin4, sizeof(sin4));\n"
        "\t\t\t\tcandidate->resolved.socktype = socktype;\n"
        "\t\t\t\treturn 0;\n"
        "\t\t\t}\n"
        "\t\t\tstruct sockaddr_in6 sin6;\n"
        "\t\t\tmemset(&sin6, 0, sizeof(sin6));\n"
        "\t\t\tif (inet_pton(AF_INET6, candidate->hostname, &sin6.sin6_addr) == 1) {\n"
        "\t\t\t\tsin6.sin6_family = AF_INET6;\n"
        "\t\t\t\tsin6.sin6_port = htons((uint16_t)port_val);\n"
        "\t\t\t\tcandidate->resolved.len = (socklen_t)sizeof(sin6);\n"
        "\t\t\t\tmemcpy(&candidate->resolved.addr, &sin6, sizeof(sin6));\n"
        "\t\t\t\tcandidate->resolved.socktype = socktype;\n"
        "\t\t\t\treturn 0;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tif (mode != ICE_RESOLVE_MODE_LOOKUP) {\n"
        "\t\t\tJLOG_INFO(\"Failed to resolve numeric candidate: %s:%s\", candidate->hostname, candidate->service);\n"
        "\t\t\tcandidate->resolved.len = 0;\n"
        "\t\t\treturn -1;\n"
        "\t\t}\n"
        "\t}\n"
        "#endif\n"
        "\tstruct addrinfo hints;\n"
        "\tmemset(&hints, 0, sizeof(hints));\n"
        "#if defined(__SWITCH__)\n"
        "\thints.ai_family = AF_INET;\n"
        "#else\n"
        "\thints.ai_family = AF_UNSPEC;\n"
        "#endif\n"
        "\tif (candidate->transport != ICE_CANDIDATE_TRANSPORT_UDP) {\n"
        "\t\thints.ai_socktype = SOCK_STREAM;\n"
        "\t\thints.ai_protocol = IPPROTO_TCP;\n"
        "\t} else {\n"
        "\t\thints.ai_socktype = SOCK_DGRAM;\n"
        "\t\thints.ai_protocol = IPPROTO_UDP;\n"
        "\t}\n"
        "#if defined(AI_ADDRCONFIG) && !defined(__SWITCH__)\n"
        "\t// SWITCH-ICE-NO-ADDRCONFIG\n"
        "\thints.ai_flags = AI_ADDRCONFIG;\n"
        "#endif",
        "SWITCH-ICE-RESOLVE",
        "libjuice ice.c ice_resolve_candidate fast-path and no AI_ADDRCONFIG",
    )

    conn_mux_c = juice_dir / "src/conn_mux.c"
    replace_once(
        conn_mux_c,
        "\t\t\tchar host[ADDR_MAX_NUMERICHOST_LEN];\n"
        "\t\t\tif (getnameinfo((const struct sockaddr *)&src->addr, src->len, host, ADDR_MAX_NUMERICHOST_LEN, NULL, 0, NI_NUMERICHOST)) {\n"
        "\t\t\t\tJLOG_ERROR(\"getnameinfo failed, errno=%d\", sockerrno);\n"
        "\t\t\t\treturn NULL;\n"
        "\t\t\t}",
        "\t\t\tchar host[ADDR_MAX_NUMERICHOST_LEN];\n"
        "#if defined(__SWITCH__)\n"
        "\t\t\t// SWITCH-CONNMUX-NUMERIC: avoid Horizon sfdnsres getnameinfo()\n"
        "\t\t\tif (src->addr.ss_family == AF_INET) {\n"
        "\t\t\t\tconst struct sockaddr_in *sin4 = (const struct sockaddr_in *)&src->addr;\n"
        "\t\t\t\tif (!inet_ntop(AF_INET, (const void *)&sin4->sin_addr, host, ADDR_MAX_NUMERICHOST_LEN)) {\n"
        "\t\t\t\t\tJLOG_ERROR(\"inet_ntop failed, errno=%d\", sockerrno);\n"
        "\t\t\t\t\treturn NULL;\n"
        "\t\t\t\t}\n"
        "\t\t\t} else if (src->addr.ss_family == AF_INET6) {\n"
        "\t\t\t\tconst struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&src->addr;\n"
        "\t\t\t\tif (!inet_ntop(AF_INET6, (const void *)&sin6->sin6_addr, host, ADDR_MAX_NUMERICHOST_LEN)) {\n"
        "\t\t\t\t\tJLOG_ERROR(\"inet_ntop failed, errno=%d\", sockerrno);\n"
        "\t\t\t\t\treturn NULL;\n"
        "\t\t\t\t}\n"
        "\t\t\t} else {\n"
        "\t\t\t\treturn NULL;\n"
        "\t\t\t}\n"
        "#else\n"
        "\t\t\tif (getnameinfo((const struct sockaddr *)&src->addr, src->len, host, ADDR_MAX_NUMERICHOST_LEN, NULL, 0, NI_NUMERICHOST)) {\n"
        "\t\t\t\tJLOG_ERROR(\"getnameinfo failed, errno=%d\", sockerrno);\n"
        "\t\t\t\treturn NULL;\n"
        "\t\t\t}\n"
        "#endif",
        "SWITCH-CONNMUX-NUMERIC",
        "libjuice conn_mux.c numeric formatting",
    )


def patch_libsrtp(srtp_dir: Path) -> None:
    srtp_cmake = srtp_dir / "CMakeLists.txt"
    replace_once(
        srtp_cmake,
        "elseif(ENABLE_MBEDTLS)\n"
        "  target_include_directories(srtp2 PRIVATE ${MBEDTLS_INCLUDE_DIRS})\n"
        "  target_link_libraries(srtp2 ${MBEDTLS_LIBRARIES})",
        "elseif(ENABLE_MBEDTLS)\n"
        "  if(TARGET MbedTLS::MbedTLS)\n"
        "    target_link_libraries(srtp2 PRIVATE MbedTLS::MbedTLS)\n"
        "  elseif(TARGET MbedTLS::mbedcrypto)\n"
        "    target_link_libraries(srtp2 PRIVATE MbedTLS::mbedcrypto)\n"
        "  else()\n"
        "    target_include_directories(srtp2 PRIVATE ${MBEDTLS_INCLUDE_DIRS})\n"
        "    target_link_libraries(srtp2 PRIVATE ${MBEDTLS_LIBRARIES})\n"
        "  endif()",
        "elseif(TARGET MbedTLS::mbedcrypto)",
        "libsrtp CMakeLists.txt MbedTLS target link",
    )

    datatypes_h = srtp_dir / "crypto/include/datatypes.h"
    text = read_text(datatypes_h)
    if "#include <arpa/inet.h>" not in text:
        write_text(datatypes_h, "#include <arpa/inet.h>\n" + text)
        print("[ok]   libsrtp datatypes.h")
    else:
        print("[skip] libsrtp datatypes.h: already patched")


def patch_usrsctp(usrsctp_dir: Path) -> None:
    # Horizon OS (libnx) uses BSD-style sockaddrs: struct sockaddr carries
    # sa_len at offset 0 and sa_family at offset 1, and usrsctp's CMake detects
    # sa_len, so usrsctplib is compiled with HAVE_SA_LEN. usrsctp.h however only
    # switches to the "len + family" sockaddr_conn layout for Apple/BSD targets,
    # so on Switch the header declared {uint16_t family, port, addr} while the
    # library read sa_family from offset 1 -> the family came back as 0 and
    # usrsctp_bind() failed with EAFNOSUPPORT (newlib 106), killing the SCTP
    # transport right after "DTLS handshake finished".
    usrsctp_h = usrsctp_dir / "usrsctplib/usrsctp.h"
    replace_once(
        usrsctp_h,
        "#if defined(__APPLE__) || defined(__Bitrig__) || defined(__DragonFly__) || \\\n"
        "    defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)",
        "#if defined(__APPLE__) || defined(__Bitrig__) || defined(__DragonFly__) || \\\n"
        "    defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \\\n"
        "    defined(__SWITCH__)",
        "defined(__SWITCH__)\nstruct sockaddr_conn",
        "usrsctp.h sockaddr_conn BSD layout for Switch",
    )

    user_malloc = usrsctp_dir / "usrsctplib/user_malloc.h"
    replace_once(
        user_malloc,
        "#include <sys/types.h>",
        "#include <sys/types.h>\n"
        "#if defined(__SWITCH__)\n"
        "#ifndef _U_TYPES_DEFINED\n"
        "#define _U_TYPES_DEFINED\n"
        "typedef unsigned char u_char;\n"
        "typedef unsigned short u_short;\n"
        "typedef unsigned int u_int;\n"
        "typedef unsigned long u_long;\n"
        "#endif\n"
        "#endif",
        "_U_TYPES_DEFINED",
        "usrsctp user_malloc.h",
    )

    socketvar = usrsctp_dir / "usrsctplib/user_socketvar.h"
    replace_once(
        socketvar,
        "#if !defined(__DragonFly__) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(_WIN32) && !defined(__native_client__)\n"
        "#include <sys/uio.h>\n"
        "#endif",
        "#if defined(__SWITCH__)\n"
        "#include <sys/_iovec.h>\n"
        "#elif !defined(__DragonFly__) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(_WIN32) && !defined(__native_client__)\n"
        "#include <sys/uio.h>\n"
        "#endif",
        "<sys/_iovec.h>",
        "usrsctp user_socketvar.h iovec",
    )
    replace_once(
        socketvar,
        "#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(_WIN32) || defined(__native_client__)",
        "#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(_WIN32) || defined(__native_client__) || defined(__SWITCH__)",
        "defined(__native_client__) || defined(__SWITCH__)",
        "usrsctp user_socketvar.h UIO_MAXIOV",
    )

    os_userspace = usrsctp_dir / "usrsctplib/netinet/sctp_os_userspace.h"
    text = read_text(os_userspace)
    if "SWITCH-USRSCTP-PATCH" in text:
        print("[skip] usrsctp sctp_os_userspace.h: already patched")
    else:
        text = (
            "/* SWITCH-USRSCTP-PATCH */\n"
            "#if defined(__SWITCH__)\n"
            "#ifndef _GNU_SOURCE\n"
            "#define _GNU_SOURCE 1\n"
            "#endif\n"
            "#ifndef _POSIX_C_SOURCE\n"
            "#define _POSIX_C_SOURCE 200809L\n"
            "#endif\n"
            "#endif\n"
            + text
        )

        replacements = [
            (
                "#if defined(_WIN32) || defined(__native_client__)\n\n"
                "#define IFNAMSIZ 64\n\n"
                "#define random() rand()\n"
                "#define srandom(s) srand(s)",
                "#if defined(__SWITCH__)\n"
                "#include <netinet/in.h>\n"
                "#include <arpa/inet.h>\n"
                "#ifndef _U_TYPES_DEFINED\n"
                "#define _U_TYPES_DEFINED\n"
                "typedef unsigned char u_char;\n"
                "typedef unsigned short u_short;\n"
                "typedef unsigned int u_int;\n"
                "typedef unsigned long u_long;\n"
                "#endif\n"
                "#endif\n"
                "#if defined(_WIN32) || defined(__native_client__) || defined(__SWITCH__)\n\n"
                "#define IFNAMSIZ 64\n\n"
                "#if defined(_WIN32)\n"
                "#define random() rand()\n"
                "#define srandom(s) srand(s)\n"
                "#endif",
            ),
            (
                "struct iovec {\n\tsize_t len;\n\tchar *buf;\n};\n\n#define iov_base buf\n#define iov_len\tlen",
                "#if !defined(__SWITCH__)\nstruct iovec {\n\tsize_t len;\n\tchar *buf;\n};\n\n#define iov_base buf\n#define iov_len\tlen\n#endif",
            ),
            (
                "struct ifa_msghdr {\n"
                "\tuint16_t         ifam_msglen;\n"
                "\tunsigned char    ifam_version;\n"
                "\tunsigned char    ifam_type;\n"
                "\tuint32_t         ifam_addrs;\n"
                "\tuint32_t         ifam_flags;\n"
                "\tuint16_t         ifam_index;\n"
                "\tuint32_t         ifam_metric;\n"
                "};",
                "#if !defined(__SWITCH__)\n"
                "struct ifa_msghdr {\n"
                "\tuint16_t         ifam_msglen;\n"
                "\tunsigned char    ifam_version;\n"
                "\tunsigned char    ifam_type;\n"
                "\tuint32_t         ifam_addrs;\n"
                "\tuint32_t         ifam_flags;\n"
                "\tuint16_t         ifam_index;\n"
                "\tuint32_t         ifam_metric;\n"
                "};\n"
                "#endif",
            ),
            (
                "#if !defined(_WIN32) && !defined(__native_client__)\n"
                "#include <net/if.h>\n"
                "#include <netinet/in.h>\n"
                "#include <netinet/in_systm.h>\n"
                "#include <netinet/ip.h>\n"
                "#endif",
                "#if defined(__SWITCH__)\n"
                "#include <net/if.h>\n"
                "#include <netinet/in.h>\n"
                "#elif !defined(_WIN32) && !defined(__native_client__)\n"
                "#include <net/if.h>\n"
                "#include <netinet/in.h>\n"
                "#include <netinet/in_systm.h>\n"
                "#include <netinet/ip.h>\n"
                "#endif",
            ),
            (
                "#if !defined(_WIN32)\n#if defined(INET) || defined(INET6)\n#include <ifaddrs.h>",
                "#if !defined(_WIN32) && !defined(__SWITCH__)\n#if defined(INET) || defined(INET6)\n#include <ifaddrs.h>",
            ),
            (
                "#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__native_client__)",
                "#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__native_client__) || defined(__SWITCH__)",
            ),
            (
                "#if defined(__native_client__)\n#define\ttimercmp",
                "#if defined(__native_client__) || defined(__SWITCH__)\n#define\ttimercmp",
            ),
            (
                "#if defined(__APPLE__) || defined(__DragonFly__) || defined(__linux__) || defined(__native_client__) || defined(__NetBSD__) || defined(_WIN32) || defined(__Fuchsia__) || defined(__EMSCRIPTEN__)",
                "#if defined(__APPLE__) || defined(__DragonFly__) || defined(__linux__) || defined(__native_client__) || defined(__NetBSD__) || defined(_WIN32) || defined(__Fuchsia__) || defined(__EMSCRIPTEN__) || defined(__SWITCH__)",
            ),
        ]
        for old, new in replacements:
            if old not in text:
                raise RuntimeError(f"[FAIL] usrsctp sctp_os_userspace.h anchor not found:\n{old}")
            text = text.replace(old, new, 1)
        write_text(os_userspace, text)
        print("[ok]   usrsctp sctp_os_userspace.h")

    user_env = usrsctp_dir / "usrsctplib/user_environment.c"
    replace_once(
        user_env,
        "#else\n#error \"Unknown platform. Please provide platform specific RNG.\"\n#endif",
        "#elif defined(__SWITCH__)\n"
        "#include <switch.h>\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "void init_random(void) {}\n"
        "void read_random(void *buf, size_t size) {\n"
        "\tif (R_SUCCEEDED(csrngInitialize())) {\n"
        "\t\tResult rc = csrngGetRandomBytes(buf, size);\n"
        "\t\tcsrngExit();\n"
        "\t\tif (R_SUCCEEDED(rc)) {\n"
        "\t\t\treturn;\n"
        "\t\t}\n"
        "\t}\n"
        "\tuint8_t *p = (uint8_t *)buf;\n"
        "\tfor (size_t i = 0; i < size; i++) {\n"
        "\t\tp[i] = (uint8_t)(rand() & 0xFF);\n"
        "\t}\n"
        "}\n"
        "void finish_random(void) {}\n"
        "#else\n"
        "#error \"Unknown platform. Please provide platform specific RNG.\"\n"
        "#endif",
        "#elif defined(__SWITCH__)",
        "usrsctp user_environment.c RNG",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mbedtls-dir", required=True, type=Path)
    parser.add_argument("--libdatachannel-dir", required=True, type=Path)
    args = parser.parse_args()

    patch_mbedtls(args.mbedtls_dir)
    patch_libdatachannel(args.libdatachannel_dir)
    patch_libjuice(args.libdatachannel_dir / "deps/libjuice")
    patch_libsrtp(args.libdatachannel_dir / "deps/libsrtp")
    patch_usrsctp(args.libdatachannel_dir / "deps/usrsctp")
    print("All Switch 1 WebRTC dependency patches applied cleanly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

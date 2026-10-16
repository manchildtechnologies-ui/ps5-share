// PS5 Share Hub Discord uploader (PS5 backend).
// Ported from the legacy SDL/ImGui PS5 Share app's webhook.cpp: libcurl
// streams files over TLS with verification, on the title's SceNet pool, with
// the DNS-over-UDP resolver and poll/select/send/recv/fcntl shims. Only the
// namespace, header paths and settings API names changed; the transfer,
// retry and diagnostics logic is verbatim.
#include "share/uploader.hpp"
#include "share/settings.hpp"

#include "core/save_file.hpp"

#ifdef HUI_PS5_BUILD

#include <curl/curl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" int sceNetInit(void);
extern "C" int sceNetPoolCreate(const char* name, int size, int flags);
extern "C" int sceNetPoolDestroy(int poolId);
/* Title I/O shim instrumentation; worker thread only. */
extern "C" unsigned long ps5share_poll_calls = 0;
extern "C" unsigned long ps5share_poll_ms = 0;
extern "C" unsigned long ps5share_select_calls = 0;
extern "C" unsigned long ps5share_select_ms = 0;
extern "C" unsigned long ps5share_send_calls = 0;
extern "C" unsigned long ps5share_send_eagain = 0;
extern "C" unsigned long long ps5share_send_bytes = 0;
extern "C" unsigned long ps5share_send_ms = 0;
extern "C" unsigned long ps5share_recv_calls = 0;
extern "C" unsigned long ps5share_recv_ms = 0;
extern "C" unsigned long ps5share_fcntl_nb = 0;
extern "C" unsigned long ps5share_fcntl_nbio_ok = 0;
extern "C" unsigned long ps5share_fcntl_getfl_fail = 0;
extern "C" unsigned long ps5share_fcntl_getfl_emul = 0;

namespace hui {
namespace share {
namespace {
std::atomic<size_t> lastCurlAllocationFailure{0};
std::atomic<unsigned> curlAllocationFailureCount{0};
std::atomic<unsigned long long> mediaReadMicros{0};
std::atomic<unsigned> mediaReadCalls{0};
std::atomic<int> lastSndBuf{-1};
std::atomic<bool> curlInitialized{false};
std::atomic<int> sceNetInitResult{-9999};
std::atomic<int> sceNetPoolResult{-9999};
std::atomic<int> sceNetPoolId{-1};

void recordCurlAllocationFailure(size_t requested) {
    lastCurlAllocationFailure.store(requested);
    curlAllocationFailureCount.fetch_add(1);
}

void* curlTrackedMalloc(size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr && size != 0) recordCurlAllocationFailure(size);
    return ptr;
}

void curlTrackedFree(void* ptr) {
    std::free(ptr);
}

void* curlTrackedRealloc(void* ptr, size_t size) {
    void* resized = std::realloc(ptr, size);
    if (!resized && size != 0) recordCurlAllocationFailure(size);
    return resized;
}

char* curlTrackedStrdup(const char* value) {
    if (!value) return nullptr;
    const size_t length = std::strlen(value);
    if (length == std::numeric_limits<size_t>::max()) {
        recordCurlAllocationFailure(length);
        return nullptr;
    }
    auto* copy = static_cast<char*>(curlTrackedMalloc(length + 1));
    if (copy) std::memcpy(copy, value, length + 1);
    return copy;
}

void* curlTrackedCalloc(size_t count, size_t size) {
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
        recordCurlAllocationFailure(std::numeric_limits<size_t>::max());
        return nullptr;
    }
    const size_t requested = count * size;
    void* ptr = std::calloc(count, size);
    if (!ptr && requested != 0) recordCurlAllocationFailure(requested);
    return ptr;
}

const char* caBundlePath() {
    static constexpr const char* candidates[] = {
        "/app0/assets/cacert.pem",
        "/download0/PS5Share/cacert.pem",
        "/data/PS5Share/cacert.pem",
    };
    for (const char* candidate : candidates) {
        if (access(candidate, R_OK) == 0) return candidate;
    }
    return candidates[0];
}
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
constexpr const char* kBuildTag = "fw-hub31";
/* Pin TLS 1.2 exactly: no post-handshake ticket/key-update reads mid-transfer. */
constexpr long kTlsVersion = CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2;

struct TransferContext {
    std::atomic<double>* fraction;
    std::atomic<bool>* cancel;
};

struct FileStream {
    int fd{-1};
    const char* mem{nullptr};
    size_t size{0};
    size_t pos{0};
    bool useMem{false};
};

size_t discardResponse(char*, size_t size, size_t count, void*) {
    return size * count;
}

/* Bounded capture of the HTTPS response body so Discord rejections explain themselves. */
struct ResponseCapture {
    char data[2048]{};
    size_t len{0};
};

size_t captureResponse(char* ptr, size_t size, size_t count, void* opaque) {
    if (size != 0 && count > static_cast<size_t>(-1) / size) return 0;
    const size_t total = size * count;
    auto* capture = static_cast<ResponseCapture*>(opaque);
    if (capture && ptr && total > 0 && capture->len < sizeof(capture->data) - 1) {
        const size_t room = sizeof(capture->data) - 1 - capture->len;
        const size_t copy = total < room ? total : room;
        std::memcpy(capture->data + capture->len, ptr, copy);
        capture->len += copy;
        capture->data[capture->len] = '\0';
    }
    return total;
}

std::string responsePreview(const ResponseCapture& capture, size_t maxChars) {
    const size_t len = capture.len < maxChars ? capture.len : maxChars;
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(capture.data[i]);
        out.push_back(ch < 0x20 && ch != '\n' && ch != '\t' ? ' ' : capture.data[i]);
    }
    return out;
}

size_t readMedia(char* buffer, size_t size, size_t count, void* opaque) {
    auto* stream = static_cast<FileStream*>(opaque);
    if (size != 0 && count > static_cast<size_t>(-1) / size) return CURL_READFUNC_ABORT;
    const size_t capacity = size * count;
    const auto start = std::chrono::steady_clock::now();
    size_t result = CURL_READFUNC_ABORT;
    if (stream->useMem) {
        const size_t left = stream->size - stream->pos;
        result = left < capacity ? left : capacity;
        if (result > 0) std::memcpy(buffer, stream->mem + stream->pos, result);
        stream->pos += result;
    } else {
        for (;;) {
            const ssize_t got = read(stream->fd, buffer, capacity);
            if (got < 0 && errno == EINTR) continue;
            result = got < 0 ? CURL_READFUNC_ABORT : static_cast<size_t>(got);
            break;
        }
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    mediaReadMicros.fetch_add(static_cast<unsigned long long>(micros));
    mediaReadCalls.fetch_add(1);
    return result;
}

int seekMedia(void* opaque, curl_off_t offset, int origin) {
    auto* stream = static_cast<FileStream*>(opaque);
    if (stream->useMem) {
        const long long base = origin == SEEK_SET ? 0 :
            origin == SEEK_CUR ? static_cast<long long>(stream->pos) :
            origin == SEEK_END ? static_cast<long long>(stream->size) : -1;
        if (base < 0) return CURL_SEEKFUNC_FAIL;
        const long long target = base + static_cast<long long>(offset);
        if (target < 0 || target > static_cast<long long>(stream->size)) {
            return CURL_SEEKFUNC_FAIL;
        }
        stream->pos = static_cast<size_t>(target);
        return CURL_SEEKFUNC_OK;
    }
    int whence = origin == SEEK_SET ? SEEK_SET : origin == SEEK_CUR ? SEEK_CUR :
                 origin == SEEK_END ? SEEK_END : -1;
    if (whence < 0 || lseek(stream->fd, static_cast<off_t>(offset), whence) < 0) {
        return CURL_SEEKFUNC_FAIL;
    }
    return CURL_SEEKFUNC_OK;
}

void closeMedia(void* opaque) {
    auto* stream = static_cast<FileStream*>(opaque);
    if (stream->fd >= 0) close(stream->fd);
    delete stream;
}

int transferProgress(void* opaque, curl_off_t totalDown, curl_off_t nowDown,
                     curl_off_t totalUp, curl_off_t nowUp) {
    (void)totalDown;
    (void)nowDown;
    auto* context = static_cast<TransferContext*>(opaque);
    if (context->cancel->load()) return 1;
    if (totalUp > 0) context->fraction->store(
        static_cast<double>(nowUp) / static_cast<double>(totalUp));
    return 0;
}

/* Plain-data debug ring: worker thread only, no locks, no dynamic init. */
constexpr size_t kCurlDebugCap = 2048;
char curlDebugBuf[kCurlDebugCap];
size_t curlDebugLen = 0;

void clearCurlDebugTrace() {
    curlDebugLen = 0;
    curlDebugBuf[0] = '\0';
}

/* TLS certificate dumps flood the trace and push failures off-screen; drop them. */
bool isCertSpam(const char* data, size_t size) {
    static const char* markers[] = {
        "Server certificate", "cert. version", "serial number", "issuer name",
        "subject name", "issued ", "expires on", "signed using", "key size",
        "basic constraints", "subject alt name", "dNSName", "key usage",
        "certificate policies",
    };
    for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++) {
        const size_t mlen = std::strlen(markers[m]);
        if (mlen == 0 || mlen > size) continue;
        for (size_t i = 0; i + mlen <= size; i++) {
            if (std::memcmp(data + i, markers[m], mlen) == 0) return true;
        }
    }
    return false;
}

int curlDebugCallback(CURL*, curl_infotype type, char* data, size_t size, void*) {
    if (type != CURLINFO_TEXT || !data || size == 0) return 0;
    if (isCertSpam(data, size)) return 0;
    size_t copyLen = size;
    bool redacted = false;
    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] == ':' && data[i + 1] == '/' && data[i + 2] == '/') {
            copyLen = i;
            redacted = true;
            break;
        }
    }
    static constexpr char kRedacted[] = "://[redacted]";
    static constexpr size_t kRedactedLen = sizeof(kRedacted) - 1;
    size_t need = copyLen + (redacted ? kRedactedLen : 0);
    if (need >= kCurlDebugCap) {
        copyLen = kCurlDebugCap - 1 - (redacted ? kRedactedLen : 0);
        need = copyLen + (redacted ? kRedactedLen : 0);
    }
    if (curlDebugLen + need >= kCurlDebugCap) {
        const size_t drop = curlDebugLen + need - (kCurlDebugCap - 1);
        std::memmove(curlDebugBuf, curlDebugBuf + drop, curlDebugLen - drop);
        curlDebugLen -= drop;
    }
    std::memcpy(curlDebugBuf + curlDebugLen, data, copyLen);
    curlDebugLen += copyLen;
    if (redacted) {
        std::memcpy(curlDebugBuf + curlDebugLen, kRedacted, kRedactedLen);
        curlDebugLen += kRedactedLen;
    }
    curlDebugBuf[curlDebugLen] = '\0';
    return 0;
}

std::string curlDebugTail(size_t maxChars) {
    if (curlDebugLen <= maxChars) return std::string(curlDebugBuf, curlDebugLen);
    return std::string(curlDebugBuf + (curlDebugLen - maxChars), maxChars);
}

std::string saveDiagnosticsPath(const char* name, const std::string& content) {
    static constexpr const char* dirs[] = {
        "/download0/PS5Share", "/data/PS5Share", ".", "/tmp", "/data/homebrew/PPSA99992",
    };
    for (const char* dir : dirs) {
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) continue;
        // Plain open, no openat/O_CLOEXEC/O_NOFOLLOW: that pattern faults
        // under this title's clean-room libc (see runFileUpload).
        const std::string full = std::string(dir) + "/" + name;
        const int fd = open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) continue;
        const char* data = content.data();
        size_t left = content.size();
        bool ok = true;
        while (left > 0) {
            const ssize_t wrote = write(fd, data, left);
            if (wrote < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (wrote == 0) { ok = false; break; }
            data += wrote;
            left -= static_cast<size_t>(wrote);
        }
        if (close(fd) != 0) ok = false;
        if (ok) return std::string(dir) + "/" + name;
    }
    return std::string();
}

std::string transferFailureDetails(CURL* curl, CURLcode result, long long totalMs,
                                   const char* what, const char* curlError,
                                   const std::string& debugTail) {
    curl_off_t dns = 0;
    curl_off_t tcp = 0;
    curl_off_t tls = 0;
    curl_off_t uploaded = 0;
    long systemError = 0;
    if (curl) {
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &dns);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &tcp);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &tls);
        curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
        curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &systemError);
    }

    const char* phase = "before DNS completed";
    if (tls > 0) phase = "after TLS handshake";
    else if (tcp > 0) phase = "during TLS handshake";
    else if (dns > 0) phase = "during TCP connection setup";

    std::string message = what;
    message += ": curl ";
    message += std::to_string(static_cast<int>(result));
    message += " (";
    message += curl_easy_strerror(result);
    message += "). Phase: ";
    message += phase;
    message += "; DNS ";
    message += std::to_string(static_cast<long long>(dns / 1000));
    message += " ms, TCP ";
    message += std::to_string(static_cast<long long>(tcp / 1000));
    message += " ms, TLS ";
    message += std::to_string(static_cast<long long>(tls / 1000));
    message += " ms, sent ";
    message += std::to_string(static_cast<long long>(uploaded));
    message += " B; total ";
    message += std::to_string(totalMs);
    message += " ms";
    if (systemError != 0) {
        message += "; system errno ";
        message += std::to_string(systemError);
    }
    const unsigned allocationFailures = curlAllocationFailureCount.load();
    message += "; libcurl allocation failures ";
    message += std::to_string(allocationFailures);
    if (allocationFailures != 0) {
        message += ", last request ";
        message += std::to_string(static_cast<unsigned long long>(
            lastCurlAllocationFailure.load()));
        message += " B";
    }
    message += "; SceNet init ";
    message += std::to_string(sceNetInitResult.load());
    message += ", pool result ";
    message += std::to_string(sceNetPoolResult.load());
    message += "; sndbuf ";
    message += std::to_string(lastSndBuf.load());
    message += "; poll-shim ";
    message += std::to_string(ps5share_poll_calls);
    message += "/";
    message += std::to_string(ps5share_poll_ms);
    message += "ms sel ";
    message += std::to_string(ps5share_select_calls);
    message += "/";
    message += std::to_string(ps5share_select_ms);
    message += "ms snd ";
    message += std::to_string(ps5share_send_calls);
    message += "/";
    message += std::to_string(ps5share_send_bytes);
    message += "B/";
    message += std::to_string(ps5share_send_eagain);
    message += "ea/";
    message += std::to_string(ps5share_send_ms);
    message += "ms rcv ";
    message += std::to_string(ps5share_recv_calls);
    message += "/";
    message += std::to_string(ps5share_recv_ms);
    message += "ms; nbio ";
    message += std::to_string(ps5share_fcntl_nb);
    message += "/";
    message += std::to_string(ps5share_fcntl_nbio_ok);
    message += " gf ";
    message += std::to_string(ps5share_fcntl_getfl_fail);
    message += "/";
    message += std::to_string(ps5share_fcntl_getfl_emul);
    message += "; build ";
    message += kBuildTag;
    if (curlError && curlError[0]) {
        message += "; curl says: ";
        message += curlError;
    }
    if (!debugTail.empty()) {
        message += "; trace: ";
        message += debugTail;
    }
    return message;
}

bool option(CURL* curl, CURLoption key, long value) {
    return curl_easy_setopt(curl, key, value) == CURLE_OK;
}

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            out.push_back(ch < 0x20 ? ' ' : static_cast<char>(ch));
        }
    }
    return out;
}

/* Minimal best-effort text post; mirrors the test setup without diagnostics. */
bool postTextPayload(const std::string& url, const std::string& json) {
    if (!curlInitialized.load()) return false; // curl is never initialized; see net_init_stage.
    CURL* curl = curl_easy_init();
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: */*");
    const bool configured = curl && headers &&
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(json.size())) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath()) == CURLE_OK &&
        option(curl, CURLOPT_POST, 1L) &&
        option(curl, CURLOPT_SSL_VERIFYPEER, 1L) &&
        option(curl, CURLOPT_SSL_VERIFYHOST, 2L) &&
        option(curl, CURLOPT_SSLVERSION, kTlsVersion) &&
        option(curl, CURLOPT_FOLLOWLOCATION, 0L) &&
        option(curl, CURLOPT_MAXREDIRS, 0L) &&
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") == CURLE_OK &&
        option(curl, CURLOPT_NOSIGNAL, 1L) &&
        option(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4) &&
        option(curl, CURLOPT_CONNECTTIMEOUT, 30L) &&
        option(curl, CURLOPT_TIMEOUT, 45L) &&
        option(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) &&
        option(curl, CURLOPT_LOW_SPEED_TIME, 15L) &&
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse) == CURLE_OK;
    const CURLcode result = configured ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    long response = 0;
    if (curl && result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    return result == CURLE_OK && response >= 200 && response < 300;
}

/* Large send buffer, real non-blocking mode, and bounded I/O timeouts.
 * SceNet ignores SOCK_NONBLOCK at socket() time (so curl's sockets stay
 * blocking), and fcntl(O_NONBLOCK) never engages either; ioctl(FIONBIO)
 * is applied here instead, with timeouts as insurance if it is ignored. */
int sndBufCallback(void*, curl_socket_t sock, curlsocktype) {
    static constexpr int kWant = 4 * 1024 * 1024;
    int actual = -2;
    socklen_t optionLen = sizeof(actual);
    ::setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_SNDBUF, &kWant, sizeof(kWant));
    if (::getsockopt(static_cast<int>(sock), SOL_SOCKET, SO_SNDBUF, &actual, &optionLen) == 0) {
        lastSndBuf.store(actual);
    } else {
        lastSndBuf.store(-2);
    }
    ++ps5share_fcntl_nb;
    int on = 1;
    if (::ioctl(static_cast<int>(sock), FIONBIO, &on) == 0) ++ps5share_fcntl_nbio_ok;
    struct timeval timeout {};
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    ::setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return CURL_SOCKOPT_OK;
}
} // namespace

void resetIoStats() {
    ps5share_poll_calls = 0;
    ps5share_poll_ms = 0;
    ps5share_select_calls = 0;
    ps5share_select_ms = 0;
    ps5share_send_calls = 0;
    ps5share_send_eagain = 0;
    ps5share_send_bytes = 0;
    ps5share_send_ms = 0;
    ps5share_recv_calls = 0;
    ps5share_recv_ms = 0;
    ps5share_fcntl_nb = 0;
    ps5share_fcntl_nbio_ok = 0;
    ps5share_fcntl_getfl_fail = 0;
    ps5share_fcntl_getfl_emul = 0;
}

/* ---- Sequential blocking HTTPS client ------------------------------------
 * SceNet sockets have no working non-blocking mode (SOCK_NONBLOCK ignored,
 * fcntl never engaged, ioctl(FIONBIO) fails), so curl's EAGAIN-driven state
 * machine wedges mid-upload. This client uses plain blocking I/O in strict
 * order instead: connect, TLS handshake, full request, then the response. */

struct TlsSession {
    mbedtls_net_context net{};
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config conf{};
    mbedtls_x509_crt cacert{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_entropy_context entropy{};
    int fd{-1};
    bool sslReady{false};

    ~TlsSession() { close(); }

    void close() {
        if (sslReady) {
            mbedtls_ssl_free(&ssl);
            sslReady = false;
        }
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&cacert);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        if (fd >= 0) ::close(fd);
        fd = -1;
        net.fd = -1;
    }
};

bool tlsOpen(TlsSession& s, const std::string& host,
             std::chrono::steady_clock::time_point deadline,
             std::string& stage, std::string& error,
             std::atomic<const char*>* note) {
    if (note) note->store("Resolving discord.com...");
    note_upload_step("dns");
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const int gai = getaddrinfo(host.c_str(), "443", &hints, &list);
    if (gai != 0 || !list) {
        stage = "dns";
        error = "getaddrinfo failed (" + std::to_string(gai) + ")";
        return false;
    }
    if (note) note->store("Connecting to Discord...");
    note_upload_step("tcp-connect");
    int lastErr = 0;
    for (addrinfo* ai = list; ai && s.fd < 0; ai = ai->ai_next) {
        const int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0) { lastErr = errno; continue; }
        timeval tv{};
        tv.tv_sec = 30;
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int sndbuf = 4 * 1024 * 1024;
        setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        if (connect(candidate, ai->ai_addr, ai->ai_addrlen) == 0) s.fd = candidate;
        else { lastErr = errno; ::close(candidate); }
    }
    freeaddrinfo(list);
    if (s.fd < 0) {
        stage = "connect";
        error = std::string("connect failed: ") + std::strerror(lastErr);
        return false;
    }
    mbedtls_ssl_init(&s.ssl);
    mbedtls_ctr_drbg_init(&s.drbg);
    mbedtls_entropy_init(&s.entropy);
    s.net.fd = s.fd;
    auto failTls = [&stage, &error](const char* what, int ret) {
        char buf[128];
        mbedtls_strerror(ret, buf, sizeof(buf));
        stage = "tls";
        error = std::string(what) + ": " + buf + " (" + std::to_string(ret) + ")";
        return false;
    };
    if (note) note->store("Seeding TLS...");
    note_upload_step("tls-seed");
    int ret = mbedtls_ctr_drbg_seed(&s.drbg, mbedtls_entropy_func, &s.entropy,
        reinterpret_cast<const unsigned char*>("PS5Share"), 8);
    if (ret != 0) return failTls("entropy seed failed", ret);
    ret = mbedtls_ssl_config_defaults(&s.conf, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return failTls("TLS config failed", ret);
    mbedtls_ssl_conf_authmode(&s.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (note) note->store("Loading CA bundle...");
    note_upload_step("ca-load");
    ret = mbedtls_x509_crt_parse_file(&s.cacert, caBundlePath());
    if (ret != 0) return failTls("CA bundle unreadable", ret);
    mbedtls_ssl_conf_ca_chain(&s.conf, &s.cacert, nullptr);
    mbedtls_ssl_conf_rng(&s.conf, mbedtls_ctr_drbg_random, &s.drbg);
    mbedtls_ssl_conf_min_tls_version(&s.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&s.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    ret = mbedtls_ssl_setup(&s.ssl, &s.conf);
    if (ret != 0) return failTls("TLS setup failed", ret);
    s.sslReady = true;
    ret = mbedtls_ssl_set_hostname(&s.ssl, host.c_str());
    if (ret != 0) return failTls("TLS hostname failed", ret);
    mbedtls_ssl_set_bio(&s.ssl, &s.net, mbedtls_net_send, mbedtls_net_recv, nullptr);
    if (note) note->store("TLS handshake...");
    note_upload_step("tls-handshake");
    while ((ret = mbedtls_ssl_handshake(&s.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return failTls("TLS handshake failed", ret);
        if (std::chrono::steady_clock::now() > deadline) {
            stage = "tls";
            error = "TLS handshake timed out";
            return false;
        }
    }
    const auto verify = mbedtls_ssl_get_verify_result(&s.ssl);
    if (verify != 0) {
        stage = "tls";
        error = "certificate verification failed (flags " + std::to_string(verify) + ")";
        return false;
    }
    note_upload_step("tls-connected");
    return true;
}

struct DirectTx {
    std::atomic<double>* fraction;
    std::atomic<bool>* cancel;
    std::chrono::steady_clock::time_point deadline;
    unsigned long long total{0};
    unsigned long long sent{0};
    std::atomic<unsigned long long>* sentOut{nullptr};
};

bool directWrite(TlsSession& s, DirectTx& tx, const unsigned char* data, size_t len,
                 std::string& error) {
    size_t off = 0;
    while (off < len) {
        if (tx.cancel->load()) { error = "cancelled"; return false; }
        if (std::chrono::steady_clock::now() > tx.deadline) {
            error = "deadline exceeded";
            return false;
        }
        const int ret = mbedtls_ssl_write(&s.ssl, data + off, len - off);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) {
            char buf[128];
            mbedtls_strerror(ret, buf, sizeof(buf));
            error = std::string("TLS write failed: ") + buf + " (" + std::to_string(ret) + ")";
            return false;
        }
        off += static_cast<size_t>(ret);
        tx.sent += static_cast<unsigned long long>(ret);
        if (tx.total > 0) tx.fraction->store(
            static_cast<double>(tx.sent) / static_cast<double>(tx.total));
        if (tx.sentOut) tx.sentOut->store(tx.sent);
    }
    return true;
}

bool directReadResponse(TlsSession& s, DirectTx& tx, ResponseCapture& out,
                        long& status, std::string& error) {
    unsigned char buf[1024];
    for (;;) {
        if (tx.cancel->load()) { error = "cancelled"; return false; }
        if (std::chrono::steady_clock::now() > tx.deadline) {
            error = "deadline exceeded";
            return false;
        }
        const int ret = mbedtls_ssl_read(&s.ssl, buf, sizeof(buf));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) {
            char ebuf[128];
            mbedtls_strerror(ret, ebuf, sizeof(ebuf));
            error = std::string("TLS read failed: ") + ebuf + " (" + std::to_string(ret) + ")";
            return false;
        }
        captureResponse(reinterpret_cast<char*>(buf), 1,
            static_cast<size_t>(ret), &out);
    }
    status = 0;
    if (out.len > 5) {
        const char* sp = static_cast<const char*>(std::memchr(out.data, ' ', out.len));
        if (sp) status = std::strtol(sp + 1, nullptr, 10);
    }
    return true;
}

bool splitWebhookUrl(const std::string& url, std::string& host, std::string& path) {
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    const size_t hostBegin = schemeEnd + 3;
    const size_t slash = url.find('/', hostBegin);
    if (slash == std::string::npos) {
        host = url.substr(hostBegin);
        path = "/";
    } else {
        host = url.substr(hostBegin, slash - hostBegin);
        path = url.substr(slash);
    }
    return !host.empty() && !path.empty();
}

std::string safeFilename(const std::string& name) {
    std::string out = name;
    for (char& ch : out) {
        if (ch == '"' || ch == '\\' || ch == '\r' || ch == '\n') ch = '_';
    }
    return out.empty() ? std::string("file") : out;
}

int net_init_stage(int stage) {
    switch (stage) {
    case 0: {
        const int netResult = sceNetInit();
        sceNetInitResult.store(netResult);
        return netResult;
    }
    case 1: {
        if (sceNetInitResult.load() != 0) {
            sceNetPoolResult.store(-9999);
            return sceNetInitResult.load();
        }
        /* Room for multi-MB socket send buffers plus resolver and TLS state. */
        const int poolId = sceNetPoolCreate("PS5Share", 16 * 1024 * 1024, 0);
        sceNetPoolResult.store(poolId);
        if (poolId >= 0) sceNetPoolId.store(poolId);
        return poolId >= 0 ? 0 : poolId;
    }
    case 2: {
        // Entropy self-check, identical to the proven legacy client: the
        // vendored mbedcrypto archive is byte-identical to the legacy
        // build's and contains no /dev/urandom platform poll, and the
        // legacy app seeds its DRBG through this same call on console.
        // An earlier revision first called sceRandomGetRandomNumber and
        // wrote a seed file here; the stage died before the self-check,
        // so those unproven calls were removed.
        mbedtls_entropy_context entropy;
        mbedtls_entropy_init(&entropy);
        unsigned char seed[32];
        const int ret = mbedtls_entropy_func(&entropy, seed, sizeof(seed));
        mbedtls_entropy_free(&entropy);
        return ret;
    }
    default: {
        // No curl global init. Transfers use the direct blocking mbedTLS
        // client (tlsOpen), identical to the proven legacy app; curl only
        // ever forwarded failure traces (postTextPayload), and its global
        // init faults in this binary (the console died at this stage),
        // while the identical call succeeds in the legacy binary. Skip
        // it: postTextPayload checks curlInitialized and stays inert.
        curlInitialized.store(false);
        return 1;
    }
    }
}

void shutdownHttpsClient() {
    if (curlInitialized.exchange(false)) curl_global_cleanup();
    const int poolId = sceNetPoolId.exchange(-1);
    if (poolId >= 0) sceNetPoolDestroy(poolId);
}

void note_upload_step(const char* step) {
    if (!step || !*step) return;
    ::mkdir("/download0/PS5Share", 0755);
    const int fd = ::open("/download0/PS5Share/ps5share-breadcrumb.txt",
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const char* data = step;
    size_t left = std::strlen(step);
    while (left > 0) {
        const ssize_t wrote = ::write(fd, data, left);
        if (wrote <= 0) break;
        data += wrote;
        left -= static_cast<size_t>(wrote);
    }
    ::close(fd);
}

WebhookUploader::~WebhookUploader() {
    requestCancel();
    join_worker();
}

// Explicit generous stack: the worker holds a TlsSession plus a 32 KiB
// file chunk on its stack, far beyond the audio thread the nullptr-attr
// precedent comes from.
bool create_upload_worker(pthread_t* out, void* (*entry)(void*), void* arg) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, 2u * 1024u * 1024u);
    const int rc = pthread_create(out, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void* WebhookUploader::thread_main(void* arg) {
    note_upload_step("worker-started");
    auto* self = static_cast<WebhookUploader*>(arg);
    if (self->job_test_) self->runTestMessage(self->job_url_);
    else self->runFileUpload(self->job_url_, self->job_path_);
    return nullptr;
}

void WebhookUploader::join_worker() {
    if (worker_started_) {
        pthread_join(worker_, nullptr);
        worker_started_ = false;
    }
}

bool WebhookUploader::startFileUpload(const std::string& webhookUrl, const std::string& path) {
    if (!is_allowed_webhook_url(webhookUrl) || path.empty()) return false;
    if (worker_started_) {
        if (snapshot().active) return false;
        join_worker();
    }
    cancel_.store(false);
    fraction_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = UploadSnapshot{true, false, false, 0.0, "Preparing upload…"};
    }
    job_url_ = webhookUrl;
    job_path_ = path;
    job_test_ = false;
    if (!create_upload_worker(&worker_, &WebhookUploader::thread_main, this)) {
        setResult("Could not start the upload thread.");
        return false;
    }
    worker_started_ = true;
    note_upload_step("worker-spawned");
    return true;
}

bool WebhookUploader::startTestMessage(const std::string& webhookUrl) {
    if (!is_allowed_webhook_url(webhookUrl)) return false;
    if (worker_started_) {
        if (snapshot().active) return false;
        join_worker();
    }
    cancel_.store(false);
    fraction_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = UploadSnapshot{true, false, false, 0.0, "Preparing webhook test…"};
    }
    job_url_ = webhookUrl;
    job_path_.clear();
    job_test_ = true;
    if (!create_upload_worker(&worker_, &WebhookUploader::thread_main, this)) {
        setResult("Could not start the upload thread.");
        return false;
    }
    worker_started_ = true;
    note_upload_step("worker-spawned");
    return true;
}

void WebhookUploader::requestCancel() {
    cancel_.store(true);
}

void WebhookUploader::shutdown() {
    requestCancel();
    join_worker();
}

UploadSnapshot WebhookUploader::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    UploadSnapshot copy = state_;
    copy.fraction = fraction_.load();
    copy.sentBytes = sentBytes_.load();
    copy.totalBytes = totalBytes_.load();
    copy.note = note_.load();
    return copy;
}

void WebhookUploader::setResult(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    state_.complete = true;
    state_.succeeded = false;
    state_.message = message;
}

void WebhookUploader::setSuccess(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    state_.complete = true;
    state_.succeeded = true;
    state_.message = message;
}

void WebhookUploader::setActiveFile(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.fileName = name;
}

void WebhookUploader::runFileUpload(std::string url, std::string path) {
    setNote("Opening capture...");
    note_upload_step("opening-capture");
    // Plain read-only open of the scan-produced path. The component walk
    // ported from the legacy app (openat + O_CLOEXEC + O_NOFOLLOW) faults
    // under this title's clean-room libc: the console breadcrumb stopped
    // at "opening-capture". save::read_file's plain ::open and stb_image's
    // fopen read these same capture paths fine in this binary, and the
    // fstat regular-file check below keeps the walk's safety contract.
    note_upload_step("open-file");
    const int fd = ::open(path.c_str(), O_RDONLY);
    struct stat info {};
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        if (fd >= 0) close(fd);
        setResult("Could not open the selected regular media file read-only.");
        return;
    }
    note_upload_step("file-open");
    const size_t slash = path.find_last_of("/\\");
    const std::string filename = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string payload = "{\"content\":\"Uploaded from PS5 Share\"}";
    resetIoStats();
    std::string host, urlPath;
    const bool urlOk = splitWebhookUrl(url, host, urlPath);
    static constexpr char kBoundary[] = "----PS5ShareFormBoundary1a2b3c";
    const std::string head1 = std::string("--") + kBoundary +
        "\r\nContent-Disposition: form-data; name=\"payload_json\""
        "\r\nContent-Type: application/json\r\n\r\n" + payload + "\r\n";
    const std::string head2 = std::string("--") + kBoundary +
        "\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"" +
        safeFilename(filename) + "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    const std::string tail = std::string("\r\n--") + kBoundary + "--\r\n";
    const unsigned long long bodyLen =
        static_cast<unsigned long long>(head1.size() + head2.size() + tail.size()) +
        static_cast<unsigned long long>(info.st_size);
    const std::string headers = "POST " + urlPath + " HTTP/1.1\r\nHost: " + host +
        "\r\nUser-Agent: " + kUserAgent + "\r\nAccept: */*\r\n"
        "Content-Type: multipart/form-data; boundary=" + kBoundary +
        "\r\nContent-Length: " + std::to_string(bodyLen) +
        "\r\nConnection: close\r\n\r\n";

    const auto startedAt = std::chrono::steady_clock::now();
    TlsSession session;
    sentBytes_.store(0);
    totalBytes_.store(0);
    DirectTx tx{&fraction_, &cancel_, startedAt + std::chrono::seconds(600),
        static_cast<unsigned long long>(headers.size()) + bodyLen, 0, &sentBytes_};
    totalBytes_.store(tx.total);
    setActiveFile(filename);
    std::string stage, error;
    long status = 0;
    ResponseCapture reply{};
    bool transported = false;
    setNote("Contacting Discord...");
    const bool opened = urlOk && tlsOpen(session, host, tx.deadline, stage, error, &note_);
    if (!urlOk) {
        stage = "dns";
        error = "stored webhook URL could not be parsed";
    }
    bool sentAll = opened;
    if (sentAll) {
        setNote("Sending capture...");
        note_upload_step("sending");
    }
    if (sentAll) {
        sentAll = directWrite(session, tx,
            reinterpret_cast<const unsigned char*>(headers.data()), headers.size(), error) &&
            directWrite(session, tx,
            reinterpret_cast<const unsigned char*>(head1.data()), head1.size(), error) &&
            directWrite(session, tx,
            reinterpret_cast<const unsigned char*>(head2.data()), head2.size(), error);
    }
    if (sentAll) {
        unsigned char chunk[32768];
        long long remaining = static_cast<long long>(info.st_size);
        while (remaining > 0 && sentAll) {
            const size_t want = remaining < static_cast<long long>(sizeof(chunk))
                ? static_cast<size_t>(remaining) : sizeof(chunk);
            const ssize_t got = read(fd, chunk, want);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                error = "could not read the media file";
                sentAll = false;
                break;
            }
            sentAll = directWrite(session, tx, chunk, static_cast<size_t>(got), error);
            remaining -= got;
        }
        if (sentAll) {
            sentAll = directWrite(session, tx,
                reinterpret_cast<const unsigned char*>(tail.data()), tail.size(), error);
        }
    }
    if (sentAll) {
        setNote("Waiting for Discord...");
        note_upload_step("waiting-response");
        transported = directReadResponse(session, tx, reply, status, error);
        if (!transported && stage.empty()) stage = "recv";
    } else if (opened && stage.empty()) {
        stage = "send";
    }

    close(fd);
    session.close();
    const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    if (cancel_.load()) {
        setResult("Upload cancelled.");
    } else if (!transported) {
        const std::string detail = "Upload \"" + filename + "\" (" +
            std::to_string(static_cast<long long>(info.st_size)) + " B) failed at " + stage +
            ": " + error + "; sent " + std::to_string(tx.sent) + " B; total " +
            std::to_string(totalMs) + " ms; build " + kBuildTag +
            (reply.len != 0 ? "; reply: " + responsePreview(reply, 200) : std::string());
        const std::string logPath = saveDiagnosticsPath("ps5share-last-upload.txt", detail);
        std::string trace = detail;
        if (trace.size() > 1200) trace.resize(1200);
        const bool forwarded = postTextPayload(url,
            "{\"content\":\"PS5 Share upload failure:\\n" + escapeJson(trace) + "\"}");
        setResult(detail + (logPath.empty() ? " TRACE SAVE FAILED" : ". Trace: " + logPath) +
            (forwarded ? ". Trace sent to Discord" : ". Trace forward FAILED"));
    } else if (status < 200 || status >= 300) {
        const std::string preview = responsePreview(reply, 400);
        const std::string detail = "Discord rejected the upload (HTTP " +
            std::to_string(status) + ")" +
            (preview.empty() ? "." : ": " + preview);
        const std::string full = detail + "\nFull response (" +
            std::to_string(reply.len) + " B):\n" + responsePreview(reply, reply.len);
        const std::string rejectLog = saveDiagnosticsPath("ps5share-last-upload.txt", full);
        setResult(detail + (rejectLog.empty() ? " TRACE SAVE FAILED" : ". Trace: " + rejectLog));
    } else {
        fraction_.store(1.0);
        note_upload_step("done");
        setSuccess("Upload completed (HTTP " + std::to_string(status) + ").");
    }
}

void WebhookUploader::runTestMessage(std::string url) {
    note_upload_step("test-message");
    const std::string payload = "{\"content\":\"PS5 Share webhook test (no media uploaded).\"}";
    resetIoStats();
    std::string host, urlPath;
    const bool urlOk = splitWebhookUrl(url, host, urlPath);
    const std::string headers = "POST " + urlPath + " HTTP/1.1\r\nHost: " + host +
        "\r\nUser-Agent: " + kUserAgent + "\r\nAccept: */*\r\n"
        "Content-Type: application/json\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
    TlsSession session;
    sentBytes_.store(0);
    totalBytes_.store(0);
    DirectTx tx{&fraction_, &cancel_,
        std::chrono::steady_clock::now() + std::chrono::seconds(60),
        static_cast<unsigned long long>(headers.size() + payload.size()), 0, &sentBytes_};
    totalBytes_.store(tx.total);
    setActiveFile("Webhook test");
    std::string stage, error;
    long status = 0;
    ResponseCapture reply{};
    bool transported = false;
    setNote("Contacting Discord...");
    const bool opened = urlOk && tlsOpen(session, host, tx.deadline, stage, error, &note_);
    if (!urlOk) {
        stage = "dns";
        error = "stored webhook URL could not be parsed";
    }
    bool sentAll = opened &&
        directWrite(session, tx,
            reinterpret_cast<const unsigned char*>(headers.data()), headers.size(), error) &&
        directWrite(session, tx,
            reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), error);
    if (sentAll) {
        transported = directReadResponse(session, tx, reply, status, error);
        if (!transported && stage.empty()) stage = "recv";
    } else if (opened && stage.empty()) {
        stage = "send";
    }
    session.close();
    if (cancel_.load()) {
        setResult("Webhook test cancelled.");
    } else if (!transported) {
        const std::string detail = "Webhook test failed at " + stage + ": " + error +
            "; build " + kBuildTag +
            (reply.len != 0 ? "; reply: " + responsePreview(reply, 200) : std::string());
        const std::string logPath = saveDiagnosticsPath("ps5share-last-test.txt", detail);
        setResult(detail + (logPath.empty() ? " TRACE SAVE FAILED" : ". Full trace: " + logPath));
    } else if (status < 200 || status >= 300) {
        const std::string preview = responsePreview(reply, 400);
        const std::string detail = "Discord rejected the webhook test (HTTP " +
            std::to_string(status) + ")" +
            (preview.empty() ? "." : ": " + preview);
        const std::string full = detail + "\nFull response (" +
            std::to_string(reply.len) + " B):\n" + responsePreview(reply, reply.len);
        const std::string rejectLog = saveDiagnosticsPath("ps5share-last-test.txt", full);
        setResult(detail + (rejectLog.empty() ? " TRACE SAVE FAILED" : ". Full trace: " + rejectLog));
    } else {
        fraction_.store(1.0);
        note_upload_step("done");
        setSuccess("Webhook test sent (HTTP " + std::to_string(status) + ").");
    }
}

}  // namespace share
}  // namespace hui

#else  // Host builds: same interface, inert backend for snapshots and tests.

// Shim instrumentation counters (title_compat.c references them in every
// build; the PS5 branch defines its own set above).
extern "C" unsigned long ps5share_poll_calls = 0;
extern "C" unsigned long ps5share_poll_ms = 0;
extern "C" unsigned long ps5share_select_calls = 0;
extern "C" unsigned long ps5share_select_ms = 0;
extern "C" unsigned long ps5share_send_calls = 0;
extern "C" unsigned long ps5share_send_eagain = 0;
extern "C" unsigned long long ps5share_send_bytes = 0;
extern "C" unsigned long ps5share_send_ms = 0;
extern "C" unsigned long ps5share_recv_calls = 0;
extern "C" unsigned long ps5share_recv_ms = 0;
extern "C" unsigned long ps5share_fcntl_nb = 0;
extern "C" unsigned long ps5share_fcntl_nbio_ok = 0;
extern "C" unsigned long ps5share_fcntl_getfl_fail = 0;
extern "C" unsigned long ps5share_fcntl_getfl_emul = 0;

namespace hui {
namespace share {

int net_init_stage(int) { return 0; }
void shutdownHttpsClient() {}
void note_upload_step(const char*) {}

WebhookUploader::~WebhookUploader() { shutdown(); }

void* WebhookUploader::thread_main(void*) { return nullptr; }
void WebhookUploader::join_worker()
{
    if (worker_started_)
    {
        pthread_join(worker_, nullptr);
        worker_started_ = false;
    }
}

bool WebhookUploader::startFileUpload(const std::string&, const std::string&) { return false; }
bool WebhookUploader::startTestMessage(const std::string&) { return false; }
void WebhookUploader::requestCancel() { cancel_.store(true); }
void WebhookUploader::shutdown()
{
    cancel_.store(true);
    join_worker();
    cancel_.store(false);
}

UploadSnapshot WebhookUploader::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void WebhookUploader::runFileUpload(std::string, std::string) {}
void WebhookUploader::runTestMessage(std::string) {}
void WebhookUploader::setResult(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    state_.complete = true;
    state_.succeeded = false;
    state_.message = message;
}
void WebhookUploader::setSuccess(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    state_.complete = true;
    state_.succeeded = true;
    state_.message = message;
}
void WebhookUploader::setActiveFile(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = true;
    state_.fileName = name;
}

}  // namespace share
}  // namespace hui

#endif  // HUI_PS5_BUILD

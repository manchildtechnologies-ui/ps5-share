// PS5 Share Hub Discord uploader.
// The PS5 backend is the working upload stack ported verbatim from the legacy
// SDL/ImGui PS5 Share app: libcurl over mbedTLS with the title's SceNet pool,
// DNS-over-UDP resolver and poll/select/send/recv/fcntl shims. Uploads run on
// a worker thread; the hub polls snapshot() each frame for progress UI.
// Host builds (snapshots, unit tests) compile a stub with the same interface.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>

namespace hui {
namespace share {

// Staged network init, one call per stage so the hub can paint the stage
// name before running it: 0 = SceNet library, 1 = SceNet memory pool,
// 2 = TLS entropy self-check, 3 = TLS stack. Stage 3 returns 1 on
// success; earlier stages return their raw result code (0 = ok).
int net_init_stage(int stage);
void shutdownHttpsClient();

// Crash breadcrumb: overwrites /download0/PS5Share/ps5share-breadcrumb.txt
// with the name of the upload step that is ABOUT to run. The hub reads it
// on the next launch, so a crash that closes the app (system popup, no
// readable final frame) still reports exactly where it stopped.
void note_upload_step(const char* step);

struct UploadSnapshot {
    bool active{false};
    bool complete{false};
    bool succeeded{false};
    double fraction{0.0};
    std::string message;
    std::string fileName;
    unsigned long long sentBytes{0};
    unsigned long long totalBytes{0};
    // Current worker milestone (string literal, never owned). The hub
    // paints it while an upload runs, so a console crash leaves the
    // faulting step on screen.
    const char* note{nullptr};
};

class WebhookUploader {
public:
    WebhookUploader() = default;
    ~WebhookUploader();
    WebhookUploader(const WebhookUploader&) = delete;
    WebhookUploader& operator=(const WebhookUploader&) = delete;

    bool startFileUpload(const std::string& webhookUrl, const std::string& path);
    bool startTestMessage(const std::string& webhookUrl);
    void requestCancel();
    void shutdown();
    UploadSnapshot snapshot() const;

private:
    static void* thread_main(void* arg);
    void join_worker();
    void runFileUpload(std::string url, std::string path);
    void runTestMessage(std::string url);
    void setResult(const std::string& message);
    void setSuccess(const std::string& message);
    void setActiveFile(const std::string& name);
    void setNote(const char* text) { note_.store(text); }

    mutable std::mutex mutex_;
    UploadSnapshot state_;
    std::atomic<bool> cancel_{false};
    std::atomic<double> fraction_{0.0};
    std::atomic<unsigned long long> sentBytes_{0};
    std::atomic<unsigned long long> totalBytes_{0};
    std::atomic<const char*> note_{nullptr};
    // Worker via pthread_create, the primitive the PS5 platform code uses;
    // std::thread is unproven under this runtime and crashed on spawn.
    pthread_t worker_{};
    bool worker_started_ = false;
    std::string job_url_;
    std::string job_path_;
    [[maybe_unused]] bool job_test_ = false; // read by the PS5 thread entry
};

}  // namespace share
}  // namespace hui

#include "sessions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "audio_capture.h"
#include "audio_resampler.h"
#include "color_convert.h"
#include "exclusions.h"
#include "h264_encoder.h"
#include "hook_capture.h"
#include "mkv_writer.h"
#include "opus_audio_encoder.h"
#include "process_list.h"
#include "ts_muxer.h"

namespace {

constexpr int kCaptureWidth = 1280;
constexpr int kCaptureHeight = 720;
constexpr int kCaptureFps = 24;
constexpr int kVideoBitrateKbps = 4000;
constexpr int kAudioBitrate = 96000;
constexpr int kHookRetryTimeoutMs = 1500;
constexpr int kSourceRetryIntervalMs = 3000;
constexpr int kHookStallFrames = kCaptureFps * 5;
constexpr int kTargetMissingLimit = 3;

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    }
    return result;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

// Cheap content check: if almost every sampled pixel is black, the hook is probably seeing a
// surface that is not the rendered game.
bool IsMostlyBlack(const std::uint8_t* bgra, int width, int height)
{
    int nonBlack = 0;
    int total = 0;
    for (int y = 0; y < height; y += 16) {
        const std::uint8_t* row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; x += 16) {
            const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
            if (pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8) {
                ++nonBlack;
            }
            ++total;
        }
    }
    return total > 0 && (nonBlack * 100) < total;  // under 1% non-black
}

bool IsProcessAlive(unsigned long processId)
{
    if (processId == 0) {
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(processId));
    if (process == nullptr) {
        return true;  // Could not query it; do not switch capture on a guess.
    }
    const DWORD result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result == WAIT_TIMEOUT;
}

bool TargetStillPresent(const std::wstring& executable, unsigned long processId)
{
    if (executable.empty()) {
        return true;  // Top-most window capture has no fixed target.
    }
    if (!IsProcessAlive(processId)) {
        return false;
    }

    // A live process with no visible window behaves like a closed target for capture purposes.
    const auto processes = ListWindowProcesses();
    for (const auto& entry : processes) {
        if (entry.processId == processId) {
            return true;
        }
    }
    return false;
}

// Batches muxer output into seven-packet datagrams, the size that fits a 1500-byte MTU.
class UdpTsSink final : public TsSink {
public:
    explicit UdpTsSink(UdpSender& sender) : sender_(sender) {}

    void OnTsPacket(const std::uint8_t* packet) override
    {
        std::memcpy(buffer_.data() + buffered_ * TsMuxer::kPacketSize, packet, TsMuxer::kPacketSize);
        ++buffered_;
        if (buffered_ == kPacketsPerBuffer) {
            Flush();
        }
    }

    void Flush()
    {
        if (buffered_ == 0) {
            return;
        }
        if (!sender_.SendTsPackets(buffer_.data(), buffered_) && sendFailures_++ == 0) {
            OutputDebugStringW(L"wxr-vid-cap cast: a UDP send failed; continuing to send.\n");
        }
        buffered_ = 0;
    }

private:
    static constexpr std::size_t kPacketsPerBuffer = UdpSender::kPacketsPerDatagram;

    UdpSender& sender_;
    std::array<unsigned char, UdpSender::kPacketSize * kPacketsPerBuffer> buffer_{};
    std::size_t buffered_ = 0;
    int sendFailures_ = 0;
};

}  // namespace

struct Recorder::Impl {
    std::wstring path;
    std::wstring targetExecutable;
    std::wstring backend;
    int hookTimeoutMs = 6000;
    std::mutex sourceMutex;
    std::wstring currentSource;
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> active{false};
    bool wroteFile = false;
    std::wstring error;

    void Run(std::promise<std::pair<bool, std::wstring>> ready);
};

Recorder::Recorder() = default;

Recorder::~Recorder()
{
    Stop();
}

bool Recorder::Start(const std::wstring& path, const std::wstring& targetExecutable, std::wstring& error,
                     int hookTimeoutMs)
{
    if (impl_ && impl_->active.load()) {
        error = L"A recording is already running.";
        return false;
    }

    impl_ = std::make_unique<Impl>();
    impl_->path = path;
    impl_->targetExecutable = targetExecutable;
    impl_->hookTimeoutMs = hookTimeoutMs;
    impl_->currentSource = targetExecutable;

    using Result = std::pair<bool, std::wstring>;
    auto promise = std::promise<Result>();
    auto future = promise.get_future();
    Impl* impl = impl_.get();
    impl->thread = std::thread([impl, promise = std::move(promise)]() mutable { impl->Run(std::move(promise)); });

    const Result result = future.get();
    if (!result.first) {
        if (impl->thread.joinable()) {
            impl->thread.join();
        }
        impl_.reset();
        error = result.second;
        return false;
    }
    return true;
}

void Recorder::Stop()
{
    if (!impl_) {
        return;
    }
    impl_->stopRequested = true;
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

bool Recorder::Active() const
{
    return impl_ && impl_->active.load();
}

bool Recorder::LastStopWroteFile() const
{
    return impl_ && impl_->wroteFile;
}

const std::wstring& Recorder::Path() const
{
    static const std::wstring empty;
    return impl_ ? impl_->path : empty;
}

const std::wstring& Recorder::Backend() const
{
    static const std::wstring empty;
    return impl_ ? impl_->backend : empty;
}

const std::wstring& Recorder::LastError() const
{
    static const std::wstring empty;
    return impl_ ? impl_->error : empty;
}

std::wstring Recorder::Source() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->sourceMutex);
    return impl_->currentSource;
}

void Recorder::Impl::Run(std::promise<std::pair<bool, std::wstring>> ready)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto fail = [&](const std::wstring& message) {
        ready.set_value({false, message});
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    };

    std::ofstream log(std::filesystem::path(path + L".log"), std::ios::binary);
    auto logLine = [&log](const std::string& line) {
        if (log) {
            log << line << '\n';
            log.flush();
        }
    };
    logLine("wxr-vid-cap recording");
    logLine("windows:");
    const auto processes = ListWindowProcesses();
    if (processes.empty()) {
        logLine("  (none)");
    } else {
        for (const auto& entry : processes) {
            logLine("  " + Narrow(entry.windowTitle) + " - " + Narrow(FileNameOf(entry.executablePath)) + "  (" +
                    Narrow(entry.executablePath) + ")");
        }
    }
    logLine("video: 1280x720 @ 24 fps, 4000 kbps");

    HookCapture hook;
    std::wstring initError;

    if (targetExecutable.empty()) {
        targetExecutable = TopMostWindowExecutable();
        if (targetExecutable.empty()) {
            fail(L"No capturable window is running.");
            return;
        }
    }

    if (!hook.Start(targetExecutable, kCaptureWidth, kCaptureHeight, initError, hookTimeoutMs)) {
        logLine("error: " + Narrow(initError));
        fail(L"Could not start the graphics hook: " + initError);
        return;
    }

    bool hookActive = true;
    backend = L"graphics hook";
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        currentSource = targetExecutable;
    }
    logLine("backend: graphics hook");
    logLine("hook: " + Narrow(hook.LastInjectionMethod()) + ", source " +
            std::to_string(hook.SourceWidth()) + "x" + std::to_string(hook.SourceHeight()));
    logLine("target: " + Narrow(targetExecutable));

    H264Encoder encoder;
    if (!encoder.Initialize(kCaptureWidth, kCaptureHeight, kCaptureFps, kVideoBitrateKbps, initError)) {
        logLine("error: " + Narrow(initError));
        fail(L"Could not start the video encoder: " + initError);
        return;
    }

    AudioCapture audio;
    OpusAudioEncoder opus;
    AudioResampler resampler;
    bool audioEnabled = false;
    if (audio.Initialize(initError) && opus.Initialize(48000, audio.Channels(), kAudioBitrate, initError) &&
        audio.Start(initError)) {
        resampler.Initialize(audio.SampleRate(), audio.Channels(), 48000);
        audioEnabled = true;
        logLine("audio: capture " + std::to_string(audio.SampleRate()) + " Hz -> 48000 Hz, " +
                std::to_string(audio.Channels()) + " ch, 96 kbps");
    } else {
        logLine("audio: unavailable (" + Narrow(initError) + "); video-only");
        audio.Shutdown();
        opus.Shutdown();
        initError.clear();
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        logLine("error: could not create the recording file");
        fail(L"Could not create the recording file.");
        return;
    }

    std::vector<std::uint8_t> audioCodecPrivate;
    if (audioEnabled) {
        audioCodecPrivate = opus.CodecPrivate();
    }

    MkvWriter writer(file, kCaptureWidth, kCaptureHeight, kCaptureFps, audioEnabled ? opus.SampleRate() : 0,
                     audioEnabled ? audio.Channels() : 0, audioCodecPrivate);

    active = true;
    ready.set_value({true, L""});

    const int64_t frameTicks = 90000 / kCaptureFps;
    const auto start = std::chrono::steady_clock::now();
    int frameIndex = 0;
    int framesWritten = 0;
    double captureMs = 0.0;
    double convertMs = 0.0;
    double encodeMs = 0.0;
    double audioMs = 0.0;
    bool contentLogged = false;
    bool lastContentBlack = false;
    int hookEmptyFrames = 0;
    int targetMissingChecks = 0;
    bool rehooked = false;
    std::vector<std::uint8_t> frame;

    const int audioChannels = audioEnabled ? audio.Channels() : 0;
    const int audioRate = audioEnabled ? opus.SampleRate() : 0;
    const int opusFrameSamples = audioEnabled ? opus.FrameSamples() : 0;
    std::vector<std::int16_t> audioBuffer;
    std::size_t audioOffset = 0;
    std::int64_t audioSamplesEncoded = 0;

    auto drainAudio = [&](std::int64_t upToPts) -> bool {
        std::vector<std::int16_t> captured;
        if (!audio.Read(captured, error)) {
            return false;
        }
        std::vector<std::int16_t> resampled;
        resampler.Process(captured, resampled);
        audioBuffer.insert(audioBuffer.end(), resampled.begin(), resampled.end());

        for (;;) {
            const std::size_t availableSamples =
                (audioBuffer.size() - audioOffset) / static_cast<std::size_t>(audioChannels);
            if (availableSamples < static_cast<std::size_t>(opusFrameSamples)) {
                break;
            }
            const std::int64_t pts = audioSamplesEncoded * 90000 / audioRate;
            if (pts > upToPts) {
                break;
            }

            std::vector<std::uint8_t> packet;
            if (!opus.Encode(audioBuffer.data() + audioOffset, opusFrameSamples, packet, error)) {
                return false;
            }
            if (!writer.WriteAudioFrame(packet.data(), packet.size(), pts)) {
                error = L"Audio muxing failed.";
                return false;
            }

            audioOffset += static_cast<std::size_t>(opusFrameSamples) * static_cast<std::size_t>(audioChannels);
            audioSamplesEncoded += opusFrameSamples;
        }

        if (audioOffset > 0 && audioOffset >= 48000u * static_cast<std::size_t>(audioChannels)) {
            audioBuffer.erase(audioBuffer.begin(), audioBuffer.begin() + static_cast<std::ptrdiff_t>(audioOffset));
            audioOffset = 0;
        }
        return true;
    };

    auto switchSource = [&](const std::wstring& newSource) -> bool {
        hook.Stop();
        hookActive = false;
        hookEmptyFrames = 0;
        targetMissingChecks = 0;

        const bool sameTarget =
            !newSource.empty() && _wcsicmp(newSource.c_str(), targetExecutable.c_str()) == 0;
        if (newSource.empty() || sameTarget) {
            error = L"The capture target closed and no other hookable window was found.";
            logLine("source: no replacement target; stopping");
            return false;
        }

        std::wstring switchError;
        if (!hook.Start(newSource, kCaptureWidth, kCaptureHeight, switchError, 3000)) {
            error = L"The capture target closed and the next window could not be hooked: " + switchError;
            logLine("source: re-hook failed: " + Narrow(switchError));
            return false;
        }

        targetExecutable = newSource;
        hookActive = true;
        rehooked = true;
        backend = L"graphics hook";
        logLine("source: re-hooked to " + Narrow(newSource));

        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            currentSource = targetExecutable;
        }

        std::wstring encoderError;
        if (!encoder.Initialize(kCaptureWidth, kCaptureHeight, kCaptureFps, kVideoBitrateKbps, encoderError)) {
            error = L"Could not restart the video encoder after switching capture sources: " + encoderError;
            return false;
        }

        return true;
    };

    while (!stopRequested.load()) {
        const std::int64_t pts = static_cast<std::int64_t>(frameIndex) * frameTicks;

        if (audioEnabled && frameIndex > 0) {
            const auto audioStart = std::chrono::steady_clock::now();
            if (!drainAudio(pts)) {
                break;
            }
            audioMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
        }

        if (hookActive && !targetExecutable.empty() && frameIndex > 0 &&
            frameIndex % kCaptureFps == 0 && hookEmptyFrames > 0) {
            if (TargetStillPresent(targetExecutable, hook.ProcessId())) {
                targetMissingChecks = 0;
            } else if (++targetMissingChecks >= 3) {
                if (!switchSource(TopMostWindowExecutable())) {
                    break;
                }
            }
        } else if (hookEmptyFrames == 0) {
            targetMissingChecks = 0;
        }

        if (hookActive && rehooked && hookEmptyFrames >= 48) {
            error = L"The re-hooked capture target stopped producing frames.";
            logLine("source: re-hooked target stopped producing frames; stopping");
            break;
        }

        const auto captureStart = std::chrono::steady_clock::now();
        if (hook.Capture(frame, 20, error)) {
            hookEmptyFrames = 0;
        } else {
            ++hookEmptyFrames;
            if (hookEmptyFrames == 48) {
                logLine("hook: no frames for 2 s; repeating the last frame");
            }
        }
        if (frame.size() != static_cast<std::size_t>(kCaptureWidth) * static_cast<std::size_t>(kCaptureHeight) * 4) {
            error = L"No frames are available from the capture target.";
            break;
        }

        captureMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();

        const auto convertStart = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> i420;
        if (!BgraToI420(frame.data(), kCaptureWidth, kCaptureHeight, i420)) {
            error = L"Frame conversion failed.";
            break;
        }
        convertMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convertStart).count();

        const auto encodeStart = std::chrono::steady_clock::now();
        bool keyframe = false;
        std::vector<std::uint8_t> annexB;
        if (!encoder.Encode(i420, pts, keyframe, annexB, error)) {
            break;
        }
        encodeMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encodeStart).count();

        if (!annexB.empty()) {
            if (!writer.WriteVideoFrame(annexB.data(), annexB.size(), pts, keyframe)) {
                error = L"Video muxing failed.";
                break;
            }
            ++framesWritten;
        }

        const bool contentBlack = IsMostlyBlack(frame.data(), kCaptureWidth, kCaptureHeight);
        if (!contentLogged || contentBlack != lastContentBlack) {
            logLine(contentBlack ? "content: black (capture may be missing a GPU-rendered surface)"
                                 : "content: visible");
            lastContentBlack = contentBlack;
            contentLogged = true;
        }

        if (audioEnabled && frameIndex == 0) {
            const auto audioStart = std::chrono::steady_clock::now();
            if (!drainAudio(0)) {
                break;
            }
            audioMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
        }

        ++frameIndex;
        const auto deadline =
            start + std::chrono::microseconds(static_cast<long long>(frameIndex) * 1000000 / kCaptureFps);
        std::this_thread::sleep_until(deadline);
    }

    if (audioEnabled) {
        if (error.empty()) {
            const auto audioStart = std::chrono::steady_clock::now();
            drainAudio(std::numeric_limits<std::int64_t>::max());
            audioMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
        }
        audio.Stop();
    }

    if (!writer.Finalize() && error.empty()) {
        error = L"Could not finalize the recording file.";
    }
    file.close();

    wroteFile = framesWritten > 0;
    if (!wroteFile) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    const double frames = static_cast<double>(frameIndex > 0 ? frameIndex : 1);
    logLine("frames: " + std::to_string(framesWritten));
    logLine("avg capture: " + std::to_string(captureMs / frames) + " ms, convert: " +
            std::to_string(convertMs / frames) + " ms, encode: " + std::to_string(encodeMs / frames) +
            " ms, audio: " + std::to_string(audioMs / frames) + " ms");
    if (!error.empty()) {
        logLine("error: " + Narrow(error));
    }
    logLine(std::string("result: ") + (wroteFile ? "ok" : "no file"));

    active = false;
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

struct Caster::Impl {
    UdpSender sender;
    unsigned short port = 0;
    std::wstring targetExecutable;
    std::wstring backend;
    int hookTimeoutMs = 6000;
    std::wstring currentSource;
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> active{false};
    std::mutex sourceMutex;
    std::wstring pendingSource;
    std::atomic<bool> sourceChangeRequested{false};
    std::wstring error;

    void Run(std::promise<std::pair<bool, std::wstring>> ready);
};

Caster::Caster() = default;

Caster::~Caster()
{
    Stop();
}

bool Caster::Start(const std::wstring& host, unsigned short port, const std::wstring& targetExecutable,
                   std::wstring& error, int hookTimeoutMs)
{
    if (impl_ && impl_->active.load()) {
        error = L"A cast is already running.";
        return false;
    }
    if (impl_) {
        Stop();  // Join a worker that already ended on its own before replacing it.
    }

    auto impl = std::make_unique<Impl>();
    impl->port = port;
    impl->targetExecutable = targetExecutable;
    impl->hookTimeoutMs = hookTimeoutMs;
    impl->currentSource = targetExecutable;

    if (!impl->sender.Open(host, port, error)) {
        return false;
    }

    using Result = std::pair<bool, std::wstring>;
    auto promise = std::promise<Result>();
    auto future = promise.get_future();
    Impl* raw = impl.get();
    impl->thread = std::thread([raw, promise = std::move(promise)]() mutable { raw->Run(std::move(promise)); });

    const Result result = future.get();
    if (!result.first) {
        if (impl->thread.joinable()) {
            impl->thread.join();
        }
        error = result.second;
        return false;
    }

    impl_ = std::move(impl);
    return true;
}

void Caster::Stop()
{
    if (!impl_) {
        return;
    }
    impl_->stopRequested = true;
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->active = false;
    impl_->sender.Close();
}

bool Caster::Active() const
{
    return impl_ && impl_->active.load();
}

const std::wstring& Caster::Backend() const
{
    static const std::wstring empty;
    return impl_ ? impl_->backend : empty;
}

const std::wstring& Caster::LastError() const
{
    static const std::wstring empty;
    return impl_ ? impl_->error : empty;
}

bool Caster::ChangeSource(const std::wstring& targetExecutable, std::wstring& error)
{
    if (!impl_ || !impl_->active.load()) {
        error = L"No cast is running.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->sourceMutex);
        impl_->pendingSource = targetExecutable;
    }
    impl_->sourceChangeRequested = true;
    return true;
}

std::wstring Caster::Source() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->sourceMutex);
    return impl_->currentSource;
}

std::wstring Caster::ViewerUrl() const
{
    return L"udp://@:" + std::to_wstring(impl_ ? impl_->port : 0);
}

void Caster::Impl::Run(std::promise<std::pair<bool, std::wstring>> ready)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool announced = false;

    auto fail = [&](const std::wstring& message) {
        if (!announced) {
            ready.set_value({false, message});
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
            return;
        }
        error = message;
        active = false;
        OutputDebugStringW((L"wxr-vid-cap cast: " + message + L"\n").c_str());
    };
    const auto trace = [](const std::wstring& line) {
        OutputDebugStringW((L"wxr-vid-cap cast: " + line + L"\n").c_str());
    };

    std::unique_ptr<HookCapture> hook;
    std::wstring initError;
    std::wstring desiredTarget = targetExecutable;
    if (!desiredTarget.empty() && IsExcludedExecutablePath(desiredTarget)) {
        trace(L"source: saved target is blacklisted; using top-most retries: " + desiredTarget);
        desiredTarget.clear();
    }

    bool hookActive = false;
    bool resetEncoder = false;
    int hookEmptyFrames = 0;
    int targetMissingChecks = 0;
    ULONGLONG nextRetry = GetTickCount64() + 1000;
    std::vector<std::uint8_t> frame;
    const std::size_t frameBytes =
        static_cast<std::size_t>(kCaptureWidth) * static_cast<std::size_t>(kCaptureHeight) * 4;
    std::vector<std::uint8_t> blankFrame(frameBytes, 0);

    std::mutex retryMutex;
    std::unique_ptr<HookCapture> pendingHook;
    std::wstring pendingTarget;
    std::atomic<bool> retryInProgress{false};
    std::atomic<bool> retryFinished{false};
    std::atomic<bool> retrySucceeded{false};
    std::atomic<unsigned> retryGeneration{0};
    std::atomic<unsigned> retryResultGeneration{0};
    std::thread retryThread;

    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        currentSource.clear();
    }

    auto tryHookSource = [&](const std::wstring& candidate, int timeoutMs) -> bool {
        if (candidate.empty() || IsExcludedExecutablePath(candidate)) {
            return false;
        }
        if (hookActive && hook) {
            hook->Stop();
            hook.reset();
            hookActive = false;
        }
        if (!hook) {
            hook = std::make_unique<HookCapture>();
        }

        std::wstring switchError;
        if (!hook->Start(candidate, kCaptureWidth, kCaptureHeight, switchError, timeoutMs)) {
            trace(L"source: hook failed for " + candidate + L": " + switchError);
            return false;
        }

        targetExecutable = candidate;
        hookActive = true;
        resetEncoder = true;
        hookEmptyFrames = 0;
        targetMissingChecks = 0;
        frame.clear();
        backend = L"graphics hook";
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            currentSource = candidate;
        }
        trace(L"source: hooked " + candidate + L", " + hook->LastInjectionMethod() + L", source " +
              std::to_wstring(hook->SourceWidth()) + L"x" + std::to_wstring(hook->SourceHeight()) +
              L", pid " + std::to_wstring(hook->ProcessId()));
        return true;
    };

    auto enterWaiting = [&]() {
        if (hook) {
            hook->Stop();
            hook.reset();
        }
        hookActive = false;
        hookEmptyFrames = 0;
        targetMissingChecks = 0;
        desiredTarget.clear();
        resetEncoder = true;
        nextRetry = GetTickCount64();
        frame.clear();
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            currentSource.clear();
        }
        trace(L"source: waiting for a capturable window; sending blank frames");
    };

    auto startRetry = [&](const std::wstring& candidate) {
        if (retryInProgress.load()) {
            return;
        }
        if (retryThread.joinable()) {
            retryThread.join();
        }
        {
            std::lock_guard<std::mutex> lock(retryMutex);
            pendingHook.reset();
            pendingTarget.clear();
        }

        const unsigned generation = ++retryGeneration;
        retrySucceeded = false;
        retryFinished = false;
        retryInProgress = true;
        nextRetry = GetTickCount64() + kSourceRetryIntervalMs;

        retryThread = std::thread([&, candidate, generation]() {
            auto candidateHook = std::make_unique<HookCapture>();
            std::wstring retryError;
            const bool ok = candidateHook->Start(candidate, kCaptureWidth, kCaptureHeight, retryError,
                                                 kHookRetryTimeoutMs);
            if (ok) {
                std::lock_guard<std::mutex> lock(retryMutex);
                pendingHook = std::move(candidateHook);
                pendingTarget = candidate;
            } else {
                OutputDebugStringW((L"wxr-vid-cap cast: source retry failed for " + candidate + L": " +
                                    retryError + L"\n").c_str());
            }
            retryResultGeneration = generation;
            retrySucceeded = ok;
            retryFinished = true;
            retryInProgress = false;
        });
    };

    // Start with the requested source, then fall back to the current top-most window.
    if (!desiredTarget.empty()) {
        if (!tryHookSource(desiredTarget, hookTimeoutMs)) {
            desiredTarget.clear();
        }
    }
    if (!hookActive) {
        const std::wstring top = TopMostWindowExecutable();
        if (!top.empty()) {
            tryHookSource(top, kHookRetryTimeoutMs);
        }
    }
    if (!hookActive) {
        trace(L"source: no capturable window at startup; sending blank frames and retrying");
    }

    H264Encoder encoder;
    if (!encoder.Initialize(kCaptureWidth, kCaptureHeight, kCaptureFps, kVideoBitrateKbps, initError)) {
        fail(L"Could not start the video encoder: " + initError);
        return;
    }

    AudioCapture audio;
    OpusAudioEncoder opus;
    AudioResampler resampler;
    bool audioCaptureEnabled = false;
    int audioChannels = 2;
    if (audio.Initialize(initError) && audio.Start(initError)) {
        audioChannels = audio.Channels();
        resampler.Initialize(audio.SampleRate(), audioChannels, 48000);
        audioCaptureEnabled = true;
        trace(L"audio: capture " + std::to_wstring(audio.SampleRate()) + L" Hz -> 48000 Hz, " +
              std::to_wstring(audioChannels) + L" ch, 96 kbps");
    } else {
        trace(L"audio capture unavailable; casting silent audio: " + initError);
        audio.Shutdown();
        initError.clear();
    }

    if (!opus.Initialize(48000, audioChannels, kAudioBitrate, initError)) {
        fail(L"Could not start the audio encoder: " + initError);
        return;
    }

    UdpTsSink sink(sender);
    TsMuxer muxer(sink, TsAudioCodec::Opus, audioChannels);

    active = true;
    announced = true;
    ready.set_value({true, L""});
    trace(L"target: " + (hookActive ? targetExecutable : std::wstring(L"waiting for a source")) +
          L", viewer udp://@:" + std::to_wstring(port));

    const int64_t frameTicks = 90000 / kCaptureFps;
    const auto start = std::chrono::steady_clock::now();
    int frameIndex = 0;
    int framesSent = 0;

    const int audioRate = 48000;
    const int opusFrameSamples = opus.FrameSamples();
    std::vector<std::int16_t> audioBuffer;
    std::size_t audioOffset = 0;
    std::int64_t audioSamplesEncoded = 0;
    std::vector<std::int16_t> silence(
        static_cast<std::size_t>(opusFrameSamples) * static_cast<std::size_t>(audioChannels), 0);

    auto drainAudio = [&](std::int64_t upToPts, bool silenceMode) -> bool {
        if (!silenceMode && audioCaptureEnabled) {
            std::vector<std::int16_t> captured;
            if (!audio.Read(captured, error)) {
                return false;
            }
            std::vector<std::int16_t> resampled;
            resampler.Process(captured, resampled);
            audioBuffer.insert(audioBuffer.end(), resampled.begin(), resampled.end());

            for (;;) {
                const std::size_t availableSamples =
                    (audioBuffer.size() - audioOffset) / static_cast<std::size_t>(audioChannels);
                if (availableSamples < static_cast<std::size_t>(opusFrameSamples)) {
                    break;
                }
                const std::int64_t pts = audioSamplesEncoded * 90000 / audioRate;
                if (pts > upToPts) {
                    break;
                }

                std::vector<std::uint8_t> packet;
                if (!opus.Encode(audioBuffer.data() + audioOffset, opusFrameSamples, packet, error)) {
                    return false;
                }
                muxer.WriteAudio(packet.data(), packet.size(), pts);
                audioOffset += static_cast<std::size_t>(opusFrameSamples) *
                               static_cast<std::size_t>(audioChannels);
                audioSamplesEncoded += opusFrameSamples;
            }

            if (audioOffset > 0 && audioOffset >= 48000u * static_cast<std::size_t>(audioChannels)) {
                audioBuffer.erase(audioBuffer.begin(),
                                  audioBuffer.begin() + static_cast<std::ptrdiff_t>(audioOffset));
                audioOffset = 0;
            }
            return true;
        }

        // Silent audio while waiting for a source, or when audio capture is unavailable.
        if (audioCaptureEnabled) {
            std::vector<std::int16_t> discard;
            if (!audio.Read(discard, error)) {
                return false;
            }
        }
        while (opusFrameSamples > 0 && audioSamplesEncoded * 90000 / audioRate <= upToPts) {
            const std::int64_t pts = audioSamplesEncoded * 90000 / audioRate;
            std::vector<std::uint8_t> packet;
            if (!opus.Encode(silence.data(), opusFrameSamples, packet, error)) {
                return false;
            }
            muxer.WriteAudio(packet.data(), packet.size(), pts);
            audioSamplesEncoded += opusFrameSamples;
        }
        return true;
    };

    while (!stopRequested.load()) {
        const std::int64_t pts = static_cast<std::int64_t>(frameIndex) * frameTicks;

        if (frameIndex > 0) {
            if (!drainAudio(pts, !hookActive || !audioCaptureEnabled)) {
                break;
            }
        }

        if (sourceChangeRequested.exchange(false)) {
            std::wstring newSource;
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                newSource = pendingSource;
            }
            if (hookActive) {
                if (hook) {
                    hook->Stop();
                    hook.reset();
                }
                hookActive = false;
            }
            ++retryGeneration;
            targetExecutable.clear();
            desiredTarget = newSource;
            if (!desiredTarget.empty() && IsExcludedExecutablePath(desiredTarget)) {
                desiredTarget.clear();
            }
            resetEncoder = true;
            hookEmptyFrames = 0;
            targetMissingChecks = 0;
            nextRetry = GetTickCount64();
            frame.clear();
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                currentSource.clear();
            }
            trace(L"source: manual source change requested");
        }

        if (hookActive && !targetExecutable.empty() && frameIndex > 0 &&
            frameIndex % kCaptureFps == 0 && hookEmptyFrames > 0) {
            if (TargetStillPresent(targetExecutable, hook->ProcessId())) {
                targetMissingChecks = 0;
            } else if (++targetMissingChecks >= kTargetMissingLimit) {
                enterWaiting();
            }
        } else if (hookEmptyFrames == 0) {
            targetMissingChecks = 0;
        }

        if (hookActive && hookEmptyFrames >= kHookStallFrames) {
            trace(L"hook: no frames for 5 s; dropping source and retrying");
            enterWaiting();
        }

        if (retryFinished.exchange(false)) {
            if (retryThread.joinable()) {
                retryThread.join();
            }

            std::unique_ptr<HookCapture> newHook;
            std::wstring newTarget;
            {
                std::lock_guard<std::mutex> lock(retryMutex);
                newHook = std::move(pendingHook);
                newTarget = pendingTarget;
                pendingTarget.clear();
            }

            const unsigned resultGeneration = retryResultGeneration.load();
            const unsigned currentGeneration = retryGeneration.load();
            if (resultGeneration != currentGeneration) {
                // A manual source change happened while this probe was running.
                if (newHook) {
                    newHook->Stop();
                    newHook.reset();
                }
                nextRetry = GetTickCount64();
            } else if (retrySucceeded.load() && newHook != nullptr) {
                if (hook) {
                    hook->Stop();
                    hook.reset();
                }
                hook = std::move(newHook);
                targetExecutable = newTarget;
                hookActive = true;
                resetEncoder = true;
                hookEmptyFrames = 0;
                targetMissingChecks = 0;
                frame.clear();
                backend = L"graphics hook";
                {
                    std::lock_guard<std::mutex> lock(sourceMutex);
                    currentSource = targetExecutable;
                }
                trace(L"source: hooked " + targetExecutable + L" after retry");
            } else {
                if (newHook) {
                    newHook->Stop();
                    newHook.reset();
                }
                if (!desiredTarget.empty()) {
                    trace(L"source: selected target unavailable; falling back to top-most retries");
                    desiredTarget.clear();
                }
                nextRetry = GetTickCount64() + kSourceRetryIntervalMs;
            }
        }

        if (!hookActive && !retryInProgress.load() && GetTickCount64() >= nextRetry) {
            std::wstring candidate = desiredTarget;
            if (candidate.empty() || IsExcludedExecutablePath(candidate)) {
                if (!candidate.empty()) {
                    trace(L"source: selected target is blacklisted; using top-most retries");
                    desiredTarget.clear();
                }
                candidate = TopMostWindowExecutable();
            }
            if (!candidate.empty()) {
                startRetry(candidate);
            } else {
                nextRetry = GetTickCount64() + kSourceRetryIntervalMs;
            }
        }

        if (resetEncoder) {
            std::wstring encoderError;
            if (!encoder.Initialize(kCaptureWidth, kCaptureHeight, kCaptureFps, kVideoBitrateKbps, encoderError)) {
                error = L"Could not restart the video encoder after switching sources: " + encoderError;
                break;
            }
            resetEncoder = false;
        }

        const std::vector<std::uint8_t>* sourceFrame = &blankFrame;
        if (hookActive && hook) {
            if (hook->Capture(frame, 20, error)) {
                hookEmptyFrames = 0;
                sourceFrame = &frame;
            } else {
                ++hookEmptyFrames;
                if (hookEmptyFrames == 48) {
                    trace(L"hook: no frames for 2 s; repeating the last frame");
                }
                if (!frame.empty()) {
                    sourceFrame = &frame;
                }
            }
        }

        if (sourceFrame->size() != frameBytes) {
            error = L"No frames are available from the capture target.";
            break;
        }

        std::vector<std::uint8_t> i420;
        if (!BgraToI420(sourceFrame->data(), kCaptureWidth, kCaptureHeight, i420)) {
            error = L"Frame conversion failed.";
            break;
        }

        bool keyframe = false;
        std::vector<std::uint8_t> annexB;
        if (!encoder.Encode(i420, pts, keyframe, annexB, error)) {
            break;
        }
        if (!annexB.empty()) {
            muxer.WriteVideo(annexB.data(), annexB.size(), pts, keyframe);
            ++framesSent;
        }

        if (frameIndex == 0) {
            if (!drainAudio(0, !hookActive || !audioCaptureEnabled)) {
                break;
            }
        }

        ++frameIndex;
        const auto deadline =
            start + std::chrono::microseconds(static_cast<long long>(frameIndex) * 1000000 / kCaptureFps);
        std::this_thread::sleep_until(deadline);
    }

    if (error.empty()) {
        drainAudio(std::numeric_limits<std::int64_t>::max(), !hookActive || !audioCaptureEnabled);
    }
    if (audioCaptureEnabled) {
        audio.Stop();
    }
    if (retryThread.joinable()) {
        retryThread.join();
    }
    if (pendingHook) {
        pendingHook->Stop();
        pendingHook.reset();
    }
    sink.Flush();

    trace(L"stopped after " + std::to_wstring(framesSent) + L" frames");
    if (!error.empty()) {
        trace(L"error: " + error);
    }

    active = false;
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

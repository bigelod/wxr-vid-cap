#pragma once

#include "udp_sender.h"

#include <memory>
#include <string>

enum class SessionMode { Idle, Recording, Casting };

// Local recording session: capture + H.264 + Opus + Matroska, all on a worker thread.
class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool Start(const std::wstring& path, const std::wstring& targetExecutable, std::wstring& error,
               int hookTimeoutMs = 6000);
    void Stop();

    bool Active() const;
    bool LastStopWroteFile() const;
    const std::wstring& Path() const;
    const std::wstring& Backend() const;
    const std::wstring& LastError() const;
    std::wstring Source() const;
    // True once after the selected target closes and capture switches to the top-most window.
    // usedGdi is true when the switch ended on the GDI path.
    bool TakeTargetChanged(bool& usedGdi);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// LAN cast session: the same capture and encode path as Recorder, muxed to MPEG-TS and sent over
// UDP. No file is ever opened. Best-effort: there is no receiver acknowledgement, so a cast keeps
// sending as long as capture and encoding work.
class Caster {
public:
    Caster();
    ~Caster();

    Caster(const Caster&) = delete;
    Caster& operator=(const Caster&) = delete;

    bool Start(const std::wstring& host, unsigned short port, const std::wstring& targetExecutable,
               std::wstring& error, int hookTimeoutMs = 6000);
    void Stop();

    // Switches the live cast to another capture source. An empty executable means the top-most
    // window (GDI); otherwise the caster tries the hook first and falls back to GDI.
    bool ChangeSource(const std::wstring& targetExecutable, std::wstring& error);

    bool Active() const;
    const std::wstring& Backend() const;
    const std::wstring& LastError() const;
    std::wstring Source() const;
    // True once after an automatic fallback to the new top-most window.
    bool TakeTargetChanged(bool& usedGdi);
    std::wstring ViewerUrl() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

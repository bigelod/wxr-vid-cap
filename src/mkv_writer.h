#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

// Streaming Matroska writer for a single H.264 track. No external muxer, no temporary file: the
// EBML header, Info, and Tracks are written once, then one Cluster at a time.
//
// The Segment uses an unknown size, which is normal for live/streamed MKV and keeps the writer
// from needing to seek back. Clusters are buffered and written with known sizes, so a crash loses
// at most the current cluster rather than the whole file.
class MkvWriter {
public:
    // annexB is one access unit with start codes, as an encoder emits it. Audio parameters are
    // optional; when channels is 0 the file is video-only.
    MkvWriter(std::ostream& out, int width, int height, int fps, int audioSampleRate = 0, int audioChannels = 0,
              const std::vector<std::uint8_t>& audioCodecPrivate = {});

    MkvWriter(const MkvWriter&) = delete;
    MkvWriter& operator=(const MkvWriter&) = delete;

    bool WriteVideoFrame(const std::uint8_t* annexB, std::size_t size, std::int64_t pts90k, bool keyframe);
    bool WriteAudioFrame(const std::uint8_t* data, std::size_t size, std::int64_t pts90k);
    bool Finalize();

    bool Ok() const { return ok_; }

private:
    bool EnsureHeader(const std::uint8_t* annexB, std::size_t size);
    void FlushCluster();
    void WriteElement(std::uint32_t id, const std::string& payload);
    void WriteRaw(const void* data, std::size_t size);

    std::ostream& out_;
    int width_;
    int height_;
    int fps_;
    int audioSampleRate_ = 0;
    int audioChannels_ = 0;
    std::vector<std::uint8_t> audioCodecPrivate_;
    bool ok_ = true;
    bool headerWritten_ = false;
    bool clusterOpen_ = false;
    std::int64_t clusterTimeMs_ = 0;
    std::int64_t lastPtsMs_ = 0;
    std::string cluster_;
};

#pragma once

#include <cstdint>
#include <vector>

// Streaming linear resampler for interleaved PCM16. Opus needs 48 kHz (or 8/12/16/24), while Wine
// endpoints often mix at 44.1 kHz.
class AudioResampler {
public:
    void Initialize(int inputRate, int channels, int outputRate);
    void Process(const std::vector<std::int16_t>& input, std::vector<std::int16_t>& output);
    int OutputRate() const { return outputRate_; }

private:
    int inputRate_ = 0;
    int channels_ = 0;
    int outputRate_ = 0;
    double position_ = 0.0;
    std::vector<std::int16_t> buffer_;
};

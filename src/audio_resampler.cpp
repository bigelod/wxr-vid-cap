#include "audio_resampler.h"

#include <cstddef>

void AudioResampler::Initialize(int inputRate, int channels, int outputRate)
{
    inputRate_ = inputRate;
    channels_ = channels;
    outputRate_ = outputRate;
    position_ = 0.0;
    buffer_.clear();
}

void AudioResampler::Process(const std::vector<std::int16_t>& input, std::vector<std::int16_t>& output)
{
    output.clear();
    if (input.empty() || channels_ <= 0 || inputRate_ <= 0 || outputRate_ <= 0) {
        return;
    }

    if (inputRate_ == outputRate_) {
        output = input;
        return;
    }

    buffer_.insert(buffer_.end(), input.begin(), input.end());

    const std::size_t totalFrames = buffer_.size() / static_cast<std::size_t>(channels_);
    const double step = static_cast<double>(inputRate_) / static_cast<double>(outputRate_);

    while (position_ + 1.0 < static_cast<double>(totalFrames)) {
        const std::size_t index = static_cast<std::size_t>(position_);
        const double fraction = position_ - static_cast<double>(index);

        for (int channel = 0; channel < channels_; ++channel) {
            const double a = static_cast<double>(buffer_[index * static_cast<std::size_t>(channels_) +
                                                        static_cast<std::size_t>(channel)]);
            const double b = static_cast<double>(buffer_[(index + 1) * static_cast<std::size_t>(channels_) +
                                                        static_cast<std::size_t>(channel)]);
            const double value = a + (b - a) * fraction;
            output.push_back(static_cast<std::int16_t>(value));
        }

        position_ += step;
    }

    const std::size_t consumed = static_cast<std::size_t>(position_);
    if (consumed > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(consumed * static_cast<std::size_t>(channels_)));
        position_ -= static_cast<double>(consumed);
    }
}

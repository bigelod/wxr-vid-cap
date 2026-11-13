#include "opus_audio_encoder.h"

#include "opus.h"

namespace {

std::vector<std::uint8_t> BuildOpusHead(int channels, int preSkip, int sampleRate)
{
    std::vector<std::uint8_t> head;
    const char magic[8] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    head.insert(head.end(), magic, magic + 8);
    head.push_back(1);  // version
    head.push_back(static_cast<std::uint8_t>(channels));
    head.push_back(static_cast<std::uint8_t>(preSkip & 0xFF));
    head.push_back(static_cast<std::uint8_t>((preSkip >> 8) & 0xFF));
    head.push_back(static_cast<std::uint8_t>(sampleRate & 0xFF));
    head.push_back(static_cast<std::uint8_t>((sampleRate >> 8) & 0xFF));
    head.push_back(static_cast<std::uint8_t>((sampleRate >> 16) & 0xFF));
    head.push_back(static_cast<std::uint8_t>((sampleRate >> 24) & 0xFF));
    head.push_back(0);  // output gain, low byte
    head.push_back(0);  // output gain, high byte
    head.push_back(0);  // channel mapping family
    return head;
}

}  // namespace

OpusAudioEncoder::~OpusAudioEncoder()
{
    Shutdown();
}

bool OpusAudioEncoder::Initialize(int sampleRate, int channels, int bitrate, std::wstring& error)
{
    Shutdown();

    if (channels != 1 && channels != 2) {
        error = L"Opus encoder supports mono or stereo only.";
        return false;
    }
    if (sampleRate != 8000 && sampleRate != 12000 && sampleRate != 16000 && sampleRate != 24000 &&
        sampleRate != 48000) {
        error = L"Opus needs an 8/12/16/24/48 kHz input; the endpoint is " + std::to_wstring(sampleRate) + L" Hz.";
        return false;
    }

    int opusError = 0;
    encoder_ = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO, &opusError);
    if (opusError != OPUS_OK || encoder_ == nullptr) {
        error = L"opus_encoder_create failed with code " + std::to_wstring(opusError) + L".";
        Shutdown();
        return false;
    }

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    int lookahead = 0;
    opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead));

    sampleRate_ = sampleRate;
    channels_ = channels;
    frameSamples_ = sampleRate / 50;  // 20 ms
    codecPrivate_ = BuildOpusHead(channels, lookahead, sampleRate);
    return true;
}

bool OpusAudioEncoder::Encode(const std::int16_t* pcm, int frameSamples, std::vector<std::uint8_t>& packet,
                              std::wstring& error)
{
    packet.clear();
    if (encoder_ == nullptr) {
        error = L"The Opus encoder is not initialized.";
        return false;
    }
    if (pcm == nullptr || frameSamples != frameSamples_) {
        error = L"Unexpected Opus frame size.";
        return false;
    }

    std::uint8_t buffer[4000] = {};
    const int encoded = opus_encode(encoder_, pcm, frameSamples, buffer, sizeof(buffer));
    if (encoded < 0) {
        error = L"opus_encode failed with code " + std::to_wstring(encoded) + L".";
        return false;
    }

    packet.assign(buffer, buffer + encoded);
    return true;
}

void OpusAudioEncoder::Shutdown()
{
    if (encoder_ != nullptr) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    codecPrivate_.clear();
    sampleRate_ = 0;
    channels_ = 0;
    frameSamples_ = 0;
}

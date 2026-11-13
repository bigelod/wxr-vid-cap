// Tests for the PCM resampler used to feed Opus at 48 kHz from 44.1 kHz endpoints.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "audio_resampler.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

}  // namespace

int RunAudioTests()
{
    std::printf("wxr-vid-cap audio tests\n");

    // 44.1 kHz -> 48 kHz, one second of a constant signal.
    AudioResampler resampler;
    resampler.Initialize(44100, 2, 48000);

    std::vector<std::int16_t> chunk(4410 * 2, 1000);
    std::vector<std::int16_t> output;
    std::vector<std::int16_t> total;

    for (int i = 0; i < 10; ++i) {
        resampler.Process(chunk, output);
        total.insert(total.end(), output.begin(), output.end());
    }

    const std::size_t outputFrames = total.size() / 2;
    CHECK(outputFrames >= 47998 && outputFrames <= 48002);

    bool constant = true;
    for (std::int16_t sample : total) {
        if (sample != 1000) {
            constant = false;
            break;
        }
    }
    CHECK(constant);

    // Same-rate path is a straight copy.
    AudioResampler passthrough;
    passthrough.Initialize(48000, 2, 48000);
    passthrough.Process(chunk, output);
    CHECK(output.size() == chunk.size());

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}

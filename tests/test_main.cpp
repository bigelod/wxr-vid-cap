// Single entry point for all structural tests. Each suite prints its own summary and returns its
// failure count.

#include <cstdio>

int RunTsMuxerTests();
int RunMkvWriterTests();
int RunAudioTests();

int main()
{
    const int failures = RunTsMuxerTests() + RunMkvWriterTests() + RunAudioTests();
    std::printf(failures == 0 ? "ALL TESTS PASSED\n" : "TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}

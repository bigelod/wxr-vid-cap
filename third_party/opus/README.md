# Opus (vendored)

- Version: **1.5.2**
- Upstream: https://github.com/xiph/opus
- License: BSD-3-Clause (see `LICENSE`)
- Artifact: `lib/opus.lib`, static x64 Release, built with the Visual Studio 2022 toolset and
  `OPUS_STATIC_RUNTIME=ON` so it links cleanly into this project's `/MT` binaries.
- SHA-256: `84CD80BD43A71546CAE7BC87282A70BCCF7B37B5864A38F5A408DFF8B47C10DC`

Build command used:

```
cmake -S opus-1.5.2 -B build -G "Visual Studio 17 2022" -A x64 ^
  -DOPUS_STATIC_RUNTIME=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF ^
  -DOPUS_BUILD_SHARED_LIBRARY=OFF
cmake --build build --config Release --parallel
```

Used for the `A_OPUS` audio track in the local recorder. Opus is royalty-free.

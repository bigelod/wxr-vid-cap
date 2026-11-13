# OpenH264 (vendored)

- Version: **2.4.1**
- Upstream: https://github.com/cisco/openh264
- License: BSD-2-Clause (see `LICENSE`)
- Runtime: `bin/openh264.dll`, loaded dynamically by `src/h264_encoder.cpp` from beside the EXE
- Binary source: `https://ciscobinary.openh264.org/openh264-2.4.1-win64.dll.bz2`
- SHA-256: `081B0C081480D177CBFDDFBC90B1613640E702F875897B30D8DE195CDE73DD34`

The DLL depends only on `KERNEL32.dll`, so it does not require a VC++ redistributable inside the
Winlator container.

## Patent note

The source and binary are BSD-licensed, but H.264 is patent-encumbered. Cisco's royalty coverage
applies to binaries distributed by Cisco; redistributing a self-built or renamed copy may not
inherit that coverage. This is a product decision, not a technical one. The alternative encoders
under consideration are minih264 (public domain, intra-only) and libjpeg-turbo MJPEG (BSD, no
H.264 patents).

## API surface used

`WelsCreateSVCEncoder`, `WelsDestroySVCEncoder`, and the `ISVCEncoder` interface from
`include/codec_api.h`.

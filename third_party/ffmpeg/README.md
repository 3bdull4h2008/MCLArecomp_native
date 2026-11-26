# FFmpeg (XMA subset)

- **Source:** Xenia's FFmpeg fork, copied unmodified from the local checkout
  `.research/xenia/third_party/FFmpeg` (no git metadata there, so no commit hash).
- **Version:** FFmpeg 4.4 (`RELEASE`), libavcodec 58.134.100, plus Xenia's
  `AV_CODEC_ID_XMAFRAMES` / `ff_xmaframes_decoder` patch in `libavcodec/wmaprodec.c`.
- **License:** LGPL-2.1-or-later, see `COPYING.LGPLv2.1`.
- **Use:** statically linked into `mcla.exe` as the `ffmpeg_xma` target, for XMA
  audio decoding only (`src/apu/xma_ffmpeg.cpp`). Config is Xenia's
  `config_windows_x86_64.h` (no x86 asm, w32threads, only wmapro/xmaframes enabled).
- **Contents:** only the `libavcodec`/`libavutil` sources linked into the static lib,
  plus the headers they include. The file set was taken from Xenia's premake lists
  and trimmed to what the linker pulls into `mcla.exe`.

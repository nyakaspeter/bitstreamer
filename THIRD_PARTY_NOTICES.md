# Third-party software

bitstreamer's own source code is MIT licensed (see `LICENSE`). Release packages
additionally contain the unmodified third-party software below, as separate
DLLs.

| Component | Files | License | Source |
|---|---|---|---|
| FFmpeg (libavcodec, libavutil) | `avcodec-*.dll`, `avutil-*.dll` | LGPL-2.1-or-later | https://ffmpeg.org |
| Windows App SDK 1.7 runtime (WinUI 3) | `Microsoft.*.dll`, `WinUI*.dll`, other runtime files and language folders | Microsoft Software License Terms (not open source; redistribution with an application is permitted) | https://github.com/microsoft/WindowsAppSDK |

## FFmpeg (LGPL)

* FFmpeg is built from source by [vcpkg](https://github.com/microsoft/vcpkg)
  with its default configuration for the `avcodec` feature only: no
  `--enable-gpl`, no `--enable-nonfree`, no external codec libraries.
* It is linked dynamically, so the DLLs can be replaced with compatible builds.
* The exact source code is the upstream release selected by the vcpkg baseline
  commit in `vcpkg.json`; anyone can rebuild the DLLs with `build.ps1` and drop
  them next to the executable.
* The licence text is at
  https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html; the copyright file
  vcpkg ships for FFmpeg is in the release package under `licenses/`.

## Windows App SDK (Microsoft)

* The user interface is built with WinUI 3. Its runtime is Microsoft's and is
  shipped beside the app unmodified, as downloaded from NuGet
  (`Microsoft.WindowsAppSDK`), so nothing has to be installed to run the app.
* It is covered by Microsoft's licence terms, not by an open-source licence.
  Those terms and Microsoft's third-party notices are in the release package
  under `licenses/`.

## Codecs, patents and trademarks

* **Dolby Digital (AC-3)** encoding uses FFmpeg's native `ac3` encoder. The
  patents covering AC-3 are generally understood to have expired (the last in
  2017). "Dolby" and "Dolby Digital" are trademarks of Dolby Laboratories.
  bitstreamer is not certified, licensed or endorsed by Dolby.
* **DTS** encoding uses FFmpeg's `dca` core encoder, which FFmpeg marks
  experimental. "DTS" is a trademark of DTS, Inc. (Xperi).
  bitstreamer is not certified, licensed or endorsed by DTS.

Nothing in this file is legal advice. Distributors are responsible for
checking patent and trademark requirements in their own jurisdiction.

## Runtime prerequisite (not bundled)

* **Steam Streaming Speakers** by Valve. bitstreamer does not contain or
  redistribute this driver. It installs the copy that Steam placed on the
  user's computer (`%CommonProgramFiles(x86)%\Steam\drivers`), which is signed
  through Microsoft's hardware compatibility program, and uses the resulting
  playback device as its input. Steam must therefore be installed.

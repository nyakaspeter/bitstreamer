# Third-party software

bitstreamer's own source code is MIT licensed (see `LICENSE`). Release packages
additionally contain the unmodified third-party libraries below, built from
source by [vcpkg](https://github.com/microsoft/vcpkg) at the registry baseline
pinned in `vcpkg.json`. All of them are linked **dynamically** (separate DLLs),
so they can be replaced by the user with compatible builds.

| Component | Files | License | Source |
|---|---|---|---|
| FFmpeg (libavcodec, libavutil) | `avcodec-*.dll`, `avutil-*.dll` | LGPL-2.1-or-later | https://ffmpeg.org |
| Qt 6 (qtbase: Core, Gui, Widgets, platform and style plugins) | `Qt6*.dll`, `platforms/`, `styles/` | LGPL-3.0-only | https://www.qt.io |
| PCRE2 | `pcre2-16.dll` | BSD-3-Clause | https://github.com/PCRE2Project/pcre2 |
| double-conversion | `double-conversion.dll` | BSD-3-Clause | https://github.com/google/double-conversion |
| zlib | `libz.dll` / `zlib*.dll` | Zlib | https://zlib.net |
| MD4C | `md4c.dll` | MIT | https://github.com/mity/md4c |

## LGPL compliance notes

* **FFmpeg** is built with vcpkg's default configuration for the `avcodec`
  feature only: no `--enable-gpl`, no `--enable-nonfree`, no external codec
  libraries.
* **Qt** is used under the LGPL v3. No Qt source file is modified.
* The exact source code of both libraries is the upstream release selected by
  the vcpkg baseline commit in `vcpkg.json`; `vcpkg install` downloads and
  builds it reproducibly. Anyone can rebuild the DLLs with `build.ps1` and
  drop them next to the executables.
* The full license texts are available at
  https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html and
  https://www.gnu.org/licenses/lgpl-3.0.html. The copyright and license files
  that vcpkg ships for every bundled library are included in the release
  package under `licenses/`.

## Codecs, patents and trademarks

* **Dolby Digital (AC-3)** encoding uses FFmpeg's native `ac3` encoder. The
  patents covering AC-3 are generally understood to have expired (the last in
  2017). "Dolby" and "Dolby Digital" are trademarks of Dolby Laboratories.
  bitstreamer is not certified, licensed or endorsed by Dolby.
* **DTS** encoding (optional, experimental, off by default) uses FFmpeg's
  experimental `dca` core encoder. "DTS" is a trademark of DTS, Inc. (Xperi).
  bitstreamer is not certified, licensed or endorsed by DTS.

Nothing in this file is legal advice. Distributors are responsible for
checking patent and trademark requirements in their own jurisdiction.

## Runtime prerequisite (not bundled)

* **Steam Streaming Speakers** by Valve. bitstreamer does not contain or
  redistribute this driver. It installs the copy that Steam placed on the
  user's computer (`%CommonProgramFiles(x86)%\Steam\drivers`), which is signed
  through Microsoft's hardware compatibility program, and uses the resulting
  playback device as its input. Steam must therefore be installed.

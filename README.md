# The MMW

Native Android rhythm chart editor based on MikuMikuWorld, with touch editing,
chart preview, video backgrounds, SEKAI intros and MP4 export.

## Downloads

Download installable APKs from [Releases](https://github.com/youkai-ux/MikuMikuWorld4Android/releases).
See [the build archive](docs/RELEASES.md) for the dated APK history and checksums.

| Android app | Package | Architectures |
| --- | --- | --- |
| The MMW | `com.the.mmw` | ARM64 |

MMW SEKAI Live is a separate app with adjustable forward-leaning note surfaces
in preview and MP4 export. It recreates the appearance in the existing renderer;
it does not contain the Project SEKAI Unity runtime. Both apps can be installed
together and keep their own private settings and imported files.

Older archived APKs use `com.mikumikuworld.editor`. Historical releases may
contain bugs fixed by later builds; use the latest build of the desired app.

## Build from source

### Android

Open `android/` in Android Studio, or use Gradle 9.1 with Android SDK 35,
Build Tools 36.1.0, NDK 30.0.16248370 and CMake 4.1.2:

```powershell
gradle -p android :app:assembleDebug
gradle -p android :sekaiLive:assembleDebug
```

See [Android build details](android/README.md) and [SEKAI Live details](android/sekai-live/README.md).
FFmpeg dependencies and the corresponding FFmpeg source archive are included.
Android video export uses the bundled libraries and does not require an external
FFmpeg executable. Signing keys, editor preferences and build caches are private
local files and are not included in this repository.

### Windows

Open `MikuMikuWorld.sln` in Visual Studio 2022 and choose **Release | x64**, or:

```powershell
cmake -S . -B windows-build -A x64
cmake --build windows-build --config Release --parallel 2
```

Keep the generated `res` directory alongside the executable. Windows MP4 export
and video backgrounds require FFmpeg available in PATH.

### iOS

See [iOS build instructions](ios/README.md). The iOS target and workflows are
included; an unsigned IPA requires signing before installation.

## Source and release history

The initial upload is the current source snapshot. Older APKs are archived using
their filesystem **last-written build timestamps in Asia/Saigon (UTC+7)**.
Each release includes SHA-256 checksums and build metadata. Archive tags point
to the uploaded source snapshot; they do not reconstruct older source revisions.
GitHub's publication time is the upload time, while titles and tags retain build
time. The latest regular and SEKAI Live builds are published separately.

Noto CJK is bundled as the default font. Optional personal Rodin font files are
not included in the public source; the renderer falls back to the bundled font.

## Credits and license

- Original project: [crash5band/MikuMikuWorld](https://github.com/crash5band/MikuMikuWorld)
- Original fork: [watagashi-uni](https://github.com/watagashi-uni/)
- Android port: **-YOUKAI- and the (Discord @desktop2202, blandora2000)**
- SEKAI overlay reference: [TootieJin/pjsekai-overlay-APPEND](https://github.com/TootieJin/pjsekai-overlay-APPEND)

This fork is distributed under **AGPL-3.0-only**. Preserve [LICENSE](LICENSE),
[third-party notices](Licenses) and the [SEKAI asset notices](MikuMikuWorld/res/editor/sekai_intro/NOTICE.txt).

## Original fork notes

# 外部非公式アプリ

在源项目的基础上
- 增加谱面 Maker 专用 JSON 格式的导入导出
- 增加 SUS 的 Notespeed 支持
- 增加本家风格的谱面预览

本项目使用并改造了以下开源项目：
- [crash5band/MikuMikuWorld](https://github.com/crash5band/MikuMikuWorld)（MIT）
- [TootieJin/pjsekai-overlay-APPEND](https://github.com/TootieJin/pjsekai-overlay-APPEND)（AGPL）

由于引入 AGPL 代码，本项目按 **AGPL-3.0-only** 发布。

- 项目许可证：`/LICENSE`

原项目 MIT 协议：
```text
MIT License

Copyright (c) 2022 Crash5b

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

```

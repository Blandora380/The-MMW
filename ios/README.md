# iOS cloud build

This target builds the shared C++ chart editor for arm64 iPhones and iPads running iOS 15 or later. It uses UIKit, OpenGL ES 3, the iOS Files picker, and AVFoundation for MP4 encoding. It does not need FFmpeg on PATH.

## Build without a Mac

Push the source to `youkai-ux/MikuMikuWorld4Android`. Under **Actions → iOS IPA**, open the latest run, or choose **Run workflow**. Download the **MikuMikuWorld-iOS-unsigned** artifact after a successful run and extract the `.ipa`.

The workflow runs on a GitHub macOS runner. No signing certificate, provisioning profile, or Apple account is stored in the repository. The IPA is unsigned: sign it with your chosen sideloading tool before installing it. An unsigned IPA cannot be installed directly.

## Platform behavior

- Landscape editor with a 16:9 canvas, Retina rendering, long press context menus, and timeline pinch zoom.
- Imports are copied into the app's Documents/Imports directory so the editor can reopen them later.
- Saves and exports are staged in Documents/Exports, then offered through the iOS Files destination picker. These local copies remain accessible through Files even when destination selection is cancelled.
- Preferences and presets live in the app's writable Application Support folder, separate from bundled resources.
- MP4 export includes the selected music and note sounds. AVFoundation supports Apple-supported media formats; format compatibility may differ from Android's FFmpeg build.
- Exports pause while the app is inactive and resume in the foreground. iOS does not allow this OpenGL renderer to keep rendering after switching apps. The Android foreground export service and YouTube mini player are not included in this target.
- Native video background decoding currently uses precise AVAssetImageGenerator frame requests; video-heavy charts may export more slowly than charts with a static background.

The cloud build checks compilation and packaging. On-device behavior still needs to be checked after signing and installing on an iPhone or iPad.

In the public cloud source snapshot, artwork/audio are stored as archives under `ios/assets`. The workflow reconstructs and extracts these archives and downloads the Noto CJK font from a pinned official upstream revision before building. For local builds, run `python3 ios/prepare_resources.py` and download the Noto font using the URLs in the workflow.

## Attribution

Original project: https://github.com/crash5band/MikuMikuWorld

Original fork: https://github.com/watagashi-uni/

Android port: -YOUKAI- (Discord @desktop2202)

Preserve the root LICENSE, Licenses directory, and bundled asset notices when redistributing. Personal Rodin font installations are not part of the public source snapshot; the bundled Noto font is the fallback.

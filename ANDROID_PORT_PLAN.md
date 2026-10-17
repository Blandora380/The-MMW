# Android port

> Historical porting notes. Some limitations below have since been resolved,
> including Android MP4 export. See [android/README.md](android/README.md) for
> current build instructions and features.

The project now includes a native Android app under `android/`. It keeps the C++ chart editor and Dear ImGui UI, uses an EGL/OpenGL ES 3 shell, and includes Android-specific touch, software keyboard, app lifecycle, and document-picker support.

## Supported editor features

- Chart editing and preview rendering
- Audio playback and chart assets bundled from `MikuMikuWorld/res`
- Opening and saving charts through Android's document picker
- Importing images, audio, and presets through the same picker
- Touch input, hardware keyboard input, and the Android software keyboard for text fields
- ARM64 and x86_64 native builds

Video background decoding and MP4 rendering are not available in the Android build; MP4 export is hidden there.

## Build and install

Open the `android/` directory in Android Studio and build or run the `app` configuration. The project uses Android Gradle Plugin 9.0.1, SDK platform 35, NDK 30.0.16248370, and CMake 4.1.2. A signed debug APK is also included at `android/MikuMikuWorld-Android.apk` for direct installation.

The signed APK was assembled for ARM64 and x86_64 and installed and launched successfully in the Android 16 x86_64 emulator. The native startup crash found during that launch was fixed by moving the initial viewport setup after GL loader initialization and removing an unsupported GLES point-size call. The local AAPT/zipalign tools still crash while writing packages and the installed Gradle distribution is incomplete, so the checked APK was assembled from the Android build outputs. A physical-device launch and document-picker interaction have not been checked yet.

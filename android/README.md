# MikuMikuWorld for Android

Open this folder in Android Studio to build and run the app. The `app` module targets Android 8.0 (API 26) and newer, and builds native code for ARM64 and x86_64.

The editor opens in landscape. Use touch for the editor controls; text fields bring up the Android keyboard. Open and save charts, and import assets, with the system document picker.

Download signed APKs from [GitHub Releases](https://github.com/youkai-ux/MikuMikuWorld4Android/releases). Copy an APK to an Android device and open it to install. Android may ask you to allow installs from the file browser used to open the APK.

Video background preview and MP4 export use the bundled FFmpeg dependencies.
Export can continue while switching apps using a foreground service and progress
notification. Android will ask for the permissions needed by these features.

Build the original app with `gradle -p android :app:assembleDebug` from the
repository root. Its package name is `com.mm4andr.youkai`.

The separate `sekaiLive` module uses package `com.example.mmw` and launcher name
**MMW SEKAI Live**. Build it with `gradle -p android :sekaiLive:assembleDebug`.
It includes adjustable forward-leaning notes and can be installed alongside the
original app. See [its README](sekai-live/README.md).

Both variants include **Settings > Preview > General > Note margin**. The
default insets each edge by 0.020 lanes, giving adjacent note heads a small
gap in preview and exported video. Set it to zero to remove the gap.

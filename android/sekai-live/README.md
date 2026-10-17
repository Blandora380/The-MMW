# MMW SEKAI Live

Separate Android application module for the forward-leaning note renderer.

- Application ID: `com.example.mmw`
- Launcher name: **MMW SEKAI Live**
- Architecture: ARM64
- Build task: `gradle -p android :sekaiLive:assembleDebug`
- APK: `android/sekai-live/build/outputs/apk/debug/sekaiLive-debug.apk`

Install this alongside the original `com.mm4andr.youkai` app. Android gives
each app its own private settings, imported files and permissions. Open your
charts in this app through the file picker; existing private settings are not
automatically migrated.

Settings > Preview > General contains **Forward-leaning notes** and **Note lean**.
The default is enabled at 80%. Disable it or set lean to zero for the previous
note geometry. Preview and MP4 export use the same renderer.

This recreates the tilted note surface in the project's existing OpenGL renderer.
It does not include or run Project SEKAI's Unity engine or game code. Note skins,
hit timing, lane paths, hold ribbons and effects retain their existing behavior.
Perspective-correct texture interpolation preserves the note artwork across the
three joined sprite slices.

The renderer is compiled only when `MIKUMIKUWORLD_SEKAI_LIVE_RENDERING=ON`.
The original `:app:assembleDebug` task continues to build the original package.

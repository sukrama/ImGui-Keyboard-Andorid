# ImGui Android — Soft Keyboard via InMemoryDex

Dear ImGui overlay for Android with soft keyboard support. The keyboard bridge is a DEX payload embedded as a C byte array inside the `.so` — loaded at runtime via `InMemoryDexClassLoader`, no file written to disk.

---

## How It Works

ImGui has no Android `View` hierarchy, so the system doesn't know when to show the keyboard. This is solved by embedding `Helper.java` as a compiled DEX array (`dex_data.h`) and loading it with `InMemoryDexClassLoader` (API 26+) on `JNI_OnLoad`. The loaded class creates an invisible `View` that the IME attaches to, and bridges keyboard input back into `ImGuiIO` via JNI native methods.

```
JNI_OnLoad
    └── LoadDex(imgui_dex, imgui_dex_len)
            └── InMemoryDexClassLoader
                    └── Helper
                            ├── activity()          → resolve current Activity (reflection)
                            ├── showKeyboard(bool)  → show/hide soft keyboard
                            ├── nativeSendChar(int) → io.AddInputCharacter()
                            └── nativeSendKey(int)  → io.AddKeyEvent()
```

---

## Key Files

| File | Description |
|---|---|
| `native-lib.cpp` | Entry point, render hook, input hook |
| `Includes/jni.hpp` | `LoadDex`, `GetClass`, `RegisterNativeMethods`, `ShowKeyboard`, `Toast` |
| `Includes/dex_data.h` | Compiled `Helper.java` as `unsigned char[]` |
| `sources keyboard/Helper.java` | Sources keyboard 

---

## Regenerate `dex_data.h`

If you modify `Helper.java`:

```bash
javac -source 8 -target 8 Helper.java
d8 --min-api 26 Helper.class --output ./
xxd -i classes.dex > Includes/dex_data.h
```

---

## Build

```bash
# CMake — configured in CMakeLists.txt, targets arm64-v8a, armeabi-v7a, C++20
# Dobby is built from source and statically linked (Includes/Dobby).
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
cmake --build build
```

Output: `libDemo.so`

---

## Injection

Open `AndroidManifest.xml` and find the `<activity>` that has:

```xml
<action android:name="android.intent.action.MAIN" />
<category android:name="android.intent.category.LAUNCHER" />
```

The `android:name` attribute on that `<activity>` is the launcher class — e.g. `com.unity3d.player.UnityPlayerActivity`. Open its smali at `com.unity3d.player.UnityPlayerActivity.smali` and insert inside `onCreate`

```smali
const-string v0, "Demo"

invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
```

Place `libDemo.so` in `lib/arm64-v8a/` and/or `lib/armeabi-v7a/` inside the APK, then repack, sign, and install. `JNI_OnLoad` fires automatically on load.

---

## Requirements

- Android API 26+
- NDK r25+
- C++20

---

## Notes

- `Toast()` is available but commented out by default — uncomment in `JNI_OnLoad` to verify DEX loaded successfully.
- Font path `/system/fonts/NotoSansCJK-Regular.ttc` may not exist on all devices — adjust as needed.
- already support `Japanese` characters
- universal keyboard support any `game` non-unity and unity

---

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) — Immediate mode GUI for C++
- [Dobby](https://github.com/jmpews/Dobby) — Lightweight, multi-platform hook framework (built from source, `dev` branch)
- [KittyMemory](https://github.com/MJx0/KittyMemory) — Android memory patching library

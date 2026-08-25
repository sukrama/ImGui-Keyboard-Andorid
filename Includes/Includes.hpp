#pragma once

#include <jni.h>
#include <android/log.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <elf.h>
#include <inttypes.h>
#include <stdint.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <istream>
#include <ostream>
#include <streambuf>
#include <iomanip>
#include <string>
#include <codecvt>
#include <array>
#include <vector>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <functional>
#include <tuple>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <random>
#include <numbers>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cwctype>
#include <format>
#include <dobby.h>
#include <frida-gum.h>
#include <gumpp.hpp>
#include <asmjit/asmjit.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_android.h>
#include <imgui_impl_opengl3.h>
#include <jni.hpp>

#include <MemoryPatch.h>
#include <il2cpp_dump.h>
#include <UnityResolve.hpp>
#include <log.h>
#include <Tool/Keyboard.hpp>
#include <Tool/Tool.hpp>
#include <Tool/HookerData.hpp>
#include <Tool/Unity.hpp>
#include <Tool/Config.hpp>

inline bool setup;
inline int glWidth, glHeight;

// UI state + helpers shared by the overlay menu (defined here, used by native-lib.cpp)
inline std::atomic<bool> toolReady{false};

inline bool fullScreen = false;
inline bool resetWindow = false;
inline bool doChangeScale = false;
inline int selectedScale = 3;
inline ImGuiStyle initialStyle;
inline bool collapsed = false;
inline auto wmStart = std::chrono::high_resolution_clock::now();

constexpr std::array<const char*, 7> possibleScale = {
    "Smallest", "Smaller", "Small", "Default", "Large", "Larger", "Largest",
};
constexpr std::array<float, 7> scaleFactors = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

inline ImVec4 hsv2rgb(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    if (h < 1.0f/6.0f) { r = c; g = x; b = 0; }
    else if (h < 2.0f/6.0f) { r = x; g = c; b = 0; }
    else if (h < 3.0f/6.0f) { r = 0; g = c; b = x; }
    else if (h < 4.0f/6.0f) { r = 0; g = x; b = c; }
    else if (h < 5.0f/6.0f) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    return ImVec4(r + m, g + m, b + m, 1.0f);
}

inline bool IsLibraryLoaded(const char* libName) {
    void* handle = dlopen(libName, RTLD_NOW | RTLD_NOLOAD);
    if (handle) { dlclose(handle); return true; }
    return false;
}

inline void openURL(const char* url) {
    auto appClass = UnityResolve::FindClass("UnityEngine.Application");
    auto openURLMethod = appClass ? appClass->Get<UnityResolve::Method>("OpenURL") : nullptr;
    if (openURLMethod && openURLMethod->address) {
        auto str = il2cpp_string_new(url);
        void* args[] = { str };
        void* exc = nullptr;
        il2cpp_runtime_invoke(openURLMethod->address, nullptr, args, &exc);
    } else {
        Keyboard::Open(url, nullptr);
    }
}

#define HOOKINPUT(ret, func, ...)                                                                                      \
    ret (*orig##func)(__VA_ARGS__);                                                                                    \
    ret my##func(__VA_ARGS__)

HOOKINPUT(void, Input, void *thiz, void *ex_ab, void *ex_ac)
{
    origInput(thiz, ex_ab, ex_ac);
    if (setup)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    return;
}

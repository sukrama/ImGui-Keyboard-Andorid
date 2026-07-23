#pragma once

#include <jni.h>
#include <android/log.h>

#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <utime.h>
#include <link.h>
#include <elf.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <assert.h>
#include <setjmp.h>
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
#include <ios>
#include <iosfwd>
#include <iomanip>
#include <string>
#include <string_view>
#include <charconv>
#include <codecvt>
#include <vector>
#include <array>
#include <deque>
#include <list>
#include <forward_list>
#include <stack>
#include <queue>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <bitset>
#include <span>
#include <algorithm>
#include <numeric>
#include <functional>
#include <iterator>
#include <execution>
#include <ranges>
#include <memory>
#include <memory_resource>
#include <new>
#include <scoped_allocator>
#include <utility>
#include <tuple>
#include <optional>
#include <variant>
#include <any>
#include <type_traits>
#include <typeinfo>
#include <typeindex>
#include <concepts>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <atomic>
#include <barrier>
#include <latch>
#include <semaphore>
#include <stop_token>
#include <chrono>
#include <ratio>
#include <ctime>
#include <random>
#include <complex>
#include <valarray>
#include <numbers>
#include <bit>
#include <compare>
#include <regex>
#include <locale>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <climits>
#include <cfloat>
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cassert>
#include <cerrno>
#include <clocale>
#include <cwchar>
#include <cwctype>
#include <cuchar>
#include <initializer_list>
#include <source_location>
#include <version>
#include <format>
#include <dobby.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_android.h>
#include <imgui_impl_opengl3.h>
#include <jni.hpp>
#include <il2cpp_dump.h>
#include <UnityResolve.hpp>
#include <log.h>
#include <asmjit/asmjit.h>
#include <gumpp.hpp>

static inline int DobbyHookCompat(void *address, void *replace_func, void **origin_func) {
    return DobbyHook(address, (dobby_dummy_func_t)replace_func, (dobby_dummy_func_t *)origin_func);
}
#define DobbyHook DobbyHookCompat

inline bool setup;
inline int glWidth, glHeight;

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
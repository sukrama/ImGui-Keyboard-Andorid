#pragma once

#include <jni.h>
#include <imgui.h>
#include <dex_data.h>
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>

inline JavaVM *jvm = nullptr;
inline bool Loader = false;
inline jobject gDexLoader = nullptr;

// IME callbacks arrive on the Android UI thread; ImGuiIO is consumed on the
// render thread inside NewFrame(). Buffer events here and drain them on the
// render thread — ImGuiIO is not safe to touch from two threads.
struct PendingInput {
    enum Kind { Char, Key } kind;
    unsigned int codepoint;
    ImGuiKey key;
};

inline std::mutex gInputMutex;
inline std::deque<PendingInput> gInputQueue;

struct JniScope {
    JNIEnv *env = nullptr;
    bool attached = false;

    JniScope() {
        if (jvm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = jvm->AttachCurrentThread(&env, nullptr) == JNI_OK;
        if (env)
            env->PushLocalFrame(16);
    }

    ~JniScope() {
        if (env) {
            if (env->ExceptionCheck())
                env->ExceptionClear();
            env->PopLocalFrame(nullptr);
        }
        if (attached)
            jvm->DetachCurrentThread();
    }

    JniScope(const JniScope &) = delete;
    JniScope &operator=(const JniScope &) = delete;

    explicit operator bool() const { return env != nullptr; }
    operator JNIEnv *() const { return env; }
    JNIEnv *operator->() const { return env; }
};

static jclass GetClass(JNIEnv *env, const char *name)
{
    if (!gDexLoader)
        return nullptr;

    jclass loaderClass = env->GetObjectClass(gDexLoader);
    jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClass)
        return nullptr;

    jclass cls = (jclass)env->CallObjectMethod(gDexLoader, loadClass, env->NewStringUTF(name));

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return cls;
}

static void nativeSendChar_impl(JNIEnv *, jclass, jint unicode)
{
    std::lock_guard<std::mutex> lock(gInputMutex);
    gInputQueue.push_back({ PendingInput::Char, (unsigned int)unicode, ImGuiKey_None });
}

static void nativeSendKey_impl(JNIEnv *, jclass, jint keyCode)
{
    static const std::unordered_map<int, ImGuiKey> map = {
        { 67,  ImGuiKey_Backspace  },
        { 112, ImGuiKey_Delete     },
        { 66,  ImGuiKey_Enter      },
        { 21,  ImGuiKey_LeftArrow  },
        { 22,  ImGuiKey_RightArrow },
        { 19,  ImGuiKey_UpArrow    },
        { 20,  ImGuiKey_DownArrow  },
    };

    auto it = map.find(keyCode);
    if (it == map.end())
        return;

    std::lock_guard<std::mutex> lock(gInputMutex);
    gInputQueue.push_back({ PendingInput::Key, 0, it->second });
}

// Call once per frame on the render thread, before ImGui::NewFrame().
static void DrainInputQueue()
{
    std::deque<PendingInput> pending;
    {
        std::lock_guard<std::mutex> lock(gInputMutex);
        pending.swap(gInputQueue);
    }

    ImGuiIO &io = ImGui::GetIO();
    for (const PendingInput &ev : pending) {
        if (ev.kind == PendingInput::Char) {
            io.AddInputCharacter(ev.codepoint);
        } else {
            io.AddKeyEvent(ev.key, true);
            io.AddKeyEvent(ev.key, false);
        }
    }
}

static void RegisterNativeMethods(JNIEnv *env)
{
    jclass helper = GetClass(env, "Helper");
    if (!helper)
        return;

    JNINativeMethod methods[] = {
        { "nativeSendChar", "(I)V", (void *)nativeSendChar_impl },
        { "nativeSendKey",  "(I)V", (void *)nativeSendKey_impl  },
    };
    env->RegisterNatives(helper, methods, 2);
}

static void LoadDex(unsigned char *dex, int size)
{
    JniScope env;
    if (!env)
        return;

    jbyteArray dexArray = env->NewByteArray(size);
    env->SetByteArrayRegion(dexArray, 0, size, (jbyte *)dex);

    jclass bufferClass = env->FindClass("java/nio/ByteBuffer");
    jobject buffer = env->CallStaticObjectMethod(bufferClass,
        env->GetStaticMethodID(bufferClass, "wrap", "([B)Ljava/nio/ByteBuffer;"), dexArray);
    if (!buffer)
        return;

    jclass appThread = env->FindClass("android/app/ActivityThread");
    jobject app = env->CallStaticObjectMethod(appThread,
        env->GetStaticMethodID(appThread, "currentApplication", "()Landroid/app/Application;"));

    jclass ctx = env->FindClass("android/content/Context");
    jobject parent = env->CallObjectMethod(app,
        env->GetMethodID(ctx, "getClassLoader", "()Ljava/lang/ClassLoader;"));

    jclass loaderClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!loaderClass)
        return;

    jmethodID ctor = env->GetMethodID(loaderClass, "<init>",
        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!ctor)
        return;

    jobject dexLoader = env->NewObject(loaderClass, ctor, buffer, parent);
    if (!dexLoader)
        return;

    gDexLoader = env->NewGlobalRef(dexLoader);

    jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!env->CallObjectMethod(gDexLoader, loadClass, env->NewStringUTF("Helper")))
        return;

    Loader = true;
    RegisterNativeMethods(env);
}

static void Toast(const char *message)
{
    JniScope env;
    if (!Loader || !env)
        return;

    jclass helper = GetClass(env, "Helper");
    if (!helper)
        return;

    jmethodID mid = env->GetStaticMethodID(helper, "showToast", "(Ljava/lang/String;)V");
    if (mid)
        env->CallStaticVoidMethod(helper, mid, env->NewStringUTF(message));
}

static void ShowKeyboard(bool show)
{
    JniScope env;
    if (!Loader || !env)
        return;

    jclass helper = GetClass(env, "Helper");
    if (!helper)
        return;

    jmethodID mid = env->GetStaticMethodID(helper, "showKeyboard", "(Z)V");
    if (mid)
        env->CallStaticVoidMethod(helper, mid, (jboolean)show);
}

static std::string GetPath(const char *type)
{
    JniScope env;
    if (!Loader || !env)
        return "";

    jclass helper = GetClass(env, "Helper");
    if (!helper)
        return "";

    jmethodID mid = env->GetStaticMethodID(helper, "getPath",
                                           "(Ljava/lang/String;)Ljava/lang/String;");
    if (!mid)
        return "";

    jstring jpath = (jstring)env->CallStaticObjectMethod(helper, mid, env->NewStringUTF(type));
    if (!jpath)
        return "";

    const char *c = env->GetStringUTFChars(jpath, nullptr);
    std::string result = c ? c : "";
    if (c)
        env->ReleaseStringUTFChars(jpath, c);
    return result;
}

static inline std::string GetDocumentsPath() { return GetPath("Documents"); }
static inline std::string GetDownloadPath()  { return GetPath("Download");  }
static inline std::string GetPicturePath()   { return GetPath("Pictures");  }
static inline std::string GetDCIMPath()      { return GetPath("DCIM");      }

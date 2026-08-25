#pragma once
// ESP overlay — 1:1 port of "Unity Controller v2" ESP stack
// (ObjectScanner.h passive get_transform harvest + EspTester.h DrawESP)
// onto UnityResolve.hpp. Only line/box/name. No aimbot, no chams.
//
// Same architecture as the original:
//   - Hook UnityEngine.Component::get_transform via frida-gum Interceptor
//     (original used MSHookFunction). Every Transform the game resolves is
//     inspected on a ThreadPool; class catalog built from Behaviour-derived
//     classes; user picks espclass from dropdown; Destroy-hook removes dead.
//   - DrawESP runs every frame on the render thread with identical math:
//     W2S feet + head(+1.7f), Hight=|dY|, Width=Hight*0.6, Y-flip glH-Y,
//     snapline from {glW/2,20}, AddRect rounding 1.4, name above box.
//   - sceneCount>1 guard wipes state and shows "Loading...".

#include <Includes.hpp>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cmath>

namespace OBJECT_SCANNER {

inline bool scanning = false;
inline bool HookStarted = false;
inline bool force = false;
inline int interval = 1;                    // ms between handleData dispatches (slider)
inline int workers = 5;                     // thread pool size (slider)
inline std::set<std::string> ClassSet;      // dedupe set
inline std::vector<std::string> ClassList = {"None"};  // UI dropdown catalog
inline std::string espclass = "null";       // selected ESP class
inline std::vector<UnityResolve::UnityType::Transform*> EspObjects;
inline std::mutex dataMutex;

inline UnityResolve::Method* m_get_transform = nullptr;
inline UnityResolve::Method* m_destroy1 = nullptr;
inline UnityResolve::Method* m_alive = nullptr;
inline UnityResolve::Method* m_get_name = nullptr;

static void ResolveMethods() {
    auto core = UnityResolve::Get("UnityEngine.CoreModule.dll");
    if (!core) return;
    if (!m_get_transform) {
        auto comp = core->Get("Component");
        m_get_transform = comp ? comp->Get<UnityResolve::Method>("get_transform") : nullptr;
    }
    if (!m_destroy1) {
        auto obj = core->Get("Object");
        m_destroy1 = obj ? obj->Get<UnityResolve::Method>("Destroy", {"*"}) : nullptr;
    }
    if (!m_alive) {
        auto obj = core->Get("Object");
        m_alive = obj ? obj->Get<UnityResolve::Method>("IsNativeObjectAlive", {"*"}) : nullptr;
    }
    if (!m_get_name) {
        auto obj = core->Get("Object");
        m_get_name = obj ? obj->Get<UnityResolve::Method>("get_name") : nullptr;
    }
}

// --- Object.Destroy hook: erase destroyed GameObject's transform ---
using Destroy_t = void (*)(void*, void*);
inline Destroy_t o_Destroy = nullptr;

inline void Remove(UnityResolve::UnityType::GameObject* thiz, void* a1) {
    if (thiz != nullptr && m_get_name) {
        std::lock_guard<std::mutex> lock(dataMutex);
        auto* nameStr = m_get_name->Invoke<UnityResolve::UnityType::String*>(thiz, m_get_name->address);
        if (nameStr && nameStr->ToString() == "GameObject") {
            using GetT_t = UnityResolve::UnityType::Transform* (*)(...);
            auto fn = (GetT_t)(m_get_transform ? m_get_transform->function : nullptr);
            if (fn) {
                auto* t = fn(thiz);
                auto it = std::find(EspObjects.begin(), EspObjects.end(), t);
                if (it != EspObjects.end()) EspObjects.erase(it);
            }
        }
    }
    if (o_Destroy) o_Destroy(thiz, a1);
}

// --- Component.get_transform hook: passive harvest (the original trick) ---
using GetTransform_t = UnityResolve::UnityType::Transform* (*)(void*);
inline GetTransform_t Scanner_Get_Object = nullptr;

struct ScanJob {
    void* component;
};

inline void handleData(void* thiz) {
    // read klass straight out of the object header, like *(Class**)thiz
    auto* obj = static_cast<UnityResolve::UnityType::Object*>(thiz);
    auto* clz = UnityResolve::GetObjectClass(thiz);
    if (!clz) return;
    const char* objectName = clz->name.c_str();

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (scanning) {
            bool add = false;
            if (ClassSet.count(objectName) == 0) {
                // original: keep only classes whose base chain contains "Behaviour"
                add = clz->parent.find("Behaviour") != std::string::npos;
            }
            if (add && ClassSet.count(objectName) == 0) {
                ClassSet.insert(objectName);
                ClassList.push_back(objectName);
            }
        }

        if (!m_get_transform || !m_get_transform->function) return;
        using GetT_t = UnityResolve::UnityType::Transform* (*)(...);
        auto fn = (GetT_t)m_get_transform->function;
        auto* transform = fn(obj);

        if (espclass != "null" && strcmp(objectName, espclass.c_str()) == 0) {
            if (std::find(EspObjects.begin(), EspObjects.end(), transform) == EspObjects.end())
                EspObjects.push_back(transform);
        }
    }
}

// rate-limited hook body (mirrors Scanner() in ObjectScanner.h)
inline UnityResolve::UnityType::Transform* Scanner(void* thiz) {
    auto now = std::chrono::steady_clock::now();
    static std::chrono::steady_clock::time_point lastScanTime;
    if (now - lastScanTime >= std::chrono::milliseconds(interval)) {
        lastScanTime = now;
        // dispatch heavy work off the game thread, like the original's ThreadPool
        struct Ctx { void* obj; };
        auto* ctx = new Ctx{thiz};
        pthread_t tid;
        pthread_create(&tid, nullptr, [](void* p) -> void* {
            auto* c = static_cast<Ctx*>(p);
            handleData(c->obj);
            delete c;
            return nullptr;
        }, ctx);
        pthread_detach(tid);
    }
    // always tail-call the original so the game never stalls
    return Scanner_Get_Object(thiz);
}

inline void scan_thread(void*) {
    ResolveMethods();
    if (!m_get_transform || !m_get_transform->function) return;
    // hook Component.get_transform via Dobby (Method::replace), and
    // Object.Destroy → Remove — same pair as the reference project.
    auto orig = m_get_transform->replace(&Scanner);
    if (orig) Scanner_Get_Object = (GetTransform_t)orig;
    if (m_destroy1 && m_destroy1->function) {
        auto dOrig = m_destroy1->replace(&Remove);
        if (dOrig) o_Destroy = (Destroy_t)dOrig;
    }
}

inline void startScan() {
    static pthread_t tid;
    pthread_create(&tid, nullptr, [](void* p) -> void* { scan_thread(p); return nullptr; }, nullptr);
    pthread_detach(tid);
    scanning = true;
    ClassList.clear();
    ClassList.push_back("None");
    ClassSet.clear();
}

} // namespace OBJECT_SCANNER

namespace ESP_TESTER {

struct EspConfig {
    bool isEsp = false;
    bool line = false;
    bool box = false;
    bool name = false;
};
inline EspConfig config;

inline float EspAlive[4] = {255.f, 255.f, 255.f, 255.f};  // single white like original

inline int32_t ToColor(float* col) {
    return ImGui::ColorConvertFloat4ToU32(*(ImVec4*)(col));
}

// === DrawESP: identical math to UI/EspTester.h ===
inline int GetSceneCount() {
    // UnityEngine.SceneManagement.SceneManager.get_sceneCount
    static UnityResolve::Method* m;
    if (!m) {
        auto core = UnityResolve::Get("UnityEngine.CoreModule.dll");
        auto sm = core ? core->Get("SceneManager") : nullptr;
        m = sm ? sm->Get<UnityResolve::Method>("get_sceneCount") : nullptr;
    }
    if (m && m->function) return m->Invoke<int>();
    return 0;
}

inline void DrawESP(ImDrawList* draw, int glWidth, int glHeight) {
    // scene-load guard: wipe everything when more than one scene is loaded
    if (!OBJECT_SCANNER::force && GetSceneCount() > 1) {
        std::lock_guard<std::mutex> lock(OBJECT_SCANNER::dataMutex);
        OBJECT_SCANNER::EspObjects.clear();
        OBJECT_SCANNER::ClassList.clear();
        OBJECT_SCANNER::ClassSet.clear();
        OBJECT_SCANNER::espclass = "null";
        config.isEsp = false;
        draw->AddText({glWidth / 2.f, glHeight / 1.2f}, ToColor(EspAlive),
                      "Kobtols [Message] : Loading...");
        return;
    }

    std::lock_guard<std::mutex> lock(OBJECT_SCANNER::dataMutex);
    if (OBJECT_SCANNER::EspObjects.empty() || !config.isEsp) return;

    auto* camera = UnityResolve::UnityType::Camera::GetMain();
    if (!camera) {
        OBJECT_SCANNER::EspObjects.clear();
        return;
    }

    for (size_t i = 0; i < OBJECT_SCANNER::EspObjects.size(); i++) {
        auto* enemy = OBJECT_SCANNER::EspObjects[i];
        try {
            if (!enemy) continue;
            if (OBJECT_SCANNER::m_alive && OBJECT_SCANNER::m_alive->function &&
                !OBJECT_SCANNER::m_alive->Invoke<bool>(enemy, OBJECT_SCANNER::m_alive->address))
                continue;

            auto pos = enemy->GetPosition();
            if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) continue;

            auto RealPos = camera->WorldToScreenPoint(pos);
            if (RealPos.z < 1.f) continue;

            UnityResolve::UnityType::Vector3 Origin = pos;
            Origin.y += 1.7f;
            auto NewPos = camera->WorldToScreenPoint(Origin);

            float Hight = fabsf(NewPos.y - RealPos.y);
            float Width = Hight * 0.6f;

            float X = NewPos.x;                 // (obfuscated X in source was identity)
            float Ytop = (float)glHeight - NewPos.y - 8.f;

            if (config.line) {
                draw->AddLine({(float)glWidth / 2, 20}, {X, Ytop}, ToColor(EspAlive), 1.f);
            }
            if (config.box) {
                ImVec2 vStart = {X - Width / 2, Ytop};
                ImVec2 vEnd = {vStart.x + Width, vStart.y + Hight};
                draw->AddRect(vStart, vEnd, ToColor(EspAlive), 1.4f);
            }
            if (config.name && OBJECT_SCANNER::m_get_name && OBJECT_SCANNER::m_get_name->function) {
                auto* nameStr = OBJECT_SCANNER::m_get_name->Invoke<UnityResolve::UnityType::String*>(
                    enemy, OBJECT_SCANNER::m_get_name->address);
                std::string t = "";
                if (nameStr) t += nameStr->ToString();
                draw->AddText({X, (float)glHeight - NewPos.y - 12.f}, ToColor(EspAlive), t.c_str());
            }
        } catch (...) {
        }
    }
}

// === UI: same layout as EspTester.UI ("Classes" tab) ===
inline void UI() {
    if (ImGui::BeginTabBar("Controls")) {
        if (ImGui::BeginTabItem("Classes")) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

            if (!OBJECT_SCANNER::HookStarted) {
                if (ImGui::Button("Scan")) {
                    OBJECT_SCANNER::startScan();
                }
            } else {
                ImGui::Text("Scanning (Transform)....");
            }
            ImGui::Checkbox("Force Scan", &OBJECT_SCANNER::force);
            ImGui::SliderInt("Interval/Ms", &OBJECT_SCANNER::interval, 0, 100);
            ImGui::Text("Increase Interval if its crashing on Loading/Lobby");

            std::lock_guard<std::mutex> lock(OBJECT_SCANNER::dataMutex);
            if (ImGui::BeginCombo("Classes", OBJECT_SCANNER::espclass != "null" ? OBJECT_SCANNER::espclass.c_str() : "-- Select --")) {
                for (auto& cls : OBJECT_SCANNER::ClassList) {
                    bool selected = cls == OBJECT_SCANNER::espclass;
                    if (ImGui::Selectable(cls.c_str(), selected)) {
                        OBJECT_SCANNER::EspObjects.clear();
                        OBJECT_SCANNER::espclass = cls;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            char buf[64];
            ImGui::Text("Objects: %d", (int)OBJECT_SCANNER::EspObjects.size());
            ImGui::Checkbox("Enable Esp", &config.isEsp);
            ImGui::Checkbox("Esp Line", &config.line);
            ImGui::Checkbox("Esp Box", &config.box);
            ImGui::Checkbox("Esp Name", &config.name);
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace ESP_TESTER

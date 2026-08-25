#pragma once
// ESP overlay: line/box/name/distance over target-class objects.
// Uses only UnityResolve.hpp reflection + Tool infra (Keyboard, Config).
// Ported from "Unity Controller v2" ESP (hook-free variant).

#include <Includes.hpp>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>

namespace ESP {

struct Config {
    bool enabled = false;
    bool box = true;
    bool line = false;
    bool name = true;
    bool distance = true;
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    std::string targetClass;
};

inline Config config;

inline std::vector<UnityResolve::UnityType::Transform*>& Targets() {
    static std::vector<UnityResolve::UnityType::Transform*> t;
    return t;
}
inline std::mutex& TargetsMtx() {
    static std::mutex m;
    return m;
}

inline void* GetTransformMethod() {
    static UnityResolve::Method* m;
    if (!m) m = UnityResolve::Get("UnityEngine.CoreModule.dll")->Get("Component")
                    ->Get<UnityResolve::Method>("get_transform");
    return m ? m->function : nullptr;
}

// Poll FindObjectsOfType on a worker thread; replace target list atomically.
inline void* ScanThread(void*) {
    while (config.enabled) {
        if (!config.targetClass.empty()) {
            auto objs = UnityResolve::FindObjectsOfType(config.targetClass);
            std::vector<UnityResolve::UnityType::Transform*> next;
            next.reserve(objs.size());
            auto fn = (UnityResolve::UnityType::Transform* (*)(...))GetTransformMethod();
            for (auto* o : objs) {
                if (!o || !fn) continue;
                auto* t = fn(o);
                if (t) { next.push_back(t); UnityResolve::GC::KeepAlive(o); }
            }
            std::lock_guard<std::mutex> lock(TargetsMtx());
            Targets().swap(next);
        }
        sleep(1);
    }
    std::lock_guard<std::mutex> lock(TargetsMtx());
    Targets().clear();
    return nullptr;
}

inline void StartScan() {
    static pthread_t tid;
    pthread_create(&tid, nullptr, ScanThread, nullptr);
    pthread_detach(tid);
}

// Render-thread draw. Called from _eglSwapBuffers after NewFrame.
inline void Draw() {
    if (!config.enabled) return;

    auto* camera = UnityResolve::UnityType::Camera::GetMain();
    if (!camera) return;

    const float W = (float)glWidth, H = (float)glHeight;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 col = ImGui::ColorConvertFloat4ToU32(*(ImVec4*)config.color);

    // camera world position for distance
    Vector3 camPos{};
    if (auto* camT = camera->GetTransform())
        camPos = camT->GetPosition();

    std::lock_guard<std::mutex> lock(TargetsMtx());
    for (auto* t : Targets()) {
        if (!t) continue;
        Vector3 pos = t->GetPosition();               // feet
        if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) continue;

        Vector3 feetS = camera->WorldToScreenPoint(pos);
        if (feetS.z < 1.f) continue;                  // behind camera

        Vector3 headPos = pos; headPos.y += 1.7f;     // head anchor
        Vector3 headS = camera->WorldToScreenPoint(headPos);

        float xF = feetS.x;
        float yFeet = H - feetS.y;
        float yHead = H - headS.y;
        float boxH = yFeet - yHead;
        if (boxH <= 0.f) continue;
        float boxW = boxH * 0.6f;

        float dist = sqrtf(Vector3{pos.x - camPos.x, pos.y - camPos.y, pos.z - camPos.z}.Length());

        char label[128]{};
        if (config.name && config.distance) {
            auto* n = t->GetName();
            std::string nm = n ? n->ToString() : "?";
            snprintf(label, sizeof(label), "%s [%.0fm]", nm.c_str(), dist);
        } else if (config.distance) {
            snprintf(label, sizeof(label), "%.0fm", dist);
        } else if (config.name) {
            auto* n = t->GetName();
            snprintf(label, sizeof(label), "%s", n ? n->ToString().c_str() : "?");
        }

        if (config.box)
            dl->AddRect({xF - boxW / 2, yHead}, {xF - boxW / 2 + boxW, yFeet}, col, 1.4f);
        if (config.line)
            dl->AddLine({W / 2, 20}, {xF, yHead}, col, 1.f);
        if (label[0])
            dl->AddText({xF, yHead - 16.f}, col, label);
    }
}

// Settings UI block for the menu tab.
inline void SettingsUI() {
    ImGui::Checkbox("ESP Enabled", &config.enabled);
    if (!config.enabled) return;

    static bool scanStarted = false;
    if (!scanStarted) { StartScan(); scanStarted = true; }

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", config.targetClass.c_str());
    ImGui::InputTextWithHint("Target class##esp", "e.g. Coin (il2cpp class name)", buf, sizeof(buf));
    if (ImGui::IsItemDeactivatedAfterEdit())
        config.targetClass = buf;

    ImGui::Checkbox("Box", &config.box);
    ImGui::Checkbox("Snapline", &config.line);
    ImGui::Checkbox("Name", &config.name);
    ImGui::Checkbox("Distance", &config.distance);
    ImGui::ColorEdit4("Color##esp", config.color, ImGuiColorEditFlags_NoInputs);

    std::lock_guard<std::mutex> lock(TargetsMtx());
    ImGui::Text("Tracking %zu objects", Targets().size());
}

} // namespace ESP

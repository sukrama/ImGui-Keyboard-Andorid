#include "ClassesTab.hpp"
#include "Frida.hpp"
#include "Patcher.hpp"
#include "Keyboard.hpp"
#include "Util.hpp"
#include "UnityDump.hpp"
#include "Tool.hpp"
#include "il2cpp/log.h"
#include "il2cpp_dump.h"
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <future>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>

constexpr int MAX_CLASSES = 500;

std::unordered_map<ClassesTab::Class, std::vector<ClassesTab::Object>> ClassesTab::objectMap;
std::unordered_map<ClassesTab::Class, std::vector<ClassesTab::Object>> ClassesTab::newObjectMap;
std::unordered_map<ClassesTab::Class, std::set<ClassesTab::Object>> ClassesTab::savedSet;
std::unordered_map<void*, ClassesTab::OriginalMethodBytes> ClassesTab::oMap;
std::unordered_map<ClassesTab::Class, bool> ClassesTab::states;
PopUpSelector ClassesTab::poper;

static std::unordered_map<void*, HookerData> s_hookerMap;
static std::list<ClassesTab> s_tabs;

static std::vector<void*> getAllImages() {
    std::vector<void*> images;
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image) return images;
    auto domain = il2cpp_domain_get();
    size_t size = 0;
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies) return images;
    // Leaked intentionally: il2cpp_free() aborts under Android Tagged-Pointer ABI (MTE) on A14+.
    for (size_t i = 0; i < size; i++) {
        if (!assemblies[i]) continue;
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (image) images.push_back(image);
    }
    if (il2cpp_image_get_name) {
        std::sort(images.begin(), images.end(), [](void* a, void* b) {
            return strcmp(il2cpp_image_get_name(a), il2cpp_image_get_name(b)) < 0;
        });
    }
    return images;
}

static std::vector<UnityResolve::Class*> getClassesFromImage(void* image) {
    std::vector<UnityResolve::Class*> result;
    if (!image || !il2cpp_image_get_class_count || !il2cpp_image_get_class) return result;
    auto count = il2cpp_image_get_class_count(image);
    for (size_t i = 0; i < count; i++) {
        auto klass = il2cpp_image_get_class(image, i);
        if (klass) result.push_back(UnityResolve::GetOrCreateClass(klass));
    }
    return result;
}

static const char* imageName(void* image) {
    if (!image || !il2cpp_image_get_name) return "?";
    return il2cpp_image_get_name(image);
}

static void* findFieldAddress(UnityResolve::Class* klass, const std::string& name) {
    if (!klass || !klass->address) return nullptr;
    void* current = klass->address;
    while (current) {
        void* iter = nullptr;
        while (auto field = il2cpp_class_get_fields(current, &iter)) {
            const char* fieldName = il2cpp_field_get_name ? il2cpp_field_get_name(field) : nullptr;
            if (fieldName && name == fieldName) return field;
        }
        current = il2cpp_class_get_parent ? il2cpp_class_get_parent(current) : nullptr;
    }
    return nullptr;
}

static void ensureIfValueType(void* currentObj, const std::vector<std::string>& paths, void* rootObj) {
    if (!currentObj || currentObj == rootObj || paths.empty()) return;
    if (!il2cpp_object_get_class || !il2cpp_class_is_valuetype || !il2cpp_object_unbox || !il2cpp_field_set_value) return;
    auto klass = il2cpp_object_get_class(currentObj);
    if (!klass || !il2cpp_class_is_valuetype(klass)) return;

    if (paths.size() > 1) {
        std::vector<std::string> parentPaths(paths.begin(), paths.end() - 1);
        auto parentResult = UnityDump::dumpObject(rootObj, parentPaths);
        if (parentResult.obj && parentResult.klass) {
            auto fieldAddr = findFieldAddress(parentResult.klass, paths.back());
            if (fieldAddr) {
                auto unboxed = il2cpp_object_unbox(currentObj);
                if (unboxed)
                    il2cpp_field_set_value(parentResult.obj, fieldAddr, unboxed);
            }
        }
        ensureIfValueType(parentResult.obj, parentPaths, rootObj);
    } else {
        auto rootKlass = il2cpp_object_get_class(rootObj);
        if (rootKlass) {
            auto rootClass = UnityResolve::GetOrCreateClass(rootKlass);
            if (rootClass) {
                auto fieldAddr = findFieldAddress(rootClass, paths.back());
                if (fieldAddr) {
                    auto unboxed = il2cpp_object_unbox(currentObj);
                    if (unboxed)
                        il2cpp_field_set_value(rootObj, fieldAddr, unboxed);
                }
            }
        }
    }
}

static int getEnumValue(UnityResolve::Type* type, const std::string& fieldName) {
    auto klass = type->getClass();
    if (!klass) return 0;
    auto field = klass->Get<UnityResolve::Field>(fieldName);
    if (!field) return 0;
    int value = 0;
    field->GetStaticValue(&value);
    return value;
}

void Tool::ConfigSave() {
    nlohmann::ordered_json j = nlohmann::ordered_json::array();
    for (auto& tab : s_tabs) {
        nlohmann::ordered_json tj;
        to_json(tj, tab);
        j.push_back(tj);
    }
    Util::FileWriter file("class_tabs.json");
    file.write(j.dump(2, ' ').c_str());
}

bool Tool::ToggleHooker(UnityResolve::Method* method, int state) {
    if (!method || !method->function) return false;
    auto ptr = method->function;
    auto oIt = ClassesTab::oMap.find(ptr);
    if (oIt != ClassesTab::oMap.end() && !oIt->second.bytes.empty()) return false;
    bool hooked;
    {
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        hooked = s_hookerMap.find(ptr) != s_hookerMap.end();
    }
    if (state == -1) {
        if (hooked) {
            if (Frida::Untrace(method)) {
                std::lock_guard<std::mutex> lock(HookerData::traceMtx);
                s_hookerMap.erase(ptr);
            }
            return false;
        }
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        auto& data = s_hookerMap[ptr];
        data.method = method;
        if (!Frida::Trace(method, &data)) {
            s_hookerMap.erase(ptr);
            return false;
        }
        return true;
    }
    if (state == 0) {
        if (hooked) {
            if (Frida::Untrace(method)) {
                std::lock_guard<std::mutex> lock(HookerData::traceMtx);
                s_hookerMap.erase(ptr);
            }
        }
        return false;
    }
    if (!hooked) {
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        auto& data = s_hookerMap[ptr];
        data.method = method;
        if (!Frida::Trace(method, &data)) {
            s_hookerMap.erase(ptr);
            return false;
        }
        return true;
    }
    return true;
}

ClassesTab& Tool::GetFirstTab() {
    if (s_tabs.empty()) s_tabs.emplace_back();
    return s_tabs.front();
}

ClassesTab& Tool::OpenNewTab() {
    void* img = nullptr;
    int imgIdx = 0;
    for (auto& c : s_tabs) {
        if (c.currentlyOpened) {
            img = c.selectedImage;
            imgIdx = c.selectedImageIndex;
            break;
        }
    }
    s_tabs.emplace_back();
    auto& tab = s_tabs.back();
    tab.setOpenedTab = true;
    if (img) {
        tab.selectedImage = img;
        tab.selectedImageIndex = imgIdx;
    }
    return tab;
}

ClassesTab& Tool::OpenNewTabFromClass(UnityResolve::Class* klass) {
    auto& tab = OpenNewTab();
    if (klass && klass->address) {
        tab.filter = klass->getFullName();
        if (il2cpp_class_get_image) {
            auto img = il2cpp_class_get_image(klass->address);
            if (img) {
                tab.selectedImage = img;
                for (size_t i = 0; i < tab.allImages.size(); i++) {
                    if (tab.allImages[i] == img) {
                        tab.selectedImageIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
        tab.FilterClasses(tab.filter);
    }
    return tab;
}

void Tool::ConfigLoad() {
    try {
        Util::FileReader configFile("class_tabs.json");
        nlohmann::ordered_json j = nlohmann::ordered_json::parse(configFile.read());
        s_tabs.clear();
        for (auto& tabJ : j) {
            ClassesTab tab;
            from_json(tabJ, tab);
            s_tabs.push_back(std::move(tab));
        }
    } catch (nlohmann::json::exception& e) {
        LOGE("Failed to load class_tabs.json: %s", e.what());
        ConfigSave();
    }
}

void Tool::ConfigInit() {
    Util::FileReader config("class_tabs.json");
    if (config.exists()) {
        ConfigLoad();
    } else {
        ConfigSave();
    }
}

void Tool::Init() {
    ConfigInit();
    if (s_tabs.empty()) {
        OpenNewTab();
    }
    for (auto& tab : s_tabs) {
        tab.FilterClasses(tab.filter);
    }
    Frida::Init();

    {
        std::lock_guard<std::mutex> lock(UnityResolve::g_MethodsMtx);
        UnityResolve::g_Methods.clear();
        for (auto* assembly : UnityResolve::assembly) {
            if (!assembly || !assembly->address) continue;
            auto image = il2cpp_assembly_get_image(assembly->address);
            if (!image) continue;
            auto count = il2cpp_image_get_class_count(image);
            for (decltype(count) i = 0; i < count; i++) {
                auto klass = il2cpp_image_get_class(image, i);
                if (!klass) continue;
                auto* cls = UnityResolve::GetOrCreateClass(klass);
                if (!cls || !cls->address) continue;
                auto methods = cls->getMethods("", false);
                for (auto* m : methods) {
                    if (m && m->function)
                        UnityResolve::g_Methods.push_back(m);
                }
            }
        }
        std::sort(UnityResolve::g_Methods.begin(), UnityResolve::g_Methods.end(),
            [](UnityResolve::Method* a, UnityResolve::Method* b) {
                return a->function < b->function;
            });
    }
    LOGI("g_Methods populated: %zu methods", UnityResolve::g_Methods.size());
}

void Tool::Draw() {
    [[maybe_unused]] static auto _ = [] { CalculateSomething(); return true; }();
    if (ImGui::BeginTabBar("tabber", ImGuiTabBarFlags_AutoSelectNewTabs |
                         ImGuiTabBarFlags_FittingPolicyScroll |
                         ImGuiTabBarFlags_TabListPopupButton)) {
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_NoTooltip | ImGuiTabItemFlags_Leading)) {
            auto& tab = OpenNewTab();
            tab.FilterClasses(tab.filter);
        }
        int i = 0;
        auto it = s_tabs.begin();
        while (it != s_tabs.end()) {
            if (!it->opened) {
                it = s_tabs.erase(it);
                if (s_tabs.empty()) {
                    auto& tab = OpenNewTab();
                    tab.FilterClasses(tab.filter);
                    break;
                }
                ConfigSave();
            } else {
                ImGui::PushID(i);
                it->Draw(i, true);
                it->DrawTabMap();
                ImGui::PopID();
                ++it;
                i++;
            }
        }
        ImGui::EndTabBar();
    }
}

void Tool::Dumper() {
    static std::string currentDump;
    if (!currentDump.empty()) {
        ImGui::Text("Dumping %s", currentDump.c_str());
    }
    static bool dumping = false;
    if (ImGui::Button("DUMP")) {
        if (dumping) {
            currentDump = "are in progress or finished!";
        }
        dumping = true;
    }
    if (dumping) {
        static char outFile[256];
        static bool outFileInit = false;
        if (!outFileInit) {
            snprintf(outFile, sizeof(outFile), "%s/%s_%s.cs",
                     UnityResolve::getDataPath().c_str(),
                     UnityResolve::getPackageName().c_str(),
                     UnityResolve::getGameVersion().c_str());
            outFileInit = true;
        }
        static bool dumped = false;
        static std::future<void> dumpFuture = std::async(std::launch::async, [] {
            il2cpp_dump(std::string(outFile), [](const char* name, int, int) {
                currentDump = name;
            });
        });
        if (!dumped && dumpFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            currentDump = "Done";
            dumped = true;
        }
        if (dumped) {
            if (ImGui::Button("Copy path")) {
                Keyboard::Open(outFile, nullptr);
            }
        }
    }
}

void Tool::CalculateSomething() {
    constexpr auto* placeholder = "BRUH";
    int max = 10;
    for (int i = 0; i < 100; i++) {
        auto labelSize = ImGui::CalcTextSize(placeholder);
        ImVec2 labellPos{20, 150 + (labelSize.y * i)};
        if (labellPos.y >= ImGui::GetIO().DisplaySize.y) {
            max = i - 5;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        HookerData::visited = RingBuffer<HookerTrace>(max);
    }
}

size_t Tool::GetHookerCount() {
    std::lock_guard<std::mutex> lock(HookerData::traceMtx);
    return s_hookerMap.size();
}

void Tool::DrawTracerTab(bool& changeToToolsTab) {
    ImGui::Text("Traced method count : %zu", Tool::GetHookerCount());
    std::vector<HookerData*> sortedHooker;
    {
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        for (auto& [ptr, data] : s_hookerMap) {
            if (data.hitCount > 0)
                sortedHooker.push_back(&data);
        }
    }

    if (!sortedHooker.empty()) {
        if (ImGui::Button("Quick Restore"))
            ImGui::OpenPopup("QuickRestorePopup");

        std::sort(sortedHooker.begin(), sortedHooker.end(),
                  [](const HookerData* a, const HookerData* b) { return a->hitCount > b->hitCount; });

        ImGui::BeginChild("TracerList", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        auto& tab = GetFirstTab();
        for (auto v : sortedHooker) {
            if (!v->method || !v->method->klass) continue;

            char label[256]{0};
            snprintf(label, sizeof(label), "%s::%s (%dx)###%p",
                     v->method->klass->name.c_str(), v->method->name.c_str(),
                     v->hitCount, v->method);

            ImGui::PushID(v->method);
            bool opened = tab.MethodViewer(v->method->klass, v->method, v->method->args);
            if (!opened && !changeToToolsTab && ImGui::IsItemHeld()) {
                changeToToolsTab = true;
                OpenNewTabFromClass(v->method->klass).setOpenedTab = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        auto& io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(io.DisplaySize.x / 1.2f, 0),
            ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
        if (ImGui::BeginPopup("QuickRestorePopup",
                              ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_MenuBar)) {
            HookerData* toBeErased = nullptr;
            for (auto v : sortedHooker) {
                if (!v->method || !v->method->klass) continue;
                char label[256]{0};
                snprintf(label, sizeof(label), "%s::%s (%dx)###%p",
                         v->method->klass->name.c_str(), v->method->name.c_str(),
                         v->hitCount, v->method);
                if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                    toBeErased = v;
            }
            if (toBeErased)
                ToggleHooker(toBeErased->method, 0);
            ImGui::EndPopup();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::Text("No traced method has been called yet");
        ImGui::PopStyleColor();
    }
}

ClassesTab::MethodList& ClassesTab::buildMethodMap(Class klass) {
    static std::mutex mtx;
    static std::unordered_map<Class, MethodList> cache;
    std::lock_guard guard(mtx);
    auto it = cache.find(klass);
    if (it == cache.end()) {
        MethodList methodList;
        auto methods = klass->getMethods("", false);
        LOGD("Rebuilding %s | %zu methods", klass->name.c_str(), methods.size());
        for (auto method : methods) {
            methodList.push_back({method, method->args});
        }
        LOGD("Rebuilt %zu methods", methodList.size());
        cache[klass] = std::move(methodList);
    }
    return cache[klass];
}

ClassesTab::ClassesTab() {
    allImages = getAllImages();
    if (!allImages.empty()) {
        selectedImage = allImages[0];
        selectedImageIndex = 0;
    }
    if (selectedImage) {
        classes = getClassesFromImage(selectedImage);
        filteredClasses = classes;
    }
}

ClassesTab::Paths& ClassesTab::getJsonPaths(Object object) {
    return dataMap[object].second;
}

void ClassesTab::setJsonObject(Object object) {
    dataMap[object].first = UnityDump::dumpObject(object);
    tabMap.emplace(object, true);
}

ClassesTab::Json& ClassesTab::getJsonObject(Object object) {
    return dataMap[object].first.json;
}

static void renderObjectList(std::vector<ClassesTab::Object>& objects, const char* prefix,
                             std::function<void(ClassesTab::Object)> onSelect, bool showRemove = true) {
    auto& io = ImGui::GetIO();
    if (objects.size() > 100)
        ImGui::Text("Showing 100 of %zu objects", objects.size());
    auto endIt = objects.size() > 100 ? objects.begin() + 100 : objects.end();
    for (auto it = objects.begin(); it != endIt;) {
        auto object = *it;
        char buff[64];
        snprintf(buff, sizeof(buff), "%s [%p]", prefix, object);
        auto size = ImGui::GetWindowSize();
        if (ImGui::Button(buff, ImVec2(size.x / 1.5f, 0)))
            onSelect(object);
        auto objClass = il2cpp_object_get_class ? il2cpp_object_get_class(object) : nullptr;
        auto className = objClass ? (il2cpp_class_get_name ? il2cpp_class_get_name(objClass) : "?") : "?";
        ImGui::SetItemTooltip("%s", className);
        if (showRemove) {
            ImGui::SameLine();
            ImGui::PushID(buff);
            if (ImGui::Button("Remove"))
                it = objects.erase(it);
            else
                ++it;
            ImGui::PopID();
        } else {
            ++it;
        }
    }
}

void ClassesTab::ImGuiObjectSelector(int id, Class klass, const char* prefix,
                                     std::function<void(Object)> onSelect, bool canNew) {
    ImGui::PushID(id);
    static std::unordered_map<void*, bool> scanState;
    auto& scanning = scanState[klass->address];
    if (ImGui::Button("Find Objects")) {
        scanning = true;
        std::thread([klass]() {
            auto found = UnityResolve::GC::FindObjects(klass);
            ClassesTab::objectMap[klass] = std::vector<ClassesTab::Object>(found.begin(), found.end());
            scanState[klass->address] = false;
        }).detach();
    }
    ImGui::PopID();

    auto& io = ImGui::GetIO();
    float width = io.DisplaySize.x;
    float height = io.DisplaySize.y;

    {
        auto& objects = objectMap[klass];
        ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
        ImGui::BeginChild("##ScrollingObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
        ImGui::SeparatorText("Result Object");
        if (objects.empty()) {
            if (scanning) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 255, 50, 255));
                ImGui::Text("Scanning...");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                ImGui::Text("Nothing...");
            }
            ImGui::PopStyleColor();
        } else {
            renderObjectList(objects, prefix, onSelect);
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "Inherited from %s", klass->name.c_str());
        if (ImGui::CollapsingHeader(buffer)) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingInheritedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            bool empty = true;
            for (auto& [setKlass, _] : savedSet) {
                if (klass != setKlass && UnityResolve::IsClassParentOf(setKlass, klass)) {
                    auto& objects = savedSet[setKlass];
                    for (auto it = objects.begin(); it != objects.end();) {
                        empty = false;
                        auto object = *it;
                        char buff[64];
                        snprintf(buff, sizeof(buff), "%s [%p]", setKlass->name.c_str(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5f, 0)))
                            onSelect(object);
                        auto objClass = il2cpp_object_get_class ? il2cpp_object_get_class(object) : nullptr;
                        auto className = objClass ? (il2cpp_class_get_name ? il2cpp_class_get_name(objClass) : "?") : "?";
                        ImGui::SetItemTooltip("%s", className);
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                            it = objects.erase(it);
                        else
                            ++it;
                        ImGui::PopID();
                    }
                }
            }
            if (empty) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                ImGui::Text("Nothing...");
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }
    }

    {
        if (ImGui::CollapsingHeader("Saved Objects")) {
            auto& objects = savedSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingSavedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            if (objects.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                ImGui::Text("Nothing...");
                ImGui::PopStyleColor();
            } else {
                std::vector<Object> objVec(objects.begin(), objects.end());
                for (auto it = objVec.begin(); it != objVec.end();) {
                    auto object = *it;
                    char buff[64];
                    snprintf(buff, sizeof(buff), "%s [%p]", klass->name.c_str(), object);
                    auto size = ImGui::GetWindowSize();
                    if (ImGui::Button(buff, ImVec2(size.x / 1.5f, 0)))
                        onSelect(object);
                    auto objClass = il2cpp_object_get_class ? il2cpp_object_get_class(object) : nullptr;
                    auto className = objClass ? (il2cpp_class_get_name ? il2cpp_class_get_name(objClass) : "?") : "?";
                    ImGui::SetItemTooltip("%s", className);
                    ImGui::SameLine();
                    ImGui::PushID(buff);
                    if (ImGui::Button("Remove")) {
                        objects.erase(object);
                        it = objVec.erase(it);
                    } else {
                        ++it;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
    }

    {
        if (ImGui::CollapsingHeader("Collected Objects")) {
            std::vector<Object> objVec;
            {
                std::lock_guard<std::mutex> lock(HookerData::traceMtx);
                auto& objects = HookerData::collectSet[klass->address];
                objVec = std::vector<Object>(objects.begin(), objects.end());
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingCollectedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            if (objVec.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                ImGui::Text("Nothing...");
                ImGui::PopStyleColor();
            } else {
                for (auto it = objVec.begin(); it != objVec.end();) {
                    auto object = *it;
                    char buff[64];
                    snprintf(buff, sizeof(buff), "%s [%p]", klass->name.c_str(), object);
                    auto size = ImGui::GetWindowSize();
                    if (ImGui::Button(buff, ImVec2(size.x / 1.5f, 0)))
                        onSelect(object);
                    auto objClass = il2cpp_object_get_class ? il2cpp_object_get_class(object) : nullptr;
                    auto className = objClass ? (il2cpp_class_get_name ? il2cpp_class_get_name(objClass) : "?") : "?";
                    ImGui::SetItemTooltip("%s", className);
                    ImGui::SameLine();
                    ImGui::PushID(buff);
                    if (ImGui::Button("Remove")) {
                        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
                        HookerData::collectSet[klass->address].erase(object);
                        it = objVec.erase(it);
                    } else {
                        ++it;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
    }

    {
        if (ImGui::CollapsingHeader("Created Objects")) {
            auto& objects = newObjectMap[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingNewObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            if (objects.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                ImGui::Text("Nothing...");
                ImGui::PopStyleColor();
            } else {
                renderObjectList(objects, prefix, onSelect);
            }
            if (canNew) {
                if (ImGui::Button("New")) {
                    auto newObject = il2cpp_object_new(klass->address);
                    newObjectMap[klass].push_back(newObject);
                    if (UnityResolve::GetClassIsValueType(klass))
                        newObject = UnityResolve::GetUnboxedValue(newObject);
                    onSelect(newObject);
                }
            }
            ImGui::EndChild();
        }
    }
}

void ClassesTab::CallerView(Class klass, Method method, const MethodParamList& paramsInfo, Object thiz) {
    auto& io = ImGui::GetIO();
    bool methodIsStatic = method->static_function;
    auto& params = paramMap[method->address];

    if (!methodIsStatic && !thiz) {
        auto& thisParam = params["this"];
        char thisLabel[128]{};
        snprintf(thisLabel, sizeof(thisLabel), "%s this", klass->name.c_str());
        if (!thisParam.value.empty())
            snprintf(thisLabel, sizeof(thisLabel), "%s = %s", thisLabel, thisParam.value.c_str());
        if (ImGui::Button(thisLabel))
            ImGui::OpenPopup("ThisObjectSelector");
        if (ImGui::BeginPopup("ThisObjectSelector")) {
            ImGuiObjectSelector(ImGui::GetID("ThisObjectSelector"), klass, "this",
                [&thisParam](Object object) {
                    char objStr[16]{};
                    snprintf(objStr, sizeof(objStr), "%p", object);
                    thisParam.value = objStr;
                    thisParam.object = object;
                    ImGui::CloseCurrentPopup();
                },
                method->name == ".ctor");
            ImGui::EndPopup();
        }
    }

    for (size_t k = 0; k < paramsInfo.size(); k++) {
        auto param = paramsInfo[k];
        char paramKey[64]{};
        snprintf(paramKey, sizeof(paramKey), "%p%s%zu", method, param->name.c_str(), k);
        auto& p = params[paramKey];

        char buttonLabel[128]{};
        snprintf(buttonLabel, sizeof(buttonLabel), "%s %s", param->pType->name.c_str(), param->name.c_str());
        if (!p.value.empty())
            snprintf(buttonLabel, sizeof(buttonLabel), "%s = %s", buttonLabel, p.value.c_str());
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::Button(buttonLabel)) {
            bool isString = param->pType->name == "System.String";
            if (param->pType->isPrimitive() || isString) {
                if (param->pType->name == "System.Boolean") {
                    poper.Open("BooleanSelector", [&p](const std::string& result) { p.value = result; });
                } else {
                    Keyboard::Open([&p, isString](const std::string& text) {
                        if (isString)
                            p.object = il2cpp_string_new(text.c_str());
                        p.value = text;
                    });
                }
            } else if (param->pType->isEnum()) {
                poper.Open("EnumSelector", [&p](const std::string& result) { p.value = result; }, param->pType);
            } else if (param->pType->name == "UnityEngine.Vector2") {
                Keyboard::Open(p.value.empty() ? "0 0" : p.value.c_str(),
                    [&p](const std::string& text) { p.value = text; });
            } else if (param->pType->name == "UnityEngine.Vector3") {
                Keyboard::Open(p.value.empty() ? "0 0 0" : p.value.c_str(),
                    [&p](const std::string& text) { p.value = text; });
            } else if (param->pType->name == "UnityEngine.Vector4") {
                Keyboard::Open(p.value.empty() ? "0 0 0 0" : p.value.c_str(),
                    [&p](const std::string& text) { p.value = text; });
            } else {
                ImGui::OpenPopup("ParamObjectSelector");
            }
        }
        if (ImGui::BeginPopup("ParamObjectSelector")) {
            ImGuiObjectSelector(ImGui::GetID("ParamObjectSelector"), param->pType->getClass(), param->name.c_str(),
                [&p](Object object) {
                    char objStr[16]{};
                    snprintf(objStr, sizeof(objStr), "%p", object);
                    p.value = objStr;
                    p.object = object;
                    ImGui::CloseCurrentPopup();
                });
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 200, 25, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 200, 25, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(30, 200, 25, 255));
    if (ImGui::Button("Call", ImVec2(io.DisplaySize.x / 2, 0))) {
        auto arrayParams = paramsInfo.size() > 0 ? new void*[paramsInfo.size()] : nullptr;
        bool hasParams = true;
        void* thisParam = nullptr;

        if (!methodIsStatic && !thiz) {
            if (params["this"].value.empty()) {
                hasParams = false;
            } else {
                thisParam = params["this"].object;
            }
        } else if (thiz) {
            thisParam = thiz;
        }

        for (size_t k = 0; k < paramsInfo.size(); k++) {
            auto param = paramsInfo[k];
            char paramKey[64]{};
            snprintf(paramKey, sizeof(paramKey), "%p%s%zu", method, param->name.c_str(), k);
            auto& p = params[paramKey];
            if (p.value.empty()) {
                hasParams = false;
                continue;
            }
            auto& tn = param->pType->name;
            if (tn == "System.Int32") {
                int32_t v = std::stoi(p.value);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.Int64") {
                int64_t v = std::stoll(p.value);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.UInt32") {
                uint32_t v = static_cast<uint32_t>(std::stoul(p.value));
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.UInt64") {
                uint64_t v = std::stoull(p.value);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.Single") {
                float v = std::stof(p.value);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.Double") {
                double v = std::stod(p.value);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.Boolean") {
                int v = p.value == "True" ? 1 : 0;
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.SByte") {
                int8_t v = static_cast<int8_t>(std::stoi(p.value));
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.Byte") {
                uint8_t v = static_cast<uint8_t>(std::stoul(p.value));
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.Int16") {
                int16_t v = static_cast<int16_t>(std::stoi(p.value));
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.UInt16") {
                uint16_t v = static_cast<uint16_t>(std::stoi(p.value));
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "UnityEngine.Vector2") {
                struct { float x, y; } v{};
                sscanf(p.value.c_str(), "%f %f", &v.x, &v.y);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "UnityEngine.Vector3") {
                struct { float x, y, z; } v{};
                sscanf(p.value.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "UnityEngine.Vector4") {
                struct { float x, y, z, w; } v{};
                sscanf(p.value.c_str(), "%f %f %f %f", &v.x, &v.y, &v.z, &v.w);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (param->pType->isEnum()) {
                int v = getEnumValue(param->pType, p.value);
                arrayParams[k] = UnityResolve::GetBoxedValue(param->pType->getClass(), &v);
            } else if (tn == "System.String") {
                arrayParams[k] = il2cpp_string_new(p.value.c_str());
            } else if (p.object) {
                arrayParams[k] = p.object;
            }
        }

        if (hasParams) {
            void* result = nullptr;
            if (method->name != ".ctor" && thisParam) {
                auto thisClass = UnityResolve::GetObjectClass(thisParam);
                if (thisClass && UnityResolve::GetClassIsValueType(thisClass)) {
                    auto thizz = UnityResolve::GetUnboxedValue(thisParam);
                    result = UnityResolve::RuntimeInvokeConvertArgs(method, thizz, arrayParams, static_cast<int>(paramsInfo.size()));
                } else {
                    result = UnityResolve::RuntimeInvokeConvertArgs(method, thisParam, arrayParams, static_cast<int>(paramsInfo.size()));
                }
            } else {
                result = UnityResolve::RuntimeInvokeConvertArgs(method, thisParam, arrayParams, static_cast<int>(paramsInfo.size()));
            }

            if (result && method->name != ".ctor") {
                auto resultClass = UnityResolve::GetObjectClass(result);
                std::string resultStr;
                void* resultObj = nullptr;
                if (resultClass && UnityResolve::GetClassIsEnum(resultClass)) {
                    auto toString = il2cpp_class_get_method_from_name(resultClass->address, "ToString", 0);
                    if (toString) {
                        void* exc = nullptr;
                        auto str = il2cpp_runtime_invoke(toString, result, nullptr, &exc);
                        if (str) resultStr = UnityDump::readString(str);
                    }
                } else if (resultClass && resultClass->name == "System.String") {
                    resultStr = UnityDump::readString(result);
                } else if (resultClass && UnityResolve::GetClassIsValueType(resultClass)) {
                    UnityResolve::GC::KeepAlive(static_cast<UnityResolve::UnityType::Object*>(result));
                    resultObj = result;
                    auto toString = il2cpp_class_get_method_from_name(resultClass->address, "ToString", 0);
                    if (toString) {
                        void* exc = nullptr;
                        auto thizz = UnityResolve::GetUnboxedValue(result);
                        auto str = il2cpp_runtime_invoke(toString, thizz, nullptr, &exc);
                        if (str) resultStr = UnityDump::readString(str);
                        else resultStr = "the call returned null";
                    }
                } else if (resultClass) {
                    resultObj = result;
                    auto toString = il2cpp_class_get_method_from_name(resultClass->address, "ToString", 0);
                    if (toString) {
                        void* exc = nullptr;
                        auto str = il2cpp_runtime_invoke(toString, result, nullptr, &exc);
                        if (str) resultStr = UnityDump::readString(str);
                        else resultStr = "the call returned null";
                    }
                }
                if (resultStr.empty()) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%p", result);
                    resultStr = buf;
                }
                callResults.at(method->address).push({resultStr, resultObj});
                if (resultClass)
                    savedSet[resultClass].insert(result);
            } else {
                callResults.at(method->address).push({"the call returned null", nullptr});
            }
        }
        delete[] arrayParams;
    }
    ImGui::PopStyleColor(3);

    auto cit = callResults.find(method->address);
    if (cit != callResults.end() && !cit->second.empty()) {
        ImGui::Separator();
        ImGui::Text("Call Results:");
        for (auto& [callResult, object] : cit->second) {
            if (object) {
                if (ImGui::Button(callResult.c_str()))
                    setJsonObject(object);
            } else {
                ImGui::Text("%s", callResult.c_str());
            }
            ImGui::Separator();
        }
    }
}

void ClassesTab::PatcherView(Class klass, Method method, const MethodParamList& paramsInfo, Object thiz) {
    if (isMethodHooked(method)) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't patch while hooked!");
        return;
    }

    auto& o = oMap[method->function];
    auto type = method->return_type;
    if (!type) return;
    auto& tn = type->name;
    bool isNumeric = tn == "System.SByte" || tn == "System.Byte" || tn == "System.Int16" ||
                     tn == "System.Int32" || tn == "System.Int64" || tn == "System.UInt16" ||
                     tn == "System.UInt32" || tn == "System.UInt64" || tn == "System.Single" ||
                     tn == "System.Double" || tn == "System.Boolean" || tn == "System.String";
    bool isEnum = type->isEnum();
    bool isValueType = tn == "UnityEngine.Vector2" || tn == "UnityEngine.Vector3" ||
                       tn == "UnityEngine.Vector4" || tn == "UnityEngine.Quaternion" ||
                       tn == "UnityEngine.Color" || tn == "UnityEngine.Rect" ||
                       tn == "UnityEngine.Plane";

    if (isNumeric || isEnum || isValueType) {
        if (ImGui::Button("Patch return value"))
            ImGui::OpenPopup("HookReturnValuePopup");
        if (!o.text.empty()) {
            ImGui::SameLine();
            ImGui::Text("-> %s", o.text.c_str());
        }
    } else if (tn == "System.Void") {
        if (o.bytes.empty()) {
            if (ImGui::Button("NOP")) {
                Patcher p{method};
                p.nop();
                p.ret();
                o.bytes = p.patch();
                o.text = "NOP";
            }
        } else {
            if (ImGui::Button("Restore")) {
                Patcher p{method};
                p.restore(o.bytes);
                o.bytes.clear();
                o.text.clear();
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
        ImGui::Text("Not supported!");
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginPopup("HookReturnValuePopup")) {
        ImGui::Text("Change return value");
        ImGui::PushID(type);
        char label[64]{};
        if (!o.bytes.empty())
            snprintf(label, sizeof(label), "Restore");
        else
            snprintf(label, sizeof(label), "%s", tn.c_str());
        if (ImGui::Button(label)) {
            if (!o.bytes.empty()) {
                Patcher p{method};
                p.restore(o.bytes);
                o.bytes.clear();
                o.text.clear();
            } else {
                if (tn == "System.Boolean") {
                    poper.Open("BooleanSelector", [method](const std::string& b) {
                        Patcher p{method};
                        p.movBool(b == "True");
                        p.ret();
                        if (oMap[method->function].bytes.empty()) {
                            oMap[method->function].bytes = p.patch();
                            oMap[method->function].text = b;
                        }
                    });
                } else if (isEnum) {
                    poper.Open("EnumSelector", [method, type](const std::string& result) {
                        int value = getEnumValue(type, result);
                        Patcher p{method};
                        p.movInt16(static_cast<int16_t>(value));
                        p.ret();
                        if (oMap[method->function].bytes.empty()) {
                            oMap[method->function].bytes = p.patch();
                            oMap[method->function].text = result;
                        }
                    }, type);
                } else if (tn == "UnityEngine.Vector2") {
                    Keyboard::Open("0 0", [method](const std::string& text) {
                        if (text.empty()) return;
                        struct { float x, y; } v{};
                        sscanf(text.c_str(), "%f %f", &v.x, &v.y);
                        Patcher p{method};
                        p.movVector2(v.x, v.y);
                        p.ret();
                        if (oMap[method->function].bytes.empty()) {
                            oMap[method->function].bytes = p.patch();
                            oMap[method->function].text = text;
                        }
                    });
                } else if (tn == "UnityEngine.Vector3") {
                    Keyboard::Open("0 0 0", [method](const std::string& text) {
                        if (text.empty()) return;
                        struct { float x, y, z; } v{};
                        sscanf(text.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
                        Patcher p{method};
                        p.movVector3(v.x, v.y, v.z);
                        p.ret();
                        if (oMap[method->function].bytes.empty()) {
                            oMap[method->function].bytes = p.patch();
                            oMap[method->function].text = text;
                        }
                    });
                } else if (tn == "UnityEngine.Vector4" || tn == "UnityEngine.Quaternion" ||
                           tn == "UnityEngine.Color" || tn == "UnityEngine.Rect" ||
                           tn == "UnityEngine.Plane") {
                    Keyboard::Open("0 0 0 0", [method](const std::string& text) {
                        if (text.empty()) return;
                        struct { float x, y, z, w; } v{};
                        sscanf(text.c_str(), "%f %f %f %f", &v.x, &v.y, &v.z, &v.w);
                        Patcher p{method};
                        p.movVector4(v.x, v.y, v.z, v.w);
                        p.ret();
                        if (oMap[method->function].bytes.empty()) {
                            oMap[method->function].bytes = p.patch();
                            oMap[method->function].text = text;
                        }
                    });
                } else {
                    auto typ = type;
                    Keyboard::Open([typ, method](const std::string& text) {
                        if (text.empty()) return;
                        auto& tn = typ->name;
                        Patcher p{method};
                        if (tn == "System.SByte") p.movInt8(static_cast<int8_t>(std::stoi(text)));
                        else if (tn == "System.Byte") p.movUInt8(static_cast<uint8_t>(std::stoul(text)));
                        else if (tn == "System.Int16") p.movInt16(static_cast<int16_t>(std::stoi(text)));
                        else if (tn == "System.UInt16") p.movUInt16(static_cast<uint16_t>(std::stoi(text)));
                        else if (tn == "System.Int32") p.movInt32(std::stoi(text));
                        else if (tn == "System.UInt32") p.movUInt32(static_cast<uint32_t>(std::stoul(text)));
                        else if (tn == "System.Int64") p.movInt64(std::stoll(text));
                        else if (tn == "System.UInt64") p.movUInt64(std::stoull(text));
                        else if (tn == "System.Single") p.movFloat(std::stof(text));
                        else if (tn == "System.Double") p.movDouble(std::stod(text));
                        else if (tn == "System.String") p.movPtr(il2cpp_string_new(text.c_str()));
                        p.ret();
                        if (oMap[method->function].bytes.empty()) {
                            oMap[method->function].bytes = p.patch();
                            oMap[method->function].text = text;
                        }
                    });
                }
            }
        }
        ImGui::PopID();
        ImGui::EndPopup();
    }
}

void ClassesTab::HookerView(Class klass, Method method, const MethodParamList& paramsInfo, Object thiz) {
    if (!oMap[method->function].bytes.empty()) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't hook while patched!");
        return;
    }

    int hitCount = 0;
    std::string lastArgs, lastRet;
    bool backtracing = false;
    std::vector<std::vector<std::string>> backtracedSnapshot;
    bool hooked;

    {
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        auto it = s_hookerMap.find(method->function);
        hooked = it != s_hookerMap.end();
        if (hooked) {
            hitCount = it->second.hitCount;
            lastArgs = it->second.lastArgs;
            lastRet = it->second.lastRet;
            backtracing = it->second.backtracing;
            for (auto& vec : it->second.backtraced)
                backtracedSnapshot.push_back(vec);
        }
    }

    char label[16];
    if (!hooked)
        snprintf(label, sizeof(label), "Trace");
    else {
        snprintf(label, sizeof(label), "Restore");
        ImGui::Text("Hit Count %d", hitCount);
        ImGui::Separator();
    }

    if (ImGui::Button(label)) {
        Tool::ToggleHooker(method);
        std::lock_guard<std::mutex> lock(HookerData::traceMtx);
        auto it = s_hookerMap.find(method->function);
        hooked = it != s_hookerMap.end();
        if (hooked) {
            hitCount = it->second.hitCount;
            backtracing = it->second.backtracing;
        }
    }
    ImGui::Separator();

    if (hooked) {
        if (!backtracing) {
            if (ImGui::Button("Backtrace")) {
                std::lock_guard<std::mutex> lock(HookerData::traceMtx);
                auto it = s_hookerMap.find(method->function);
                if (it != s_hookerMap.end())
                    it->second.backtracing = true;
            }
        }

        if (lastArgs.empty() && backtracedSnapshot.empty()) {
            ImGui::Text("Method has not been called");
        } else {
            if (!lastRet.empty()) {
                ImGui::SeparatorText("return value");
                auto rt = method->return_type;
                ImGui::Text("%s %s -> %s",
                    rt ? rt->name.c_str() : "?",
                    method->name.c_str(),
                    lastRet.c_str());
                ImGui::Spacing();
            }
            if (!lastArgs.empty()) {
                ImGui::SeparatorText("args");
                ImGui::TextWrapped("%s;", lastArgs.c_str());
                ImGui::Spacing();
            }
            if (!backtracedSnapshot.empty()) {
                ImGui::SeparatorText("Backtraced methods");
                for (auto& result : backtracedSnapshot) {
                    for (auto& r : result)
                        ImGui::Text("%s", r.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(50, 255, 100, 255));
                    ImGui::Separator();
                    ImGui::PopStyleColor();
                }
            }
        }
    }
}

bool ClassesTab::isMethodHooked(Method method) {
    if (!method || !method->function) return false;
    std::lock_guard<std::mutex> lock(HookerData::traceMtx);
    return s_hookerMap.find(method->function) != s_hookerMap.end();
}

bool ClassesTab::MethodViewer(Class klass, Method method, const MethodParamList& paramsInfo,
                              Object thiz, bool includeInflated) {
    bool zeroPointer = method->function == nullptr;

    if (callResults.find(method->address) == callResults.end())
        callResults.emplace(method->address, RingBuffer<std::pair<std::string, Object>>(5));

    bool methodIsStatic = method->static_function;

    char treeLabel[512]{};
    auto rt = method->return_type;
    snprintf(treeLabel, sizeof(treeLabel), "%s %s(%zu)###",
        rt ? rt->name.c_str() : "?", method->name.c_str(), paramsInfo.size());

    int pushedColor = 0;
    if (methodIsStatic) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 100, 255));
        pushedColor++;
        Util::prependStringToBuffer(treeLabel, "static ");
    }
    bool patched = !oMap[method->function].bytes.empty();
    bool hooked = isMethodHooked(method);
    if (zeroPointer) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        pushedColor++;
    }
    if (patched || hooked) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(25, 255, 125, 255));
        pushedColor++;
        if (hooked) {
            int hitCount = 0;
            {
                std::lock_guard<std::mutex> lock(HookerData::traceMtx);
                auto it = s_hookerMap.find(method->function);
                if (it != s_hookerMap.end()) hitCount = it->second.hitCount;
            }
            char hitLabel[64]{};
            snprintf(hitLabel, sizeof(hitLabel), "Hit Count %d | ", hitCount);
            Util::prependStringToBuffer(treeLabel, hitLabel);
        } else if (patched) {
            auto& text = oMap[method->function].text;
            if (!text.empty()) {
                char buff[64]{};
                snprintf(buff, sizeof(buff), "Returns %s | ", text.c_str());
                Util::prependStringToBuffer(treeLabel, buff);
            }
        }
    }

    bool state = ImGui::TreeNode(treeLabel);
    if (state) {
        if (pushedColor) { ImGui::PopStyleColor(pushedColor); pushedColor = 0; }
        if (ImGui::BeginTabBar("##methoder")) {
            if (ImGui::BeginTabItem("Caller")) {
                CallerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Patcher")) {
                PatcherView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tracer")) {
                HookerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::TreePop();
    }
    poper.Update();
    ImGui::PopStyleColor(pushedColor);
    return state;
}

void ClassesTab::ClassViewer(Class klass) {
    if (ImGui::Button("Inspect Objects"))
        ImGui::OpenPopup("DumpPopup");

    {
        ImGui::SameLine();
        bool& state = states[klass];
        char label[12]{};
        if (!state) snprintf(label, sizeof(label), "Trace all");
        else snprintf(label, sizeof(label), "Restore");
        if (ImGui::Button(label)) {
            if (!state) {
                ImGui::OpenPopup("ConfirmPopup");
            } else {
                state = false;
                for (auto& [method, paramsInfo] : methodMap[klass]) {
                    if (!method->function && !UnityResolve::GetIsMethodInflated(method))
                        continue;
                    Tool::ToggleHooker(method, 0);
                }
            }
        }
        if (ImGui::BeginPopup("ConfirmPopup")) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0, 1), "WARNING: There's a high-risk of crash");
            if (ImGui::Button("Continue?")) {
                state = true;
                std::thread([this, klass]() {
                    for (auto& [method, paramsInfo] : methodMap[klass]) {
                        if (!method->function && !UnityResolve::GetIsMethodInflated(method))
                            continue;
                        Tool::ToggleHooker(method, 1);
                    }
                }).detach();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (ImGui::BeginPopup("DumpPopup")) {
        ImGuiObjectSelector(ImGui::GetID("ObjectSelector"), klass, "Inspect",
            [this](Object object) { setJsonObject(object); });
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Separator();

    int j = 0;
    for (auto& [method, paramsInfo] : methodMap[klass]) {
        ImGui::PushID(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(method) + j++));
        MethodViewer(klass, method, paramsInfo);
        ImGui::Separator();
        ImGui::PopID();
    }
}

void ClassesTab::Draw(int index, bool closeable) {
    auto& io = ImGui::GetIO();
    char tabLabel[256];
    if (filter.empty()) {
        snprintf(tabLabel, sizeof(tabLabel), "Classes");
        if (index >= 0)
            snprintf(tabLabel, sizeof(tabLabel), "Classes [%d]", index + 1);
    } else {
        snprintf(tabLabel, sizeof(tabLabel), "%s", filter.c_str());
    }

    if ((currentlyOpened = ImGui::BeginTabItem(tabLabel, closeable ? &opened : nullptr,
                                               setOpenedTab ? ImGuiTabItemFlags_SetSelected : 0))) {
        setOpenedTab = false;
        ImGui::BeginDisabled(includeAllImages);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.6f, io.DisplaySize.y / 1.5f));
        if (ImGui::BeginCombo("Image##ImageSelector", imageName(selectedImage))) {
            for (int i = 0; i < static_cast<int>(allImages.size()); i++) {
                bool selected = selectedImageIndex == i;
                if (ImGui::Selectable(imageName(allImages[i]), selected)) {
                    selectedImage = allImages[i];
                    selectedImageIndex = i;
                    FilterClasses(filter);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Checkbox("All", &includeAllImages))
            FilterClasses(filter);

        {
            const std::string& liveText = (Keyboard::IsOpen() && filterKeyboardOpen) ? Keyboard::GetCurrentText() : filter;
            static std::string lastLiveText;
            if (Keyboard::IsOpen() && filterKeyboardOpen && liveText != lastLiveText) {
                lastLiveText = liveText;
                FilterClasses(liveText);
            } else if (!Keyboard::IsOpen()) {
                lastLiveText.clear();
                filterKeyboardOpen = false;
            }

            char filterBuffer[256];
            snprintf(filterBuffer, sizeof(filterBuffer), "Filter : %s | %zu of %zu",
                liveText.empty() ? "(none)" : liveText.c_str(),
                filteredClasses.size(), classes.size());
            if (ImGui::Button(filterBuffer, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && !Keyboard::IsOpen()) {
                filterKeyboardOpen = true;
                Keyboard::Open(filter.c_str(), [this](const std::string& text) {
                    filterKeyboardOpen = false;
                    filter = text;
                    FilterClasses(filter);
                });
            }
        }

        if (ImGui::Button("Filter Options"))
            ImGui::OpenPopup("FilterOptions");

        {
            static std::atomic<bool> processing{false};
            ImGui::SameLine();
            char label[12]{};
            if (!traceState) snprintf(label, sizeof(label), "Trace all");
            else snprintf(label, sizeof(label), "Restore");

            bool disabled = false;
            if (processing) {
                disabled = true;
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(label)) {
                if (!traceState) {
                    ImGui::OpenPopup("ConfirmPopup");
                } else {
                    traceState = false;
                    std::thread([this]() {
                        processing = true;
                        maxProgress = static_cast<int>(tracedMethods.size());
                        for (auto method : tracedMethods)
                            Tool::ToggleHooker(method);
                        tracedMethods.clear();
                        processing = false;
                        maxProgress = 0;
                        progress = 0;
                    }).detach();
                }
            }
            if (disabled) ImGui::EndDisabled();

            if (ImGui::BeginPopup("ConfirmPopup")) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0, 1), "WARNING: There's a high-risk of crash");
                if (ImGui::Button("Continue?")) {
                    traceState = true;
                    std::thread([this]() {
                        for (auto& klass : filteredClasses) {
                            for (auto& [method, paramsInfo] : methodMap[klass]) {
                                if (!method->function && !UnityResolve::GetIsMethodInflated(method))
                                    continue;
                                maxProgress++;
                            }
                        }
                        processing = true;
                        for (auto& klass : filteredClasses) {
                            states[klass] = true;
                            for (auto& [method, paramsInfo] : methodMap[klass]) {
                                if (!method->function && !UnityResolve::GetIsMethodInflated(method))
                                    continue;
                                if (Tool::ToggleHooker(method, 1))
                                    tracedMethods.push_back(method);
                                progress++;
                            }
                        }
                        processing = false;
                        maxProgress = 0;
                        progress = 0;
                    }).detach();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (processing) {
                ImGui::SameLine();
                ImGui::Text("Processing %d of %d ...", progress, maxProgress);
            }
        }

        if (ImGui::BeginPopup("FilterOptions")) {
            if (ImGui::Checkbox("Case-Sensitive", &caseSensitive))
                FilterClasses(filter);
            ImGui::Text("Filter by ");
            ImGui::SameLine();
            if (ImGui::RadioButton("Class", filterByClass == true)) {
                filterByClass = true; filterByMethod = false; filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Method", filterByMethod == true)) {
                filterByClass = false; filterByMethod = true; filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Field", filterByField == true)) {
                filterByClass = false; filterByMethod = false; filterByField = true;
                FilterClasses(filter);
            }
            if (ImGui::Checkbox("Show All Classes", &showAllClasses))
                FilterClasses(filter);
            ImGui::EndPopup();
        }

        ImGui::Separator();
        if (!filteredClasses.empty()) {
            ImGui::BeginChild("Child", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            for (size_t i = 0; i < filteredClasses.size(); i++) {
                auto klass = filteredClasses[i];
                bool isValueType = UnityResolve::GetClassIsValueType(klass);
                int pushedColor = 0;
                if (isValueType) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
                    pushedColor++;
                }
                bool collapsingHeader = ImGui::CollapsingHeader(klass->getFullName().c_str());
                if (filterByMethod && ImGui::IsItemHeld(0.7f)) {
                    auto name = Util::extractClassNameFromTypename(klass->getFullName().c_str());
                    auto& tab = Tool::OpenNewTab();
                    tab.filter = name;
                    if (klass->address && il2cpp_class_get_image) {
                        tab.selectedImage = il2cpp_class_get_image(klass->address);
                        for (size_t si = 0; si < tab.allImages.size(); si++) {
                            if (tab.allImages[si] == tab.selectedImage) {
                                tab.selectedImageIndex = static_cast<int>(si);
                                break;
                            }
                        }
                    }
                    tab.FilterClasses(tab.filter);
                }
                if (collapsingHeader) {
                    if (pushedColor) { ImGui::PopStyleColor(pushedColor); pushedColor = 0; }
                    ImGui::PushID(static_cast<int>(i));
                    ClassViewer(klass);
                }
                if (pushedColor) { ImGui::PopStyleColor(pushedColor); pushedColor = 0; }
            }
            ImGui::ScrollWhenDraggingOnVoid();
            ImGui::EndChild();
        }
        ImGui::EndTabItem();
    }
}

void ClassesTab::DrawTabMap() {
    for (auto it = tabMap.begin(); it != tabMap.end();) {
        auto& [object, visible] = *it;
        char buff[32]{};
        snprintf(buff, sizeof(buff), "[%p]", object);
        if (!visible) {
            it = tabMap.erase(it);
            dataMap.erase(object);
        } else {
            if (ImGui::BeginTabItem(buff, &visible)) {
                ImGuiJson(object);
                ImGui::EndTabItem();
            }
            ++it;
        }
    }
}

void ClassesTab::ImGuiJson(Object rootObj) {
    auto& paths = getJsonPaths(rootObj);
    auto& io = ImGui::GetIO();

    int indentCounter = 0;
    auto currentObj = dataMap[rootObj].first.obj;
    auto currentClass = dataMap[rootObj].first.klass;

    for (auto it = paths.begin() + (paths.size() > 3 ? paths.size() - 4 : 0); it != paths.end(); ++it) {
        const bool isLast = std::next(it) == paths.end();
        auto key = it->c_str();
        ImGui::PushID(indentCounter);

        bool buttonPressed = false;
        bool isValueType = currentClass ? UnityResolve::GetClassIsValueType(currentClass) : false;
        if (isLast && isValueType)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
        if (isLast && currentObj && currentClass && savedSet[currentClass].count(currentObj) == 0) {
            auto width = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 5.f;
            buttonPressed = ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - width, 0));
            if (ImGui::IsItemHeld())
                Tool::OpenNewTabFromClass(currentClass);
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
                savedSet[currentClass].insert(currentObj);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%p", currentObj);
        } else {
            buttonPressed = ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0));
            if (ImGui::IsItemHeld())
                Tool::OpenNewTabFromClass(currentClass);
        }
        if (isLast && isValueType)
            ImGui::PopStyleColor();
        ImGui::Indent(10.f);
        indentCounter++;
        ImGui::PopID();

        if (buttonPressed) {
            paths.erase(it, paths.end());
            dataMap[rootObj].first = UnityDump::dumpObject(rootObj, paths);
            break;
        }
    }
    if (indentCounter)
        ImGui::Unindent(10.f * indentCounter);

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 200, 20, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(100, 200, 20, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(100, 200, 20, 255));
    static bool doRefresh = false;
    if (ImGui::Button("Refresh", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
        doRefresh = true;
    if (doRefresh) {
        doRefresh = false;
        dataMap[rootObj].first = UnityDump::dumpObject(rootObj, paths);
    }
    ImGui::PopStyleColor(3);

    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(0, 255, 100, 255));
    ImGui::Separator();
    ImGui::PopStyleColor();

    static PopUpSelector localPoper;
    if (ImGui::BeginTable("sometable", 1)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("ChildJson",
            ImVec2(0, ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 1.8f) * 2.f), 0,
            ImGuiWindowFlags_HorizontalScrollbar);

        const auto& current = getJsonObject(rootObj);
        if (current.empty())
            ImGui::Text("Empty");

        currentObj = dataMap[rootObj].first.obj;
        currentClass = dataMap[rootObj].first.klass;

        for (auto& [key, value] : current.items()) {
            if (value.is_object() || value.is_array()) {
                if (value.is_array() && value.size() == 0) {
                    ImGui::Text("%s = [Empty]", key.c_str());
                } else if (ImGui::Button(key.c_str(), ImVec2(key.length() <= 3 ? ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x : 0, 0))) {
                    try {
                        paths.push_back(key);
                        dataMap[rootObj].first = UnityDump::dumpObject(rootObj, paths);
                        break;
                    } catch (nlohmann::json::exception& e) {
                        LOGE("Json exception %s", e.what());
                    } catch (std::exception& e) {
                        LOGE("Exception %s", e.what());
                    }
                }
            } else if (value.is_string()) {
                auto text = value.get<std::string>();
                ImGui::Text("%s = %s", key.c_str(), text.c_str());
                if (ImGui::IsItemClicked() && currentClass && currentObj) {
                    auto fieldAddr = findFieldAddress(currentClass, key);
                    if (fieldAddr) {
                        auto fieldType = il2cpp_field_get_type(fieldAddr);
                        auto typeKind = fieldType ? il2cpp_type_get_type(fieldType) : 0;
                        if (typeKind == IL2CPP_TYPE_STRING) {
                            Keyboard::Open(text.c_str(), [this, rootObj, currentObj, fieldAddr](const std::string& val) {
                                auto newStr = il2cpp_string_new(val.c_str());
                                il2cpp_field_set_value(currentObj, fieldAddr, &newStr);
                                ensureIfValueType(currentObj, getJsonPaths(rootObj), rootObj);
                                doRefresh = true;
                            });
                        }
                    }
                }
            } else if (value.is_boolean()) {
                ImGui::Text("%s = %s", key.c_str(), value.get<bool>() ? "True" : "False");
                if (ImGui::IsItemClicked() && currentClass && currentObj) {
                    auto fieldAddr = findFieldAddress(currentClass, key);
                    if (fieldAddr) {
                        localPoper.Open("BooleanSelector", [this, rootObj, currentObj, fieldAddr](const std::string& val) {
                            int b = val == "True" ? 1 : 0;
                            il2cpp_field_set_value(currentObj, fieldAddr, &b);
                            ensureIfValueType(currentObj, getJsonPaths(rootObj), rootObj);
                            doRefresh = true;
                        });
                    }
                }
            } else if (value.is_number_float()) {
                ImGui::Text("%s = %f", key.c_str(), value.get<float>());
                if (ImGui::IsItemClicked() && currentClass && currentObj) {
                    auto fieldAddr = findFieldAddress(currentClass, key);
                    if (fieldAddr) {
                        auto fieldType = il2cpp_field_get_type(fieldAddr);
                        auto typeName = fieldType ? il2cpp_type_get_name(fieldType) : nullptr;
                        std::string tn = typeName ? typeName : "";
                        if (typeName) il2cpp_free(typeName);
                        Keyboard::Open(std::to_string(value.get<float>()).c_str(),
                            [this, rootObj, currentObj, fieldAddr, tn](const std::string& text) {
                                if (tn == "System.Single") {
                                    float v = std::stof(text);
                                    il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                } else if (tn == "System.Double") {
                                    double v = std::stod(text);
                                    il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                }
                                ensureIfValueType(currentObj, getJsonPaths(rootObj), rootObj);
                                doRefresh = true;
                            });
                    }
                }
            } else if (value.is_number()) {
                ImGui::Text("%s = %lld", key.c_str(), static_cast<long long>(value.get<int64_t>()));
                if (ImGui::IsItemClicked() && currentClass && currentObj) {
                    auto fieldAddr = findFieldAddress(currentClass, key);
                    if (fieldAddr) {
                        auto fieldType = il2cpp_field_get_type(fieldAddr);
                        auto typeKind = fieldType ? il2cpp_type_get_type(fieldType) : 0;
                        if (typeKind == IL2CPP_TYPE_ENUM) {
                            static UnityResolve::Type enumType;
                            enumType.address = fieldType;
                            localPoper.Open("EnumSelector", [this, rootObj, fieldAddr, fieldType, currentObj](const std::string& result) {
                                auto klass = il2cpp_class_from_type(fieldType);
                                if (!klass) { doRefresh = true; return; }
                                auto enumClass = UnityResolve::GetOrCreateClass(klass);
                                if (!enumClass) { doRefresh = true; return; }
                                auto fields = enumClass->getFields(false);
                                for (auto& f : fields) {
                                    if (f && f->address && f->name == result) {
                                        int val = 0;
                                        il2cpp_field_static_get_value(f->address, &val);
                                        il2cpp_field_set_value(currentObj, fieldAddr, &val);
                                        break;
                                    }
                                }
                                ensureIfValueType(currentObj, getJsonPaths(rootObj), rootObj);
                                doRefresh = true;
                            }, &enumType);
                        } else {
                            auto typeName = fieldType ? il2cpp_type_get_name(fieldType) : nullptr;
                            std::string tn = typeName ? typeName : "";
                            if (typeName) il2cpp_free(typeName);
                            Keyboard::Open(std::to_string(value.get<int64_t>()).c_str(),
                                [this, rootObj, currentObj, fieldAddr, tn](const std::string& text) {
                                    if (tn == "System.Int16") {
                                        int16_t v = static_cast<int16_t>(std::stoi(text));
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.UInt16") {
                                        uint16_t v = static_cast<uint16_t>(std::stoi(text));
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.Int32") {
                                        int32_t v = std::stoi(text);
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.UInt32") {
                                        uint32_t v = static_cast<uint32_t>(std::stoul(text));
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.Int64") {
                                        int64_t v = std::stoll(text);
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.UInt64") {
                                        uint64_t v = std::stoull(text);
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.SByte") {
                                        int8_t v = static_cast<int8_t>(std::stoi(text));
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    } else if (tn == "System.Byte") {
                                        uint8_t v = static_cast<uint8_t>(std::stoul(text));
                                        il2cpp_field_set_value(currentObj, fieldAddr, &v);
                                    }
                                    ensureIfValueType(currentObj, getJsonPaths(rootObj), rootObj);
                                    doRefresh = true;
                                });
                        }
                    }
                }
            } else {
                ImGui::Text("Unk %s %s", key.c_str(), value.type_name());
            }
            ImGui::Separator();
        }

        ImGui::ScrollWhenDraggingOnVoid();
        ImGui::EndChild();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("bottom");
        if (currentClass && currentObj) {
            if (ImGui::Button("Methods", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
                ImGui::OpenPopup("MethodPopup");
            ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x / 1.2f, 0), ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
            if (ImGui::BeginPopup("MethodPopup")) {
                auto text = currentClass->name.c_str();
                auto windowWidth = ImGui::GetWindowSize().x;
                auto textWidth = ImGui::CalcTextSize(text).x;
                ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
                ImGui::Text("%s", text);
                ImGui::Separator();
                auto& methods = buildMethodMap(currentClass);
                if (methods.empty()) {
                    ImGui::Text("No methods for class %s", currentClass->name.c_str());
                } else {
                    int j = 0;
                    for (auto& [method, paramsInfo] : methods) {
                        ImGui::PushID(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(method) + j++));
                        MethodViewer(currentClass, method, paramsInfo, currentObj, true);
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                }
                ImGui::EndPopup();
            }

            if (ImGui::Button("Dump to file", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
                ImGui::OpenPopup("ProceedPopUp");
            if (ImGui::BeginPopup("ProceedPopUp")) {
                char fileName[256]{};
                snprintf(fileName, sizeof(fileName), "dump_%s (%p).json", currentClass->name.c_str(), currentObj);
                ImGui::Text("File will be saved as %s", fileName);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 50, 255));
                ImGui::Text("Note: This may take a while depending on the size of the object");
                ImGui::Text("Do not touch the screen if it's freezing!");
                ImGui::PopStyleColor();
                if (ImGui::Button("Proceed", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    UnityDump::SetMaxArraySize(9999);
                    try {
                        auto dump = UnityDump::dumpObject(currentObj);
                        Util::FileWriter file(fileName);
                        file.write(dump.json.dump(2, ' ').c_str());
                        ImGui::CloseCurrentPopup();
                    } catch (std::exception& e) {
                        LOGE("Dump to file failed: %s", e.what());
                    }
                    UnityDump::SetMaxArraySize(50);
                }
                ImGui::EndPopup();
            }
        }

        localPoper.Update();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void ClassesTab::FilterClasses(const std::string& filt) {
    filteredClasses.clear();
    classes.clear();
    methodMap.clear();

    if (includeAllImages) {
        for (auto image : allImages) {
            auto imageClasses = getClassesFromImage(image);
            classes.insert(classes.end(), imageClasses.begin(), imageClasses.end());
        }
    } else {
        classes = getClassesFromImage(selectedImage);
    }

    auto finderCaseSensitive = [](const std::string& a, const std::string& b) {
        return a.find(b) != std::string::npos;
    };
    auto finderCaseInsensitive = [](const std::string& a, const std::string& b) {
        auto newA = a;
        auto newB = b;
        std::transform(newA.begin(), newA.end(), newA.begin(), ::tolower);
        std::transform(newB.begin(), newB.end(), newB.begin(), ::tolower);
        return newA.find(newB) != std::string::npos;
    };
    auto finder = caseSensitive ? finderCaseSensitive : finderCaseInsensitive;

    size_t limit = showAllClasses ? classes.size() : MAX_CLASSES;
    for (size_t i = 0; i < classes.size() && filteredClasses.size() < limit; i++) {
        auto klass = classes[i];
        if (UnityResolve::GetClassIsEnum(klass))
            continue;

        if (filterByClass) {
            if (finder(klass->getFullName(), filt)) {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods("", false))
                    methodMap[klass].push_back({m, m->args});
            }
        } else if (filterByMethod) {
            bool found = false;
            for (auto m : klass->getMethods("", false)) {
                if (finder(m->name, filt)) {
                    found = true;
                    methodMap[klass].push_back({m, m->args});
                }
            }
            if (found)
                filteredClasses.push_back(klass);
        } else if (filterByField) {
            bool found = false;
            for (auto f : klass->getFields(false)) {
                if (finder(f->name, filt))
                    found = true;
            }
            if (found) {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods("", false))
                    methodMap[klass].push_back({m, m->args});
            }
        }
    }
    Tool::ConfigSave();
}

void to_json(nlohmann::ordered_json& j, const ClassesTab& p) {
    j["filter"] = p.filter;
    j["filterByClass"] = p.filterByClass;
    j["filterByField"] = p.filterByField;
    j["filterByMethod"] = p.filterByMethod;
    j["showAllClasses"] = p.showAllClasses;
    j["includeAllImages"] = p.includeAllImages;
    j["caseSensitive"] = p.caseSensitive;
    j["selectedImage"] = imageName(p.selectedImage);
}

void from_json(const nlohmann::ordered_json& j, ClassesTab& p) {
    j.at("filter").get_to(p.filter);
    j.at("filterByClass").get_to(p.filterByClass);
    j.at("filterByField").get_to(p.filterByField);
    j.at("filterByMethod").get_to(p.filterByMethod);
    j.at("showAllClasses").get_to(p.showAllClasses);
    j.at("includeAllImages").get_to(p.includeAllImages);
    j.at("caseSensitive").get_to(p.caseSensitive);
    std::string name = j.at("selectedImage").get<std::string>();
    if (name.length() >= 4 && name.compare(name.length() - 4, 4, ".dll") == 0)
        name.erase(name.length() - 4);
    auto images = getAllImages();
    for (size_t i = 0; i < images.size(); i++) {
        if (imageName(images[i]) == name) {
            p.selectedImage = images[i];
            p.selectedImageIndex = static_cast<int>(i);
            break;
        }
    }
}

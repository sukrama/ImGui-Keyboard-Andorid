#include <Includes.hpp>
#include <Tool/ESP.hpp>

void DrawMenu() {
    const char* title = "Kobtols";
    static ImVec2 lastSize = ImVec2(0, 0);
    static ImVec2 lastPos = ImVec2(0, 0);

    if (resetWindow) {
        resetWindow = false;
        if (fullScreen) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            auto screenSize = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(screenSize);
        } else {
            ImGui::SetNextWindowPos(lastPos);
            ImGui::SetNextWindowSize(lastSize);
        }
    }
    if (fullScreen) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetFrameHeight()));
    }

    int i = 0;
    auto drawList = ImGui::GetBackgroundDrawList();

    for (auto& v : HookerData::visited) {
        if (v.name.empty()) continue;
        char label[512]{0};
        auto dt = ImGui::GetIO().DeltaTime;
        constexpr ImVec4 GREEN = {0.f, 1.f, 0.f, 1.f};
        ImColor color = ImColor(1.f, 1.f, 1.f, 1.f);
        if (v.time > 0.f) {
            v.time -= dt;
            auto t = v.time;
            color = ImColor(ImLerp(color.Value.x, GREEN.x, t), ImLerp(color.Value.y, GREEN.y, t),
                            ImLerp(color.Value.z, GREEN.z, t), 1.f);
        }
        v.goneTime -= dt;
        if (v.goneTime > 0.f && v.goneTime <= 1.f) {
            auto t = v.goneTime;
            color.Value.w = ImLerp(0.f, color.Value.w, t);
        }
        if (v.goneTime <= 0.f) {
            v.name = "";
            continue;
        }

        bool hasArgs = !v.argsStr.empty();
        bool hasRet = !v.retStr.empty();

        if (hasArgs) {
            float lineH = ImGui::GetTextLineHeight();
            float maxW = ImGui::GetIO().DisplaySize.x - 40.f;
            const char* indent = "           ";

            std::vector<std::string> args;
            {
                std::string tmp;
                for (char c : v.argsStr) {
                    if (c == ',') { args.push_back(tmp); tmp.clear(); }
                    else if (c != ' ' || !tmp.empty()) tmp += c;
                }
                if (!tmp.empty()) args.push_back(tmp);
            }

            std::string prefix = v.name + "(";
            std::string curLine = prefix;
            bool firstLine = true;

            auto flushLine = [&](const std::string& text, bool last) {
                std::string full = text + (last ? ")" : ",");
                if (!firstLine) full = std::string(indent) + full;
                if (v.hitCount > 0 && last) {
                    char hc[32]; snprintf(hc, sizeof(hc), " (%dx)", v.hitCount);
                    full += hc;
                }
                auto sz = ImGui::CalcTextSize(full.c_str());
                ImVec2 pos{20, 150 + (lineH * i)};
                drawList->AddRectFilled(pos, {pos.x + sz.x, pos.y + sz.y}, IM_COL32(0, 0, 0, 100));
                drawList->AddText(pos, color, full.c_str());
                i++;
                firstLine = false;
            };

            for (int ai = 0; ai < (int)args.size(); ai++) {
                bool last = (ai == (int)args.size() - 1);
                std::string candidate = curLine + args[ai];
                std::string testStr = (firstLine ? "" : indent) + candidate + (last ? ")" : ",");
                float w = ImGui::CalcTextSize(testStr.c_str()).x;
                if (w > maxW && curLine != prefix) {
                    flushLine(curLine.substr(0, curLine.size()), false);
                    curLine = args[ai];
                } else {
                    if (curLine != prefix) curLine += ", ";
                    curLine += args[ai];
                }
                if (last) flushLine(curLine, true);
            }
            if (args.empty()) {
                std::string full = prefix + ")";
                auto sz = ImGui::CalcTextSize(full.c_str());
                ImVec2 pos{20, 150 + (lineH * i)};
                drawList->AddRectFilled(pos, {pos.x + sz.x, pos.y + sz.y}, IM_COL32(0, 0, 0, 100));
                drawList->AddText(pos, color, full.c_str());
                i++;
            }
        } else if (hasRet) {
            snprintf(label, sizeof(label), "%s -> %s", v.name.c_str(), v.retStr.c_str());
            if (v.hitCount > 0)
                snprintf(label + strlen(label), sizeof(label) - strlen(label), " (%dx)", v.hitCount);
            auto labelSize = ImGui::CalcTextSize(label);
            ImVec2 labellPos{20, 150 + (labelSize.y * i)};
            drawList->AddRectFilled(labellPos, {labellPos.x + labelSize.x, labellPos.y + labelSize.y}, IM_COL32(0, 0, 0, 100));
            drawList->AddText(labellPos, color, label);
            i++;
        } else {
            snprintf(label, sizeof(label), "%s", v.name.c_str());
            if (v.hitCount > 0)
                snprintf(label + strlen(label), sizeof(label) - strlen(label), " (%dx)", v.hitCount);
            auto labelSize = ImGui::CalcTextSize(label);
            ImVec2 labellPos{20, 150 + (labelSize.y * i)};
            drawList->AddRectFilled(labellPos, {labellPos.x + labelSize.x, labellPos.y + labelSize.y}, IM_COL32(0, 0, 0, 100));
            drawList->AddText(labellPos, color, label);
            i++;
        }
    }

    collapsed = !ImGui::Begin(title, nullptr, (fullScreen ? ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove : 0));
    if (fullScreen) {
        ImGui::PopStyleVar();
    }

    if (!toolReady.load()) {
        ImGui::Text("Loading...");
        ImGui::End();
        return;
    }

    static bool changeToToolsTab = false;
    if (ImGui::BeginTabBar("mainTabber")) {
        if (ImGui::BeginTabItem("Tools", nullptr, changeToToolsTab ? ImGuiTabItemFlags_SetSelected : 0)) {
            changeToToolsTab = false;
            if (ImGui::Checkbox("Fullscreen", &fullScreen)) {
                if (fullScreen) {
                    lastSize = ImGui::GetWindowSize();
                    lastPos = ImGui::GetWindowPos();
                }
                resetWindow = true;
            }
            Tool::Draw();
            ImGui::EndTabItem();
        }

        if (Tool::GetHookerCount() > 0) {
            if (ImGui::BeginTabItem("Tracer")) {
                Tool::DrawTracerTab(changeToToolsTab);
                ImGui::EndTabItem();
            }
        }

        if (ImGui::BeginTabItem("Dumper")) {
            Tool::Dumper();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("ESP")) {
            ESP_TESTER::UI();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings")) {
            ImGui::Separator();
            ImGui::Text("Display");
            auto preview = possibleScale[selectedScale];
            if (ImGui::BeginCombo("Scale##Global", preview)) {
                for (int si = 0; si < (int)possibleScale.size(); si++) {
                    bool selected = si == selectedScale;
                    if (ImGui::Selectable(possibleScale[si], selected)) {
                        selectedScale = si;
                        ConfigSet("selectedScale", selectedScale);
                        doChangeScale = true;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            static char text[256] = "";
            ImGui::InputText("Input", text, sizeof(text));
            ImGui::Separator();
            ImGui::Text("Info");
            static auto packageName = UnityResolve::getPackageName();
            static auto unityVersion = UnityResolve::getUnityVersion();
            static auto gameVersion = UnityResolve::getGameVersion();
            ImGui::Text("Package: %s", packageName.c_str());
            ImGui::Text("Version: %s", gameVersion.c_str());
            ImGui::Text("Unity: %s", unityVersion.c_str());
#ifdef __aarch64__
            ImGui::Text("Arch: %s", "arm64-v8a");
#else
            ImGui::Text("Arch: %s", "armeabi-v7a");
#endif
            ImGui::Separator();

            if (ImGui::Button("Updates and stuff : https://t.me/En_Xperience", ImVec2(-1, 0)))
                openURL("https://t.me/En_Xperience");
            if (ImGui::Button("Youtube : https://www.youtube.com/@mIsmanXP", ImVec2(-1, 0)))
                openURL("https://www.youtube.com/@mIsmanXP");
            if (ImGui::Button("Platinmods thread", ImVec2(-1, 0)))
                openURL("https://platinmods.com/threads/imgui-il2cpp-tool.211155/");
            ImGui::Separator();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    static bool doCalculate = false;
    if (doCalculate) {
        doCalculate = false;
        Tool::CalculateSomething();
    }

    if (doChangeScale) {
        doChangeScale = false;
        static auto font = ImGui::GetFont();
        font->Scale = scaleFactors[selectedScale];
        auto style = initialStyle;
        style.ScaleAllSizes(font->Scale);
        ImGui::GetStyle() = style;
        doCalculate = true;
        if (fullScreen)
            resetWindow = true;
    }

    ImGui::End();
}

EGLBoolean(*orig_eglSwapBuffers)(EGLDisplay, EGLSurface);
EGLBoolean _eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    eglQuerySurface(dpy, surface, EGL_WIDTH, &glWidth);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &glHeight);

    if (!setup) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.Fonts->AddFontFromFileTTF("/system/fonts/NotoSansCJK-Regular.ttc", 40.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());

        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(2.5f);
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScrollbarSize *= 1.25f;
        initialStyle = ImGui::GetStyle();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        setup = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(glWidth, glHeight);
    static bool prevWantTextInput = false;
    bool wantTextInput = ImGui::GetIO().WantTextInput;
    if (wantTextInput != prevWantTextInput) {
        prevWantTextInput = wantTextInput;
        ShowKeyboard(wantTextInput);
    }
    UnityResolve::EnsureAttached();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2((float)glWidth / 2, (float)glHeight / 2), ImGuiCond_Once);
    Keyboard::Update();
    Unity::ProcessInput();
    DrawMenu();
    ESP_TESTER::DrawESP(ImGui::GetForegroundDrawList(), glWidth, glHeight);
    {
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - wmStart).count();
        float hue = std::fmod(elapsed * 0.5f, 1.0f);
        ImVec4 color = hsv2rgb(hue, 0.9f, 1.0f);
        color.w = 0.85f;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        std::string wmText = "https://t.me/dairymays";
        ImVec2 textSize = ImGui::CalcTextSize(wmText.c_str());
        float centerX = ((float)glWidth - textSize.x) * 0.5f;
        dl->AddText(ImGui::GetFont(), 40.0f, ImVec2(centerX - 50.0f, 20.0f),
                   ImGui::GetColorU32(color), wmText.c_str());
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return orig_eglSwapBuffers(dpy, surface);
}

void *input_thread(void *) {
    void* egl = DobbySymbolResolver("libEGL.so", "eglSwapBuffers");
    void* inp = DobbySymbolResolver("libinput.so", "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE");

    if (egl) DobbyHook(egl, (void*)_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    if (inp) DobbyHook(inp, (void*)myInput, (void**)&origInput);

    return nullptr;
}

void *MainThread(void *) {
    while (!IsLibraryLoaded("libil2cpp.so"))
        usleep(100000);

    void *handle = xdl_open("libil2cpp.so", XDL_DEFAULT);
    bool fromXdl = handle != nullptr;
    if (!handle)
        handle = dlopen("libil2cpp.so", RTLD_NOW);

    if (handle) {
        il2cpp_api_init(handle);
        if (fromXdl) xdl_close(handle);
        else dlclose(handle);
        UnityResolve::EnsureAttached();
        Unity::HookInput();
        Keyboard::Init();
        ConfigInit();
        selectedScale = ConfigGet<int>("selectedScale", selectedScale);
        if (selectedScale < 0 || selectedScale >= (int)scaleFactors.size()) {
            selectedScale = 3;
            ConfigSet("selectedScale", selectedScale);
        }
        Tool::Init();
        doChangeScale = true;
        toolReady = true;
    }

    return nullptr;
}

__attribute__((constructor))
void libmain() {
    pthread_t thread1, thread2;
    pthread_create(&thread1, nullptr, input_thread, nullptr);
    pthread_create(&thread2, nullptr, MainThread, nullptr);
    pthread_detach(thread1);
    pthread_detach(thread2);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    jvm = vm;
    LoadDex(imgui_dex, imgui_dex_len);
    return JNI_VERSION_1_6;
}

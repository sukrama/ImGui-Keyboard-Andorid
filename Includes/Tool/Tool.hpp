#pragma once
#include "il2cpp/UnityResolve.hpp"

class ClassesTab;

namespace Tool {
    void Init();
    void Draw();
    void Dumper();
    void DrawTracerTab(bool& changeToToolsTab);
    void CalculateSomething();
    size_t GetHookerCount();
    void ConfigSave();
    void ConfigLoad();
    void ConfigInit();
    bool ToggleHooker(UnityResolve::Method* method, int state = -1);
    ClassesTab& GetFirstTab();
    ClassesTab& OpenNewTab();
    ClassesTab& OpenNewTabFromClass(UnityResolve::Class* klass);
}

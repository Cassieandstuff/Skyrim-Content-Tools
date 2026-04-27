#pragma once
#include "AppState.h"  // AppTab enum
#include <vector>

class IPanel;

// ── AppTabDef ──────────────────────────────────────────────────────────────────
// Describes one top-level editor tab: its identity and which panels to draw.
// Panels are non-owning pointers — lifetime is managed by the App class.
struct AppTabDef {
    AppTab               id;
    const char*          label;
    std::vector<IPanel*> panels;
};

// ── TabRegistry ───────────────────────────────────────────────────────────────
// Singleton registry of all top-level editor tabs.
// MainLayout iterates this to draw the tab bar and dispatch panel draws.
class TabRegistry {
public:
    static TabRegistry& Get();

    void       Register(AppTabDef def);
    AppTabDef* Find(AppTab id);
    const std::vector<AppTabDef>& All() const { return tabs_; }

private:
    TabRegistry() = default;
    std::vector<AppTabDef> tabs_;
};

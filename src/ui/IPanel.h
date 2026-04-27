#pragma once

struct AppState;

// Abstract base for every UI panel.
// Panels are stateless draw-function wrappers; they receive AppState by reference
// and have no lifecycle beyond construction.  Instances are owned by the App class
// and registered (non-owning) into TabRegistry via TabDefs.
class IPanel {
public:
    virtual ~IPanel() = default;

    // Called once per frame when the owning tab is active.
    virtual void Draw(AppState& state) = 0;

    // Stable string key used as the ImGui window title *and* dock target name.
    // Must be unique across all registered panels.
    virtual const char* PanelID() const = 0;
};

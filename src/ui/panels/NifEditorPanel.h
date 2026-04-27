#pragma once
#include "ui/IPanel.h"
#include "NifEditorState.h"

// ── NifPropertiesPanel ────────────────────────────────────────────────────────
// Left lower panel: selected-block properties table.
class NifPropertiesPanel : public IPanel {
public:
    explicit NifPropertiesPanel(NifEditorState& s) : m_s(s) {}
    void        Draw(AppState& state) override;
    const char* PanelID()      const override { return "NIF Properties"; }
private:
    NifEditorState& m_s;
};

// ── NifViewportPanel ──────────────────────────────────────────────────────────
// Central panel: 3-D viewport with orbit camera.
class NifViewportPanel : public IPanel {
public:
    explicit NifViewportPanel(NifEditorState& s) : m_s(s) {}
    void        Draw(AppState& state) override;
    const char* PanelID()      const override { return "NIF Viewport"; }
private:
    NifEditorState& m_s;
};

#pragma once
#include "ui/IPanel.h"
#include "NifEditorState.h"
#include <string>
#include <vector>

// ── NifBrowserPanel ───────────────────────────────────────────────────────────
// Scans dataFolder/meshes/ for *.nif files and presents them in a filterable
// tree.  Double-clicking a file calls NifEditorState::LoadFile().
class NifBrowserPanel : public IPanel {
public:
    explicit NifBrowserPanel(NifEditorState& s) : m_s(s) {}

    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "NIF Browser"; }

private:
    NifEditorState& m_s;

    struct Entry {
        std::string fullPath;
        std::string label;      // path relative to meshes/ root for display
        std::string lowerLabel; // for filter matching
    };

    std::string        m_scannedDataFolder;
    std::vector<Entry> m_entries;
    char               m_filter[128] = {};

    void Scan(const std::string& dataFolder);
    void DrawTree(AppState& state);
};

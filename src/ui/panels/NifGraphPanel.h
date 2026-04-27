#pragma once
#include "ui/IPanel.h"
#include "NifEditorState.h"
#include <imgui_node_editor.h>
#include <vector>

// ── NifGraphPanel ─────────────────────────────────────────────────────────────
// Replaces the tree view with an imgui-node-editor graph of the NIF block
// hierarchy. Two tabs: "Block Graph" (full hierarchy) and "Extra Data"
// (filtered to extra-data block kinds).
class NifGraphPanel : public IPanel {
public:
    explicit NifGraphPanel(NifEditorState& s);
    ~NifGraphPanel() override;

    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "NIF Graph"; }

private:
    NifEditorState&                m_s;
    ax::NodeEditor::EditorContext* m_ctx = nullptr;

    int  m_seenVersion    = -1;   // last layoutVersion acted on
    bool m_positionsDirty = false; // call SetNodePosition before next draw
    bool m_needsNav       = false; // call NavigateToContent after positions applied
    int  m_lastSel        = -2;   // last selectedBlock synced to graph
    int  m_activeTab      = 0;    // 0 = Block Graph, 1 = Extra Data

    std::vector<ImVec2> m_positions; // canvas positions, parallel to doc.blocks

    void ComputeLayout();
    void LayoutSubtree(int blockIdx, float x, float& nextY, float xStep, float yStep);
    void DrawGraph(bool extraDataOnly);

    static ImVec4 KindColor(const std::string& kind);
};

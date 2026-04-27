#pragma once
#include "ui/IPanel.h"

// Phase 1: stub.
// Phase 7: imgui-node-editor node canvas — scene DAG, node selection drives
//          active timeline context.
class SceneGraphPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Scene Graph"; }
};

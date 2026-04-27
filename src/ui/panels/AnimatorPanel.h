#pragma once
#include "ui/IPanel.h"

class AnimatorPanel : public IPanel {
public:
    void        Draw(AppState& state) override;
    const char* PanelID() const override { return "Animator"; }
};

#include "NifGraphPanel.h"
#include "AppState.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

namespace ned = ax::NodeEditor;

// ── Construction / destruction ────────────────────────────────────────────────

NifGraphPanel::NifGraphPanel(NifEditorState& s)
    : m_s(s)
{
    ned::Config cfg;
    cfg.SettingsFile = nullptr;  // positions managed by ComputeLayout()
    m_ctx = ned::CreateEditor(&cfg);
}

NifGraphPanel::~NifGraphPanel()
{
    if (m_ctx)
        ned::DestroyEditor(m_ctx);
}

// ── Tree auto-layout ──────────────────────────────────────────────────────────

void NifGraphPanel::LayoutSubtree(int blockIdx, float x, float& nextY,
                                   float xStep, float yStep)
{
    const NifBlock& b = m_s.doc.blocks[blockIdx];
    if (b.children.empty()) {
        m_positions[blockIdx] = { x, nextY };
        nextY += yStep;
        return;
    }
    const float startY = nextY;
    for (int c : b.children)
        LayoutSubtree(c, x + xStep, nextY, xStep, yStep);
    const float endY = nextY - yStep;
    m_positions[blockIdx] = { x, (startY + endY) * 0.5f };
}

void NifGraphPanel::ComputeLayout()
{
    m_positions.assign(m_s.doc.blocks.size(), { 0.f, 0.f });
    float nextY = 0.f;
    constexpr float kXStep = 250.f;
    constexpr float kYStep = 76.f;
    for (int r : m_s.doc.roots)
        LayoutSubtree(r, 0.f, nextY, kXStep, kYStep);
    m_positionsDirty = true;
    m_needsNav = true;
}

// ── Node color by kind ────────────────────────────────────────────────────────

ImVec4 NifGraphPanel::KindColor(const std::string& kind)
{
    if (kind == "NiNode"       || kind == "BSFadeNode"      ||
        kind == "BSMultiBoundNode" || kind == "BSLeafAnimNode" ||
        kind == "BSOrderedNode"    || kind == "BSDebrisNode")
        return { 0.18f, 0.42f, 0.72f, 1.f };  // blue — scene node

    if (kind == "BSTriShape"   || kind == "NiTriShape"    ||
        kind == "BSSubIndexTriShape" || kind == "BSMeshLODTriShape" ||
        kind == "NiTriStrips")
        return { 0.10f, 0.58f, 0.80f, 1.f };  // cyan — mesh

    if (kind.find("ShaderProperty") != std::string::npos)
        return { 0.52f, 0.24f, 0.78f, 1.f };  // purple — shader

    if (kind.find("TextureSet") != std::string::npos)
        return { 0.78f, 0.44f, 0.10f, 1.f };  // orange — texture

    if (kind.find("Skin") != std::string::npos)
        return { 0.78f, 0.22f, 0.22f, 1.f };  // red — skin

    if (kind.find("ExtraData") != std::string::npos  ||
        kind == "BSXFlags"   || kind == "BSInvMarker" ||
        kind == "BSBound")
        return { 0.22f, 0.68f, 0.32f, 1.f };  // green — extra data

    if (kind.find("Controller") != std::string::npos ||
        kind.find("Interpolator") != std::string::npos)
        return { 0.78f, 0.66f, 0.10f, 1.f };  // yellow — animation

    return { 0.32f, 0.32f, 0.38f, 1.f };      // gray — default
}

// ── ID space partitioning ─────────────────────────────────────────────────────
// emitInteractiveArea formats all ID types with "%p", so NodeId(N), PinId(N),
// and LinkId(N) would all produce the same idString and collide in ImGui's ID
// system.  Give each type a distinct uintptr_t range so they never overlap.
static constexpr uintptr_t kNodeBase = 0x00000001u; // blockIndex + kNodeBase
static constexpr uintptr_t kInPBase  = 0x00010000u; // blockIndex + kInPBase
static constexpr uintptr_t kOutPBase = 0x00020000u; // blockIndex + kOutPBase
static constexpr uintptr_t kLinkBase = 0x00030000u; // blockIndex + kLinkBase

// ── Graph draw ────────────────────────────────────────────────────────────────

void NifGraphPanel::DrawGraph(bool extraDataOnly)
{
    const NifDocument& doc = m_s.doc;

    ned::SetCurrentEditor(m_ctx);
    ned::Begin("##nif_graph", ImGui::GetContentRegionAvail());

    // Apply auto-computed positions before drawing any nodes.
    if (m_positionsDirty && !m_positions.empty()) {
        for (int i = 0; i < (int)doc.blocks.size(); i++)
            ned::SetNodePosition(ned::NodeId(kNodeBase + i), m_positions[i]);
        m_positionsDirty = false;
    }

    // ── Nodes ─────────────────────────────────────────────────────────────────
    for (const NifBlock& b : doc.blocks) {
        if (extraDataOnly) {
            const bool isExtra =
                b.isExtraData;
            if (!isExtra) continue;
        }

        const uintptr_t nid  = kNodeBase + (uintptr_t)b.index;
        const uintptr_t inP  = kInPBase  + (uintptr_t)b.index;
        const uintptr_t outP = kOutPBase + (uintptr_t)b.index;

        ned::BeginNode(ned::NodeId(nid));
        ImGui::PushID(b.index);

        // Left (input) pin — receives link from parent
        ned::BeginPin(ned::PinId(inP), ned::PinKind::Input);
        ImGui::Dummy({ 6.f, 14.f });
        ned::EndPin();

        ImGui::SameLine(0.f, 6.f);

        // Node body
        ImGui::BeginGroup();

        // Kind label colored by category
        ImGui::TextColored(KindColor(b.kind), "%s", b.kind.c_str());

        // Name (if any)
        if (!b.name.empty())
            ImGui::TextDisabled("  %s", b.name.c_str());

        // Shape geometry stats
        if (b.isShape) {
            for (const NifDocShape& ds : doc.shapes) {
                if (ds.blockIndex != b.index) continue;
                ImGui::TextDisabled("  %d verts / %d tris",
                    (int)ds.meshData.positions.size(),
                    (int)ds.meshData.indices.size() / 3);
                break;
            }
        }

        ImGui::EndGroup();

        ImGui::SameLine(0.f, 6.f);

        // Right (output) pin — sends links to children
        ned::BeginPin(ned::PinId(outP), ned::PinKind::Output);
        ImGui::Dummy({ 6.f, 14.f });
        ned::EndPin();

        ImGui::PopID();
        ned::EndNode();
    }

    // ── Links ─────────────────────────────────────────────────────────────────
    for (const NifBlock& b : doc.blocks) {
        if (b.parent < 0) continue;
        if (extraDataOnly) {
            const bool isExtra =
                b.isExtraData;
            if (!isExtra) continue;
        }
        const uintptr_t parentOut = kOutPBase + (uintptr_t)b.parent;
        const uintptr_t selfIn    = kInPBase  + (uintptr_t)b.index;
        ned::Link(ned::LinkId(kLinkBase + (uintptr_t)b.index),
                  ned::PinId(parentOut), ned::PinId(selfIn),
                  { 0.75f, 0.75f, 0.75f, 0.65f }, 1.5f);
    }

    // ── Selection sync ────────────────────────────────────────────────────────

    // External change (e.g. Properties panel clicked something): push to graph.
    if (m_lastSel != m_s.selectedBlock) {
        m_lastSel = m_s.selectedBlock;
        ned::ClearSelection();
        if (m_s.selectedBlock >= 0) {
            ned::SelectNode(ned::NodeId(kNodeBase + (uintptr_t)m_s.selectedBlock));
            ned::NavigateToSelection(false, 0.25f);
        }
    }
    // Graph-driven change: pull into shared state.
    else if (ned::HasSelectionChanged()) {
        ned::NodeId picked[1];
        if (ned::GetSelectedNodes(picked, 1) > 0) {
            const int idx = (int)((uintptr_t)picked[0].Get() - kNodeBase);
            if (idx >= 0 && idx < (int)doc.blocks.size()) {
                m_s.selectedBlock = idx;
                m_lastSel         = idx;
            }
        } else if (ned::GetSelectedObjectCount() == 0) {
            m_s.selectedBlock = -1;
            m_lastSel         = -1;
        }
    }

    // Zoom to fit after loading a new document.
    if (m_needsNav) {
        ned::NavigateToContent(0.4f);
        m_needsNav = false;
    }

    ned::End();
    ned::SetCurrentEditor(nullptr);
}

// ── IPanel::Draw ──────────────────────────────────────────────────────────────

void NifGraphPanel::Draw(AppState& /*state*/)
{
    if (!ImGui::Begin(PanelID())) { ImGui::End(); return; }

    // ── Toolbar ───────────────────────────────────────────────────────────────
    if (ImGui::Button("Open NIF...")) {
#if defined(_WIN32)
        char buf[MAX_PATH] = {};
        OPENFILENAMEA ofn  = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "NIF Files\0*.nif\0All Files\0*.*\0";
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = sizeof(buf);
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle  = "Open NIF";
        if (GetOpenFileNameA(&ofn))
            m_s.LoadFile(buf);
#endif
    }

    if (!m_s.doc.empty()) {
        ImGui::SameLine(0.f, 8.f);
        const std::string& p = m_s.doc.path;
        const size_t slash   = p.find_last_of("/\\");
        const char* fname    = (slash != std::string::npos) ? p.c_str() + slash + 1 : p.c_str();
        ImGui::TextDisabled("%s  |  %d blocks  |  %d shapes",
                            fname,
                            (int)m_s.doc.blocks.size(),
                            (int)m_s.doc.shapes.size());
    }

    // ── Graph mode tabs ───────────────────────────────────────────────────────
    if (ImGui::BeginTabBar("##graphTabs")) {
        if (ImGui::BeginTabItem("Block Graph")) { m_activeTab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Extra Data"))  { m_activeTab = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    // Detect document reload.
    if (m_seenVersion != m_s.layoutVersion) {
        m_seenVersion = m_s.layoutVersion;
        m_lastSel     = -2;  // force selection re-sync
        if (!m_s.doc.empty())
            ComputeLayout();
    }

    // ── Content ───────────────────────────────────────────────────────────────
    if (m_s.doc.empty()) {
        ImGui::TextDisabled("(no file loaded)");
    } else if (m_activeTab == 1) {
        // Extra Data tab — filter to extra-data block kinds.
        bool hasExtra = false;
        for (const NifBlock& b : m_s.doc.blocks) {
            if (b.isExtraData) { hasExtra = true; break; }
        }
        if (hasExtra) {
            DrawGraph(true);
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("No extra data blocks detected in this NIF.");
            ImGui::TextDisabled("BSXFlags, BSBehaviorGraphExtraData, etc. appear here");
            ImGui::TextDisabled("when present in the NIF block list.");
        }
    } else {
        DrawGraph(false);
    }

    ImGui::End();
}

#include "NifEditorPanel.h"
#include "app/AppState.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>

static const glm::mat4 kNifToWorld =
    glm::rotate(glm::mat4(1.f), glm::radians(-90.f), glm::vec3(1.f, 0.f, 0.f));

// ── NifPropertiesPanel ────────────────────────────────────────────────────────

void NifPropertiesPanel::Draw(AppState& /*state*/)
{
    if (!ImGui::Begin(PanelID())) { ImGui::End(); return; }

    const int sel = m_s.selectedBlock;
    if (sel < 0 || sel >= (int)m_s.doc.blocks.size()) {
        ImGui::TextDisabled("Select a block to inspect");
        ImGui::End();
        return;
    }

    const NifBlock& b = m_s.doc.blocks[sel];

    if (ImGui::BeginTable("##bprops", 2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 72.f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto Row = [&](const char* key, const char* val) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", key);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(val);
        };
        char buf[256];

        Row("Kind",  b.kind.c_str());
        Row("Name",  b.name.empty() ? "(none)" : b.name.c_str());

        std::snprintf(buf, sizeof(buf), "%d", sel);
        Row("Index", buf);

        if (b.parent >= 0) {
            const NifBlock& par = m_s.doc.blocks[b.parent];
            std::snprintf(buf, sizeof(buf), "%s (%d)",
                          par.name.empty() ? par.kind.c_str() : par.name.c_str(), b.parent);
            Row("Parent", buf);
        } else {
            Row("Parent", "(root)");
        }

        const glm::vec3 t = glm::vec3(b.toRoot[3]);
        std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", t.x, t.y, t.z);
        Row("Pos (root)", buf);

        const float scl = glm::length(glm::vec3(b.toParent[0]));
        std::snprintf(buf, sizeof(buf), "%.4f", scl);
        Row("Scale", buf);

        if (b.isShape) {
            for (const NifDocShape& ds : m_s.doc.shapes) {
                if (ds.blockIndex != sel) continue;
                std::snprintf(buf, sizeof(buf), "%d", (int)ds.meshData.positions.size());
                Row("Verts", buf);
                std::snprintf(buf, sizeof(buf), "%d", (int)ds.meshData.indices.size() / 3);
                Row("Tris", buf);
                break;
            }
        } else if (b.isExtraData) {
            if (!b.extraValue.empty())
                Row("Value", b.extraValue.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%d", (int)b.children.size());
            Row("Children", buf);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

// ── NifViewportPanel ──────────────────────────────────────────────────────────

void NifViewportPanel::Draw(AppState& /*state*/)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin(PanelID());
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    const ImVec2 vsize = ImGui::GetContentRegionAvail();
    if (vsize.x >= 1.f && vsize.y >= 1.f) {
        const glm::mat4 proj = m_s.camera.Proj(vsize.x / vsize.y);
        const glm::mat4 view = m_s.camera.View();

        m_s.renderer.BeginFrame((int)vsize.x, (int)vsize.y);
        m_s.renderer.SetCamera(view, proj);
        m_s.renderer.DrawGrid(10.f, 10);

        for (int si = 0; si < (int)m_s.handles.size(); si++) {
            const NifDocShape& ds    = m_s.doc.shapes[si];
            const NifBlock&    block = m_s.doc.blocks[ds.blockIndex];
            const bool selected = (m_s.selectedBlock == block.index);

            const TextureHandle th = (si < (int)m_s.textures.size())
                                     ? m_s.textures[si]
                                     : TextureHandle::Invalid;
            const bool hasTex = (th != TextureHandle::Invalid);

            DrawSurface surf;
            surf.diffuse = th;
            surf.tint = selected
                ? glm::vec4(1.00f, 0.78f, 0.20f, 1.f)
                : (hasTex ? glm::vec4(1.f, 1.f, 1.f, 1.f)
                          : glm::vec4(0.70f, 0.70f, 0.75f, 1.f));

            m_s.renderer.DrawMesh(m_s.handles[si], kNifToWorld * block.toRoot, surf);

            // Draw a wireframe overlay on the selected shape.
            if (selected) {
                DrawSurface wire;
                wire.wireframe = true;
                wire.tint = glm::vec4(1.00f, 0.78f, 0.20f, 0.7f);
                m_s.renderer.DrawMesh(m_s.handles[si], kNifToWorld * block.toRoot, wire);
            }
        }

        m_s.renderer.EndFrame();

        ImGui::Image(m_s.renderer.GetOutputTexture(), vsize,
                     ImVec2(0.f, 1.f), ImVec2(1.f, 0.f));

        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.f)) {
                if (io.KeyShift)
                    m_s.camera.Pan(io.MouseDelta.x, -io.MouseDelta.y);
                else
                    m_s.camera.Orbit(-io.MouseDelta.x * 0.5f, io.MouseDelta.y * 0.5f);
            }
            if (io.MouseWheel != 0.f)
                m_s.camera.Zoom(io.MouseWheel);
        }
    }

    ImGui::End();
}

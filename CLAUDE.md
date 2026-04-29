# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Build

**Always build from Visual Studio, not the command line.** The CLI cmake approach fails on this machine. Open the `.sln` in VS and build from there.

Do **not** push to GitHub — the user pushes from VS. Claude commits locally only.

---

## Project Overview

**Skyrim Content Tools (SCT)** — a non-linear scene and animation authoring tool for Skyrim Special Edition. Mental model: a video editor where the footage is Havok animation and the screen is a 3D viewport. Target: SSE/AE only (stream version 100 BSTriShape NIFs, LZ4 BSAs). Legendary Edition is explicitly out of scope.

Repo: `F:\Modding\Development\SKSE Dev\Projects\Claude Working Directory\Skyrim-Content-Tools`

---

## Architecture

### Panel System (`src/ui/`)

All UI panels implement `IPanel` (`src/ui/IPanel.h`):
```cpp
class IPanel {
    virtual void Draw(AppState& state) = 0;
    virtual const char* PanelID() const = 0;
};
```

Panels are **stateless draw functions** — all persistent data lives in `AppState`. Panel instances are owned **by value** in `MainLayout` and registered as **non-owning pointers** into `TabRegistry`. Adding a new panel/tab requires zero changes to existing code.

**`TabRegistry`** — maps `AppTab` enum → `AppTabDef { label, vector<IPanel*> }`.  
**`TrackRegistry`** — maps `TrackType` → evaluate callbacks. New track types registered at startup.

### AppState (`src/AppState.h`)

Single source of truth passed by reference to every `IPanel::Draw`. Key fields:
- `cast[]` — `ActorDocument` records (identity + resolved asset cache + authored overrides)
- `actors[]` — scene instances (`castIndex` → cast entry)
- `clips[]` / `skeletons[]` — animation and skeleton asset pools
- `sequence` — the NLE document
- `time`, `playing`, `loop` — playback state
- `activeTab`, `selectedClip`, `selectedCast` — UI state
- `toasts[]` — `Toast { message, ttl, level }` queue; rendered via `DrawToasts()` in MainLayout
- `dataFolder` — path to game Data directory (persisted to `sct_settings.ini`)

### ActorDocument (`src/ActorDocument.h`)

Three-layer model:
1. **Identity**: `formId`, `formKey`, `editorId`, `name`, `raceEditorId`, `pluginSource`
2. **Resolved asset cache**: `creatureType`, `skeletonHkxPath`, `bodyNifPath`, `headNifPath`, `headTriPath`, `isFemale`
3. **Authored overrides**: `morphWeights` map

`skeletonIndex` is a runtime field (index into `AppState::skeletons`), not serialised.

### AppTab Enum

```cpp
enum class AppTab { SceneEditor=0, AnimEditor, NifEditor, Workflow };
```

Workflow tab hosts `PluginBrowserPanel` — plugin search/NPC import, actor cast management, future previs generation.

### Renderer (`src/renderer/`)

`ISceneRenderer` interface — `GlSceneRenderer` is the concrete OpenGL implementation. Single `m_renderer` instance in `MainLayout` shared by all viewports (render-to-texture via FBO → `ImGui::Image()`).

Key types:
- `MeshHandle` / `TextureHandle` — typed enum handles (Invalid = 0)
- `DrawSurface { diffuse, tint, wireframe, xray }` — per-draw surface params
- `MeshData { positions, normals, uvs, boneIndices, boneWeights, indices }` — upload descriptor

Frame sequence: `BeginFrame → SetCamera → Draw* → EndFrame → GetOutputTexture`.

`DrawSkinnedMesh` accepts `std::span<const glm::mat4> boneTransforms` — linear blend skinning done on GPU.

### NLE / Sequence (`src/Sequence.h`)

```
Sequence
  actorTracks[]  → ActorTrackGroup { actorIndex, lanes[] }
    lanes[]      → TrackLane { type, items[] }
      items[]    → SequenceItem { assetIndex, seqStart, trimIn, trimOut, blendIn, blendOut }
  sceneTracks[]  → TrackLane[]  (Camera, Audio)
```

`Sequence::Evaluate(t, state, out[])` → writes `ActorEval { Pose }` per actor.

### Havok / Animation Pipeline

- `.hkx` binary → `DotNetHost` → `SctBridge.HkxToXml()` → XML → skeleton/animation parser
- `.xml` parsed directly
- `Skeleton::bones[]` — name, parent index, local transform
- `Pose::SolveFK()` — computes `worldPos[]` / `worldRot[]` from accumulated local transforms
- `AnimClip::Evaluate(t, pose)` — lerp/slerp per channel, bone binding by name

### NIF Rendering (Phase 3 — partially implemented)

**Two parallel skeletal systems in Skyrim**: Havok skeleton (drives animation) + NIF NiNode hierarchy (drives rendering). Connected at runtime via name-matched transform copy. SCT already has the Havok skeleton; NIF rendering requires LBS skinning math using `BSSkin::Instance` bone names + inverse bind matrices.

- `NifDocument` / `NifDocShape` / `NifBlock` — loaded via `LoadNifDocument()` using nifly
- `NifViewportPanel` already does textured rendering (loads DDS textures via `renderer_.LoadTexture`)
- `ViewportPanel` currently hardcodes `{ .wireframe = true }` — textured rendering is next pending work

### Plugin Integration

`IPluginBackend` / `MutagenBackend` — Mutagen (MIT) via `SctBridge.dll` + `DotNetHost`. Used for NPC record resolution, body NIF path discovery, facegen NIF paths. Set to `nullptr` until `DotNetHost::Init()` succeeds; always null-check before use.

### FaceGen / Expression Morphs

- TRI files are FRTRI003 format (not NIF) — custom thin parser in `TriDocument.cpp`
- ARKit blend shape names (52 morphs, camelCase) — not legacy Skyrim phoneme names
- `BSFaceGenBaseMorphExtraData` on `BSDynamicTriShape` — baked chargen base vertices
- Runtime blend: `finalPos = basePos + Σ(delta[m][i] × weight[m])`
- `morphWeights` map in `ActorDocument` stores authored overrides keyed by ARKit name

**FRTRI003 header layout** (64 bytes, confirmed by binary analysis):
```
+0x00  char[8]  magic = "FRTRI003"
+0x08  int32    vertexNum      — base mesh vertex count
+0x0C  int32    faceNum        — triangle count
+0x10  int32    (reserved=0)
+0x14  int32    (reserved=0)
+0x18  int32    (reserved=0)
+0x1C  int32    uvVertexNum    — unique UV coord count (= vertexNum for 1:1 UV, may be less)
+0x20  int32    morphNum       — diff morph count
+0x24  int32    addMorphNum    — modifier morph count (52 ARKit blend shapes in MFEE)
+0x28  int32    addVertexNum   — extra verts for modifier section (0 in MFEE)
+0x2C  byte[20] (reserved)
```

**FRTRI003 geometry layout** (after header):
1. Base vertices: `vertexNum × 12` bytes (3 × float32)
2. Face indices: `faceNum × 12` bytes (3 × uint32 per triangle)
3. UV coords: `uvVertexNum × 8` bytes (2 × float32) — only if uvVertexNum > 0
4. UV face indices: `faceNum × 12` bytes (3 × uint32) — only if uvVertexNum > 0

**Morph record format** (same for diff and mod morphs):
- `int32 nameLen` (includes null terminator) + `nameLen` bytes (narrow ASCII)
- `float32 baseDiff` — scale: finalDelta = int16_delta × baseDiff
- `vertexNum × int16[3]` — signed per-vertex deltas (dx, dy, dz)

### Face Animation (FaceClip)

HKX body animation files can carry face morph keyframes as Havok annotation track entries. SCT extracts these into `FaceClip` objects (stored in `AppState::faceClips[]`) separately from the body `AnimClip`.

**Annotation text format** (one entry per morph per keyframe):
```
MorphFace.<system>|<morphName>|<intWeight>
```
- `<system>` — face capture system name, e.g. `RokokofaceUBE` (matched by prefix `MorphFace`, rest is informational)
- `<morphName>` — ARKit blend shape name, e.g. `mouthFunnel`, `jawOpen`, `eyeBlinkLeft`
- `<intWeight>` — integer 0–100 representing normalized weight 0.0–1.0

Multiple morphs at the same timestamp = multiple annotation entries sharing that timestamp. Weight divisor is `kWeightScale = 100.f` in `FaceClip.cpp`.

**`FaceClip` data structure** (`src/FaceClip.h/cpp`):
- `channels[]` — one `FaceMorphChannel` per unique ARKit morph found, each with sorted `times[]` + `weights[]` arrays
- `Evaluate(morphName, t)` — linear interpolation for a single morph
- `EvaluateAll(t, outWeights)` — fills all non-zero morphs at time t
- `ParseFromXml(xmlData, xmlLen, name, ...)` — parses Havok packfile XML (output of `HkxToXml`)

**Auto-extraction**: `AppState::LoadClipFromPath` piggybacks face clip extraction on the same HkxToXml buffer that produces the body AnimClip. If face annotations are found (`channels.empty() == false`), the FaceClip is appended to `faceClips[]` at no extra cost.

---

## Key UI Implementation Rules

**ImGui Push/Pop balance**: Always snapshot boolean state BEFORE the widget call when that widget can mutate the state. Example from TimelinePanel LOOP button:
```cpp
const bool loopWas = state.loop;   // snapshot BEFORE Button()
if (loopWas) ImGui::PushStyleColor(ImGuiCol_Button, ...);
if (ImGui::Button("LOOP")) state.loop ^= true;
if (loopWas) ImGui::PopStyleColor();
```

**`ImDrawList::AddText` signature**: Takes `(ImVec2 pos, ImU32 col, const char* text)` — NOT `(float x, float y, ...)`.

**Viewport overlays**: Capture `ImVec2 imgMin = ImGui::GetCursorScreenPos()` before `ImGui::Image()`. Use `ImGui::GetWindowDrawList()` after the Image widget for overlays drawn over the viewport.

**Axis gizmo math**: `glm::mat4 view = camera_.View()` is column-major. `view[col][row]`. World axis X maps to screen as `(view[0][0], -view[0][1])`, Y as `(view[1][0], -view[1][1])`, Z as `(view[2][0], -view[2][1])`.

**Coordinate system**: SCT world space is **Skyrim/Havok Z-up** (X=east, Y=north, Z=up). NIF vertex data, Havok bone transforms, and REFR placement records are all in this same space — no axis conversion is ever needed between them. The camera is configured with `up=(0,0,1)` so OpenGL renders the Z-up scene correctly. `Pose::SolveFK` writes `worldPos[i] = (wT.x, wT.y, wT.z)` directly (no swap). There is no `kNifToWorld` matrix.

**Cell REFR placement**: `T(posX, posY, posZ) * Rx(rotX) * Ry(rotY) * Rz(rotZ) * S` — values used directly from the ESM record, no sign changes, no axis remapping. Extrinsic ZYX order: Rz (yaw around world Z-up) applied first, column-vector form R = Rx·Ry·Rz.

**DockBuilder layout** (`SetupDefaultLayout`): Scene Editor top(65%) = Bin|SceneGraph|Viewport; bottom(35%) = Timeline|Inspector. Workflow = Plugin Browser fills full space.

---

## NIF Format Notes (SSE)

- Version `0x14020007`, stream version **100** for SSE
- `BSTriShape` — inline vertex data; `VertexDesc` uint64 bitfield encodes attribute layout
- `BSDynamicTriShape` — extends BSTriShape with CPU-writable `dynamicData[]` (used for compiled NPC heads)
- `BSBehaviorGraphExtraData` on root NiNode → behavior HKX path → derive skeleton path deterministically
- `BSSkin::Instance` → `boneRefs[]` names + `BSSkinBoneData` inverse bind matrices

---

## File Layout

```
src/
  AnimClip.h/cpp        Clip data + Evaluate()
  HavokAnimation.h/cpp  HKX/XML animation parser
  HavokSkeleton.h/cpp   HKX/XML skeleton parser
  Pose.h/cpp            SolveFK(), worldPos/worldRot
  Skeleton.h            Bone hierarchy
  AppState.h/cpp        Shared state, ScanDataFolder(), settings I/O
  ActorDocument.h       Three-layer actor authoring record
  Sequence.h/cpp        NLE document model + Evaluate()
  DotNetHost.h/cpp      .NET runtime host + SctBridge API
  NifDocument.h/cpp     NIF parsing via nifly
  NifLoader.h/cpp       LoadNifDocument()
  BsaReader.h/cpp       BSA v104/v105 extraction
  renderer/
    ISceneRenderer.h    Renderer interface (MeshHandle, TextureHandle, DrawSurface)
    GlSceneRenderer.h/cpp  OpenGL implementation
  ui/
    IPanel.h            Panel interface
    TabRegistry.h/cpp   AppTab → panels mapping
    TrackRegistry.h/cpp TrackType → evaluate callbacks
    MainLayout.h/cpp    Owns all panels, registers tabs/tracks, draws host
    panels/
      ClipBinPanel       Clips tab + Cast tab (●/○ placement indicators)
      ViewportPanel      3D viewport, multi-actor, orbit camera, axis gizmo, playback badge
      TimelinePanel      NLE timeline, transport bar (step buttons, LOOP toggle)
      InspectorPanel     Context-sensitive: actor properties / scene properties
      PluginBrowserPanel Plugin search/NPC import/create (lives in Workflow tab)
      SceneGraphPanel    Stub
      AnimatorPanel      Stub
      NifEditorPanel     Stub (routes to NifBrowser/NifGraph/NifProps/NifViewport)
      NifBrowserPanel    NIF asset browser
      NifGraphPanel      Node graph for NIF block structure
      NifEditorState.h   Shared state for NIF editor sub-panels
```

---

## Current Phase Status

**Phase 1** (Animation Foundation) ✅ Complete  
**Phase 2** (NLE Timeline & Multi-Actor) 🔄 In Progress — scene graph panel and sequence serialisation remaining  
**Phase 3** (NIF Mesh Rendering) 🔄 In Progress — static NIF geometry renders textured; BSA texture resolution now wired

### Immediate Pending Work

1. **Body NIF auto-attachment** — extend `NpcRecord` + SctBridge to resolve `NPC_ → Race → WornArmor → ArmorAddon → NIF path`; wire into `AddActorFromRecord`

2. **NIF Browser fix** — currently not scanning recursively; will be redesigned as plugin-driven

---

## Export / Runtime Model

SCT targets the **True Cinematics** + **Engine Relay** SKSE runtime:
- Per-actor **behavior HKX** encodes scene graph topology as `hkbStateMachine`
- Per-actor **animation HKX** encodes Beat timeline as clips; non-animation events (audio, camera, face data) encoded as annotation track payloads
- **YAML sidecar** carries spawn list, actor placements, wait node parameters
- **Engine Relay** puts actors into custom Havok user state (physics/CC disabled), injects behavior graphs per-actor without touching shared defaultmale/female.hkx
- **Camera** is a custom minimal creature (single bone, invisible, no AI); camera animation is bone animation

---

## Skyrim-Specific Constraints

- **SSE/AE only** — no Legendary Edition fallback paths anywhere
- **BSA v104/v105** — LZ4 block compressed, 64-bit form IDs
- Skeleton type strings (`"character"`, `"horse"`, `"dragon"`, etc.) extracted from `actors/<type>/` path segment via `ExtractCreatureType()`
- `AppState::ScanDataFolder()` walks `{dataFolder}/meshes/actors/` for `skeleton*.hkx`; results in `discoveredSkeletons[]`. Also builds `bsaSearchList` — plugin-associated BSAs first (stem-prefix match on `discoveredPlugins`), then ini-listed BSAs from `%USERPROFILE%/Documents/My Games/Skyrim Special Edition/Skyrim.ini` `[Archive]` keys `sResourceArchiveList` / `sResourceArchiveList2`.
- `AppState::ResolveAsset(relPath, outBytes)` — resolves a data-folder-relative path to raw bytes. Checks loose file first, then walks `bsaSearchList` via `BsaReader::FindExact` + `Extract`. Used by all texture-loading paths (ViewportPanel, NifEditorState).
- Settings persisted to `sct_settings.ini` in working directory

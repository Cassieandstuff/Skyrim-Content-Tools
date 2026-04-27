# Skyrim Content Tools — Design Document (Priority-Ordered)

> **Fork of DESIGN.md — Status as of 2026-04-25.** Same content as the living document, sections reordered by build dependency / order of operations. Each Part maps to a development phase. Read and implement from top to bottom.

---

## How This Document Is Organized

Original DESIGN.md is structured by topic. This fork is structured by **what must exist before the next thing can be built**. The dependency chain is:

```
Orientation (why) → Architecture foundation → Animation core → NIF rendering
  → Plugin integration → Scene environment → Export/runtime → Anim editor
```

Sections from the original §14 UX/UI spec are distributed into the phase where they first become buildable, rather than collected at the end.

---

# PART 0 — ORIENTATION

*Read before writing any code. These sections establish the vision, constraints, and design rationale. Nothing in them requires implementation.*

---

## 0.1 What SCT Is
*(was §1)*

A purpose-built scene and animation authoring tool for Skyrim Special Edition content creators. The mental model is a **video editor**, not a plugin editor. Actors, animations, cameras, lights, and props are arranged on a non-linear timeline and previewed in a 3D viewport. Export produces the plugin records and animation assets the game engine consumes.

SCT is not xEdit with a better UI. xEdit is a general-purpose plugin inspection and scripting environment. SCT is a workflow tool that happens to read and write plugins as one of its I/O formats.

---

## 0.2 Key Design Decisions & Rationale
*(was §12)*

**Video editor metaphor** — clips are assets in a bin, actors are cast members, the timeline is the scene composition surface. This mental model is more accessible to content creators than a game engine scene editor.

**IPanel + registry pattern** — panels are stateless; all state lives in AppState. Adding a new tab or panel requires zero changes to existing code. New track types are registered at startup with a callback, not hard-coded.

**ViewportPanel construction injection** — the same ViewportPanel class serves different app tabs with different viewport tab sets (Scene Editor gets Scene/Face/Cameras; Anim Editor gets Scene/Bones). No subclassing required.

**nifly for NIF I/O** — The NIF format has 200+ block types, versioned conditional fields, a binary string table, and significantly different geometry layouts between LE and SE. Writing a bespoke parser would be months of work and require maintaining the nif.xml schema by hand. nifly (used by BodySlide and Outfit Studio) handles all of this and provides clean `NifFile::Load` / `NifFile::Save` round-trips. It is GPL-3.0, which is compatible with SCT's distribution model.

**Custom thin TRI parser over extracting from RaceMenu** — The FRTRI003 format is simple enough (~200 lines of C++) that owning the parser outright is preferable to taking on a GPL dependency chain from RaceMenu's chargen plugin. SCT only needs read-only access to difference morph delta arrays — the full stat morph / UV / base geometry data in the TRI file is irrelevant for runtime expression animation. A focused parser is easier to understand and maintain than pulling in a larger library.

**TRI is animation data, not geometry** — For SCT's purposes, a `.tri` file is a named set of float32 delta arrays, not a mesh. The base vertex positions inside the TRI are the race-neutral head (pre-chargen), which SCT never uses — the compiled facegeom NIF already contains the chargen-baked base via `BSFaceGenBaseMorphExtraData`. Keeping this conceptual separation clean prevents future confusion about which vertex buffer is authoritative.

**Mutagen over xeditlib** — MIT vs GPL. xeditlib's GPL v3 is incompatible with a source-available license with additional restrictions. Mutagen provides equivalent plugin read/write with a permissive license, and integrates via the existing SctBridge/.NET bridge pattern.

**Vertex-color-only terrain first** — full 6-layer terrain texture blending is a significant rendering system. Vertex colors from VCLR are already baked approximations of terrain variety and are sufficient for previs spatial context.

**Focal vs context NIF distinction** — loading and rendering every placed NIF in a radius would be prohibitively expensive and visually noisy. Context objects render as bounding boxes. Only objects the scene actively uses (the chair the actor sits in) are promoted to full first-class assets with NIF geometry.

**Animated props as actors** — a door, a chair, a wagon are structurally identical to actors: they have a visual representation and optional animation. The NLE model handles them identically. The difference is NiNode transforms vs bone poses at the evaluation output layer. The same CastEntry/Actor pattern applies with PropEntry/Prop.

**BSBehaviorGraphExtraData as invisible import bridge** — most character and creature NIFs already contain the path to their Havok behavior graph. SCT can derive the skeleton path from this without any user interaction or plugin parsing. This is the key to seamless "drop a NIF, get an actor" UX.

**No bundled Bethesda assets** — SCT reads from the user's installed game Data folder. The data folder path is stored in `sct_settings.ini` and persisted across sessions. Creature types are discovered by scanning `meshes/actors/`.

**Timeline-inside-node as the authoring model** — SCT's scene graph uses Hierarchical Temporal Composition: each Beat node contains a full NLE timeline; the graph canvas handles branching between beats. This is the pattern used by CD Projekt RED's Scene Editor and is the established AAA approach for narrative-driven cinematic tools. The alternative (graph-as-flat-timeline, as in the pre-SCT SceneGenerator) requires inverting the representation at export time and cannot cleanly express parallel tracks. See §10.

**Three distinct wait node types** — QTE (fixed window, single input, exactly two forks), Struggle (mash-input blend mechanic, indeterminate duration, exactly two forks), and Choice (N arbitrary options, each its own outbound edge) are meaningfully different in authoring, mechanics, and output topology. They are not one family. Conflating them produces a lowest-common-denominator node that cannot express the tuning parameters unique to each type.

**Root motion as a global-clock actor property** — world position is authored as a continuous spline on the global scene clock, not as per-node positional data. The exporter samples the spline at each node boundary to produce the anchor offsets the runtime expects. This is the correct architecture because: (a) root position must be continuous across node boundaries regardless of what beat or wait node is active; (b) manual stitching per node boundary is error-prone for multi-actor scenes; (c) the global clock is already defined by the sum of node durations. See §10.4.

**The scene graph is a behavior file; the timeline is an animation** — SCT exports two artifact types per scene: per-actor behavior HKX files encoding the scene graph topology as `hkbStateMachine` nodes, and per-actor animation HKX files encoding Beat timelines as clips. Non-animation timeline events (audio, camera, face data) are encoded as annotation track payloads on the animations. Engine Relay puts actors into a custom Havok user state (character controller and physics fully disabled) and injects per-actor behavior graphs dynamically via its reference relay. True Cinematics routes cross-actor sync events and dispatches annotation effects. Havok's state machine execution is the scheduler; no separate scene execution format is required. See §10.

**The camera is a creature** — A custom minimal creature (single bone, invisible, no AI, no physics, no NPC baggage) serves as the camera. Its root bone world transform is fed into Engine Relay's PlayerCamera API each frame — camera animation is bone animation, authored in SCT identically to any actor. Scene-level events (music, environment, weather) live as annotations on the camera creature's animation tracks. This eliminates the asymmetry between actor tracks and scene-level tracks: every event in a scene belongs to some actor's annotation track, and the camera creature owns the ones that belong to no one else. See §10.8.

---

## 0.3 UX/UI Design Philosophy
*(was §14.1)*

The metaphor is a **non-linear video editor where the footage is Havok animation and the screen is a 3D scene**. The primary reference is DaVinci Resolve's Edit page — a professional tool that puts the viewer and timeline at the center, with asset management on the left and properties on the right.

Three rules govern every decision:

**Actors are first-class, not tracks.** Unlike a video editor where tracks are anonymous lanes, SCT organizes the entire timeline around named actors from the Cast. Adding an actor to the scene is like casting a performer — the tracks follow from that actor's presence.

**The viewport is the authority.** All scrubbing, selection, and playback resolves in the viewport. The timeline is a *controller* for what the viewport shows — not a separate mode.

**Progressive disclosure.** The default layout shows only what a first-time user needs. Panels can be revealed, docked, and resized. Advanced panels (Bone Inspector, Curve Editor, Animator state machine) are accessible but not forced on screen.

---

# PART 1 — ARCHITECTURE FOUNDATION

*These sections define the structures every other system depends on. No feature work until this layer is solid.*

---

## 1.1 Architecture
*(was §2)*

### 1.1.1 Technology Stack
*(was §2.1)*

| Layer | Technology | Notes |
|---|---|---|
| UI / rendering | Dear ImGui + OpenGL 4.5 | ImGui DrawList for all current 3D projection |
| Math | GLM | Quaternions, mat4 VP matrix, vec3 bone positions |
| Window / input | GLFW | |
| XML parsing | pugixml | HKX XML intermediate |
| .NET bridge | DotNetHost → SctBridge.dll | In-process .NET 10 runtime; pattern for all managed libs |
| HKX conversion | HKX2E (via SctBridge) | HKX binary ↔ XML; skeleton + animation |
| NIF I/O | **nifly** (ousnius/nifly) | GPL-3.0; read/write NIF LE+SE; used by BodySlide/Outfit Studio; see Part 3 |
| TRI parsing | Custom thin parser | FRTRI003 format; read-only; ~200 lines; see Part 3 |
| Plugin I/O | **Mutagen** (via SctBridge) | MIT-licensed; read/write ESP/ESM/ESL; see Part 4 |
| Language | C++23 | MSVC, Windows only for now |
| Runtime foundation | **Engine Relay** (SKSE plugin) | Custom Havok user state (character controller + physics disabled); dynamic behavior injection via reference relay; PlayerCamera subclass with frame-by-frame transform API. See Part 6 |
| Runtime coordinator | **True Cinematics** (SKSE plugin) | Cross-actor sync via annotation event routing; scene lifecycle (spawn, cleanup); annotation dispatch (music, environment). See Part 6 |

### 1.1.2 Panel System
*(was §2.2)*

All UI panels implement `IPanel` (`src/ui/IPanel.h`):

```cpp
class IPanel {
    virtual void Draw(AppState& state) = 0;
    virtual const char* PanelID() const = 0;
};
```

Panels are **stateless draw functions** — all persistent data lives in `AppState`. Panel instances are owned by value in `MainLayout` and registered (as non-owning pointers) into `TabRegistry`.

**`TabRegistry`** — singleton mapping `AppTab` enum → `AppTabDef { label, vector<IPanel*> }`.  
**`TrackRegistry`** — singleton of `TrackTypeDef` structs with evaluate callbacks. Register once at startup; new track types require no changes to existing code.

Current panels:

| Panel | Class | Status |
|---|---|---|
| Bin (Clips + Cast tabs) | `BinPanel` | ✅ Working |
| Viewport | `ViewportPanel` | ✅ Working |
| Timeline | `TimelinePanel` | ✅ Working |
| Scene Graph | `SceneGraphPanel` | 🔲 Stub |
| Animator | `AnimatorPanel` | 🔲 Stub |
| NIF Editor | `NifEditorPanel` | 🔲 Stub |

### 1.1.3 AppState
*(was §2.3)*

Single source of truth passed by reference to every `IPanel::Draw` call.

```
AppState
  clips[]          AnimClip  — imported animation clips (name, skeletonType, frames)
  skeletons[]      Skeleton  — loaded HKX skeletons (bone hierarchy + ref pose)
  cast[]           CastEntry — proto-NPC definitions (name, editorId, skeletonType, skeletonIndex)
  actors[]         Actor     — scene instances (castIndex)
  sequence         Sequence  — the NLE document
  dataFolder       string    — path to game Data directory (persisted → sct_settings.ini)
  discoveredSkeletons[]      — populated by ScanDataFolder()
  time, playing, loop        — playback state
  activeTab, selectedClip    — UI state
```

### 1.1.4 Sequence / NLE Model
*(was §2.4)*

```
Sequence
  actorTracks[]    ActorTrackGroup  — one group per actor
    actorIndex
    collapsed
    lanes[]        TrackLane        — one lane per registered per-actor track type
      type         TrackType        — AnimClip | FaceData | LookAt | Camera | Audio
      items[]      SequenceItem     — { assetIndex, seqStart, trimIn, trimOut, blendIn, blendOut }
  sceneTracks[]    TrackLane        — scene-level tracks (Camera, Audio)
```

`Sequence::Evaluate(t, state, out[])` — iterates actor groups → lanes → calls `TrackRegistry::evaluate` per lane → writes `ActorEval { Pose }` per actor.

Multi-item overlap on the same lane uses per-channel lerp/slerp blend weighted by `blendIn`/`blendOut` ramps.

### 1.1.5 Viewport
*(was §2.5)*

- Orbit/pan/zoom camera with GLM `View()` + `Proj()` matrices
- Grid with axis lines (X=red, Z=blue) at adaptive unit scale
- All 3D rendering done via ImGui `ImDrawList` — CPU projection of world positions to screen coordinates
- Multi-actor support: each actor renders its own posed skeleton independently
- `ViewportPanel` is construction-injected with a `vector<ViewportTabDef>` so the same class serves both the Scene Editor (Scene/Face/Cameras tabs) and Anim Editor (Scene/Bones tabs)

### 1.1.6 Skeleton Type System
*(was §2.6)*

Both `AnimClip` and `CastEntry` carry a `skeletonType` string (e.g. `"character"`, `"horse"`, `"draugr"`) extracted from the `actors/<type>/` path segment at load/import time via `ExtractCreatureType()`.

- Clips bin groups clips under `SeparatorText` headers per creature type
- Drag-drop from Clips → Timeline shows blue (compatible), orange (wrong type), or red (wrong lane) feedback
- Drop is hard-rejected on skeleton type mismatch

---

## 1.2 File Layout (src/)
*(was §13)*

```
src/
  AnimClip.h/cpp          Animation clip data + Evaluate()
  HavokAnimation.h/cpp    HKX/XML animation parser
  HavokSkeleton.h/cpp     HKX/XML skeleton parser
  Pose.h/cpp              Bone pose (local channels + world positions), SolveFK()
  Skeleton.h              Bone hierarchy definition
  AppState.h/cpp          Shared application state, ScanDataFolder(), settings I/O
  CastEntry.h             Proto-NPC definition (name, editorId, skeletonType, skeletonIndex)
  Sequence.h/cpp          NLE document model + Evaluate()
  SceneState.h            Redirect shim → AppState.h
  DotNetHost.h/cpp        .NET runtime host + SctBridge API declarations
  ui/
    IPanel.h              Abstract panel interface
    TabRegistry.h/cpp     AppTab → AppTabDef mapping
    TrackRegistry.h/cpp   TrackType → TrackTypeDef + evaluate callbacks
    MainLayout.h/cpp      Owns all panels, registers tabs/tracks, draws host window
    panels/
      ClipBinPanel.h/cpp  Bin panel: Clips tab + Cast tab + skeleton picker modal
      ViewportPanel.h/cpp 3D viewport with multi-actor skeleton rendering
      TimelinePanel.h/cpp NLE timeline: ruler, lanes, scrubber, drag-drop
      SceneGraphPanel.h/cpp  Stub
      AnimatorPanel.h/cpp    Stub
      NifEditorPanel.h/cpp   Stub
```

---

## 1.3 Visual Language
*(was §14.10)*

Define the color system before building any UI — panels must agree on shared constants from day one.

### Color System

**Track type colors:**

| Track | Color | Hex |
|---|---|---|
| AnimClip | Steel blue | `#3866A6` |
| FaceData | Violet | `#8C4DA6` |
| LookAt | Amber | `#A68C33` |
| Camera | Teal | `#33A68C` |
| Audio | Burnt orange | `#A6622E` |

**State colors:**

| State | Color | Use |
|---|---|---|
| OK / active | `#4CAF50` green | Data folder connected, clip valid |
| Warning | `#FFA726` amber | Unsaved changes, muted lane |
| Error | `#EF5350` red | Import failure, skeleton mismatch |
| Disabled | `#555555` gray | Not-yet-implemented features |
| Selected | `#FFFFFF` border | Any selected element |

**Skeleton type badge colors** (used in Cast tab):

| Type | Color |
|---|---|
| character | `#5E9AD1` |
| horse | `#9C7E52` |
| dragon | `#C0392B` |
| (others) | hashed from string |

### Typography

Embed **Cousine Regular** (monospace, SIL license) for time codes and numeric fields. **Roboto Regular** for body labels. Both compact enough for high-density UI at 13–14px. Current ImGui default (ProggyClean) acceptable through Phase 2.

---

# PART 2 — ANIMATION CORE

*Phase 1 ✅ Complete / Phase 2 🔄 In Progress. Asset discovery, Havok pipeline, and the NLE shell. UI specs for every panel that currently exists or is under active development.*

---

## 2.1 Data Folder & Asset Discovery
*(was §3)*

`AppState::ScanDataFolder()` walks `{dataFolder}/meshes/actors/` recursively for `skeleton*.hkx` files, extracts creature type, and populates `discoveredSkeletons[]`. Sorted by creature type + display name.

Settings persisted to `sct_settings.ini` in the working directory:
```
dataFolder=C:\...\Skyrim Special Edition\Data
```

When discovered skeletons exist, "Add Actor" opens a filtered skeleton picker modal (grouped by creature type, tooltip shows full path, double-click to confirm) instead of a raw file dialog.

---

## 2.2 Havok / Animation Pipeline
*(was §4)*

### 2.2.1 File Loading
*(was §4.1)*

```
.hkx  →  DotNetHost → SctBridge.HkxToXml()  →  XML buffer  →  LoadHavokSkeletonXmlFromBuffer()
.xml  →  LoadHavokSkeletonXml() directly
```

### 2.2.2 Skeleton Representation
*(was §4.2)*

`Skeleton::bones[]` — name, parent index, local transform (pos, rot, scale). `MakeReferencePose()` produces a `Pose` with local channels set from the reference.

`Pose::SolveFK()` — walks bone hierarchy parent-first, computes `worldPos[]` and `worldRot[]` from accumulated local transforms. Used for both reference pose display and animated pose display.

### 2.2.3 Animation Evaluation
*(was §4.3)*

`AnimClip::Evaluate(t, pose)` — floor/ceil frame index, per-channel lerp (translation) + slerp (rotation). Writes only the bones the clip covers (`trackToBone[]` mapping). Caller pre-fills pose from reference before calling.

Bone binding: HKX tracks are matched to skeleton bones by name at load time. Unmatched tracks are silently discarded. This means creature support is automatic — no special casing.

---

## 2.3 Application Shell
*(was §14.2)*

```
┌─ Title Bar: "Skyrim Content Tools — [scene.sct]  ●" ───────────────┐
│ File  Edit  Scene  View  Help                                        │
├─ [Scene Editor]  [Anim Editor] ──────────────────────── [Settings] ─┤
│                                                                      │
│           ... docked panels fill this space ...                      │
│                                                                      │
├─ ● Guard01 · idle_lookidle_se   00:00:04:12 / 30fps   Data: SE ────┤
└──────────────────────────────────────────────────────────────────────┘
```

- The title bar shows a dirty-state dot `●` when the scene has unsaved changes.
- The app tab bar (Scene Editor / Anim Editor) is immediately below the menu bar — it is the root-level tab, not an ImGui tab inside a panel. Switching tabs changes the entire panel arrangement.
- The status bar carries four zones (§2.8).

---

## 2.4 Layout Regions
*(was §14.3)*

SCT defines five named layout regions per tab, borrowing DaVinci Resolve's terminology:

| Region | Resolve Equivalent | SCT Role |
|---|---|---|
| **Media Pool** | Media Pool | Clip Bin + Cast Roster |
| **Viewer** | Viewer | 3D Viewport |
| **Inspector** | Inspector | Context-sensitive properties |
| **Timeline** | Edit Timeline | NLE track lanes |
| **Console** | (no equivalent) | Log / import feedback |

#### Scene Editor Default Layout

```
┌────────────────────────────────────────────────────────────────────────────────┐
│  MEDIA POOL (22%)  │               VIEWER (56%)              │ INSPECTOR (22%) │
│                    │                                          │                 │
│  [Clips|Cast|Env]  │    3D scene — actors, skeleton, grid    │  (selection-    │
│                    │                                          │  sensitive)     │
│                    │  [Scene ▾] [Face] [Cameras]  [●] [⟳]   │                 │
│                    │                                          │                 │
│                    │                       [Bones][Wire][Tex] │                 │
├────────────────────┴──────────────────────────────────────────┴─────────────────┤
│  TIMELINE (full width, ~35% of total height)                                    │
│  [◀◀] [◀] [▶] [▶▶]  00:00:04:12  30fps  ↻  ──────────────────────  [─┤├─] [⊠]│
│  ─ ─ ─ ─ ─ ─ ─ Ruler ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│
│  ▼ Guard01          │  [idle_lookidle_se ────────][combat_idle───]    │          │
│    AnimClip         │  ████████████████████████████████████████       │          │
│    FaceData         │  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░       │          │
│    LookAt           │                                                  │          │
│  ▼ Lydia            │  [walk_forward──────────────────]                │          │
│    AnimClip         │  ████████████████████                            │          │
│  ─── Scene ─────────┤                                                  │          │
│    Camera           │  [cam_dolly_01─────────────────────────────────] │          │
│    Audio            │                                                  │          │
└─────────────────────────────────────────────────────────────────────────────────┘
```

The 22/56/22 column ratio is the default, not a lock — all three columns resize by dragging the splitter.

#### Anim Editor Default Layout

```
┌────────────────────────────────────────────────────────────────────────────────┐
│  CLIP BIN (18%)  │ BONE TREE (18%) │        VIEWER (64%)                       │
│                  │                 │                                            │
│  [clips list]    │  ▼ Root         │   bone-gizmo skeleton — single actor      │
│                  │    ▶ Pelvis     │                                            │
│  ┄┄ Bone Insp. ┄┄│    ▶ Spine1     │      [Bones ▾] [Scene]                    │
│  [selected bone] │    ▶ Spine2     │                                            │
│  T: x y z        │    ▼ Neck       │                                            │
│  R: x y z        │      Head       │                                            │
│  [Key] [KeyAll]  │    ▶ L Clavicle │                                            │
│                  │    ▶ R Clavicle │                                            │
├──────────────────┴─────────────────┴────────────────────────────────────────────┤
│  DOPESHEET / CURVE EDITOR                  [Dopesheet ▾] [Curves]  [Filter]    │
│  Ruler  |0    |5    |10   |15   |20   |25   |30   ...                          │
│  Root   |                                                                       │
│  ▼ Spine|  ◆     ◆           ◆                                                 │
│    T.x  |  ◆     ◆           ◆                                                 │
│    T.y  |  ◆     ◆           ◆                                                 │
│    R.x  |        ◆           ◆                                                 │
│  ▼ Head |        ◆                 ◆                                           │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2.5 Media Pool
*(was §14.5)*

Three tabs. The active tab does not change the viewport — it only changes what's in the pool.

### Clips Tab

```
┌── Clips ────────────────────────────────┐
│ [⊕ Import] [⟳ Rescan]   🔍 [filter   ] │
├─────────────────────────────────────────┤
│ ── character ──────────────────────── │
│   idle_lookidle_se        3.96s  119f   │
│   mt_idle                 4.20s  126f   │
│   combat_idle             2.80s   84f   │
│ ── horse ───────────────────────────── │
│   horse_idle              2.10s   63f   │
└─────────────────────────────────────────┘
```

Columns: name | duration | frame count. Skeleton type shown only as a group header — it's implicit.

Single-click selects and populates Inspector with clip metadata. Double-click previews in viewport (plays on a ghost skeleton, no scene actor required).

**Drag:** `SCT_CLIP` payload (int clipIndex). On hover over an incompatible lane: red tint + tooltip "Skeleton type mismatch — expected 'character', got 'horse'."

**Right-click:** Open in Anim Editor, Rename, Duplicate, Delete from bin, Show in Explorer.

### Cast Tab

```
┌── Cast ─────────────────────────────────┐
│ [+ New Actor]  [⇣ Import from ESP]      │
├─────────────────────────────────────────┤
│ ● Guard01      character    in scene    │
│ ○ Lydia        character    in scene    │
│ ○ Shadowmere   horse        not placed  │
└─────────────────────────────────────────┘
```

`●` = actor placed in the current scene. `○` = in cast but not placed.

Double-click opens Cast Properties modal (name, EditorID, skeleton assignment, tint, etc.).

**Drag:** `SCT_CAST` payload — drag to viewport to place actor at a world position, or drag to timeline header area to create a new actor track group.

**Right-click:** Edit Properties, Add to Scene / Remove from Scene, Duplicate Entry, Remove from Cast.

### Assets Tab (Phase 3+)

Grayed out label until NIF rendering ships. When available: environment NIFs, prop assets, terrain chunks. Drag to viewport to place.

---

## 2.6 Timeline
*(was §14.6)*

### Transport Bar

```
[◀|] [◀◀] [◀] [▶] [▶▶] [▶|]   ↻   00:00:04:12   [30fps ▾]   ─────○─── [⊠fit]
```

- `◀|` = Go to start (Home)
- `◀◀` = Step back 1 frame (`,`)
- `◀` = Rewind (J)
- `▶` / `⏸` = Play / Pause (Space)
- `▶▶` = Fast forward (L)
- `▶|` = Go to end (End)
- `↻` = Loop toggle (highlighted when active)
- Time display: `00:00:04:12` — click to type an exact time, format HH:MM:SS:FF
- FPS dropdown: 30 (default), 24, 60, game-native
- Zoom slider: drag to zoom timeline horizontally
- `⊠ fit` = fit all clips in view

### Ruler and Playhead

`kRulerH = 20px` strip with adaptive ticks: major ticks at second intervals with labels at low zoom, frame-level ticks at high zoom. The playhead is a **red vertical line** the full height of the track area with a small downward triangle handle in the ruler.

- Click ruler = snap playhead
- Drag playhead = scrub

### Track Area

**Track header column** (`headerW_ = 150px`, resizable):

Actor group row (h=22px, collapsible):
```
[▼] Guard01                           [+] [⋮]
```
- `[+]` = add optional lane (LookAt, FaceData) if not already present
- `[⋮]` = group context menu

Lane row (h=26px per lane):
```
   [●] AnimClip   [M] [S] [🔒]
```
- `[M]` = mute lane (clips remain at position, contribute 0 weight)
- `[S]` = solo lane (all others muted)
- `[🔒]` = lock lane (no edits)

Scene tracks separator:
```
── Scene ─────────────────────────────────────────────────────────────
```

### Clip Blocks

Each clip renders as a colored rectangle with:
- Background fill at ~80% saturation of track type color
- 2px top border at full saturation
- Name text left-aligned, 4px padding, clipped to block width
- Duration badge right-aligned inside if wide enough
- **Blend ramps**: if `blendIn > 0`, left edge has a diagonal gradient fade from transparent to full; same for `blendOut` on right
- **Overlap zone**: when two clips overlap, a hatched diagonal pattern covers the overlap region, making blending tangible

Selected clip: 1px white outline border.

**Drag states:**
- Body drag: translucent ghost at original position, solid block follows cursor; lane highlight green (ok) or red (skeleton mismatch)
- Edge drag: resize cursor, live trim update, tooltip showing new value

**Right-click menu:** Delete, Duplicate (Ctrl+D), separator, Set Blend In..., Set Blend Out..., Clip Properties..., separator, Open in Anim Editor (AnimClip lanes only).

### Multi-Clip Blending Visualization

When clips overlap: the overlap region gets diagonal hatching in both clips' colors. Tooltip on hover: "Blend: 0.8s cross-fade."

---

## 2.7 Status Bar
*(was §14.8)*

```
│ ● scene.sct    │  Guard01 · idle_lookidle_se  │  00:04:12  30fps  │  SE · Data OK  │
```

- **Left zone:** scene name + dirty dot; click to Save
- **Selection zone:** breadcrumb of current selection — actor · clip · track
- **Time zone:** current time + actual playback FPS
- **Data zone:** current edition (LE/SE auto-detected) + data folder status; red if not set; click to open settings

Import errors appear as **toast notifications** floating above the status bar, fading after 5 seconds. Also written to the Console panel (dockable, hidden by default).

---

## 2.8 Keyboard Shortcuts
*(was §14.9)*

### Playback

| Key | Action |
|---|---|
| `Space` | Play / Pause |
| `K` | Stop (return to frame 0) |
| `J` | Rewind (hold = slow reverse) |
| `L` | Fast forward (hold = slow forward) |
| `,` | Step back 1 frame |
| `.` | Step forward 1 frame |
| `[` | Jump to previous clip on active lane |
| `]` | Jump to next clip on active lane |
| `Home` | Go to scene start |
| `End` | Go to scene end |

### Navigation

| Key | Action |
|---|---|
| `1` | Scene Editor tab |
| `2` | Anim Editor tab |
| `Tab` | Cycle viewport mode (Scene → Face → Cameras) |
| `F` | Frame selected in viewport |
| `A` | Frame all actors in viewport |
| `Numpad 1/3/7` | Front / Right / Top orthographic |
| `Numpad 5` | Toggle perspective / ortho |

### Editing

| Key | Action |
|---|---|
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` | Redo |
| `Ctrl+D` | Duplicate selected clip |
| `Delete` | Delete selected clip / keyframe |
| `Ctrl+A` | Select all on current lane |
| `Escape` | Deselect all |
| `I` | Insert keyframe (Anim Editor) |
| `Ctrl+N` | New Scene |
| `Ctrl+O` | Open Scene |
| `Ctrl+S` | Save Scene |
| `Ctrl+Shift+S` | Save As |

---

## 2.9 Drag & Drop Contract
*(was §14.11)*

| Payload ID | Type | Source | Valid Drop Targets |
|---|---|---|---|
| `SCT_CLIP` | `int` clipIndex | Clips tab, Anim Editor | Timeline AnimClip lane (skeleton match required) |
| `SCT_CAST` | `int` castIndex | Cast tab | Viewport (place actor), Timeline header (create actor group) |
| `SCT_BONE` | `int` boneIndex | Bone Tree | LookAt constraint target slot (future) |
| `SCT_ASSET` | `int` assetIndex | Assets tab | Viewport (place NIF), future |

Skeleton type validation on `SCT_CLIP` drop: on hover over incompatible lane, show red highlight + tooltip. On drop, silently reject — no modal.

---

## 2.10 Missing Components — Priority Order
*(was §14.13)*

Components absent from the current implementation, in priority order:

1. **Inspector Panel** — `InspectorPanel : IPanel` reading a `Selection` struct from AppState. Without it users cannot see or change any properties.

2. **Selection Model** — AppState needs a `Selection` struct: `{ SelectionType type; int actorIdx; int clipIdx; int boneIdx; }`. All panels read this; clicks write to it.

3. **Undo / Redo** — Command pattern stack. Every mutation to AppState goes through a reversible Command. Without this the tool is unusable for production.

4. **Scene serialization** — `.sct` JSON format: scene metadata, cast array (name, editorID, skeletonPath), actor placements (castIdx, worldXform), sequence (track groups, lane items with assetIdx/timing/blend). Enables File > Save and File > Open.

5. **Toast notification system** — a queue of timed messages rendering as floating overlays above the status bar, replacing the `importErr[256]` buffer pattern.

6. **Progress indicator** — HKX loading via DotNetHost can take 200–500ms. An indeterminate spinner in the status bar ("Loading skeleton...") prevents the app from appearing frozen.

---

# PART 3 — NIF MESH RENDERING

*Phase 3. NIF format knowledge must come before the rendering pipeline. The FaceGen morph system lives here because it depends on BSDynamicTriShape and the compiled head NIF structure.*

---

## 3.1 The HKX / NIF Dual-Skeleton Architecture
*(was §5)*

**This is foundational to understanding how character rendering will work.**

Skyrim runs two parallel skeletal systems:

| System | File | Purpose |
|---|---|---|
| Havok animation skeleton | `skeleton.hkx` | Drives all animation, IK, physics constraints. What Havok Behavior Tool operates on. |
| NIF rendering skeleton | `NiNode` hierarchy in body/armor NIFs | Scene graph nodes the renderer skins meshes against. Contains no animation data. |

**The runtime connection** is a per-frame name-lookup transform copy: after Havok evaluates the pose, the Creation Engine writes each bone's world transform to the `NiNode` with the matching name in the equipped NIFs.

This is **not retargeting** and not scene-graph parenting. It is a name-matched synchronisation pass.

The `skeleton.nif` files in the data folder are CK/tooling artifacts, not required by the game.

**Consequence for SCT**: we already have the right skeleton from the HKX. NIF mesh rendering requires:
1. Parse `BSTriShape` → vertices, triangle indices
2. Parse `BSSkin::Instance` → bone name list
3. Parse `BSSkin::BoneData` → per-bone bind-pose inverse transforms + vertex weights
4. At render time: `skinMatrix[bone] = worldTransform[bone] * bindPoseInverse[bone]`
5. Apply per-vertex weighted sum of skinMatrices to transform vertices

The NIF's own `NiNode` hierarchy is irrelevant to SCT's pipeline. We drive everything from the Havok side.

### 3.1.1 BSBehaviorGraphExtraData — The Invisible Import Bridge
*(was §5.1)*

Most body, armor, and creature NIFs carry a `BSBehaviorGraphExtraData` block on the root node containing a string like `Actors\Horse\Behaviors\DefaultHorse.hkx`.

From this path, the skeleton can be derived deterministically:
```
Actors\Horse\Behaviors\DefaultHorse.hkx
  → actor dir: Actors\Horse\
    → skeleton: Actors\Horse\Character Assets\skeleton.hkx
```

This means SCT can accept a NIF drop and automatically resolve the correct skeleton without any user interaction — the NIF already carries the address.

### 3.1.2 NIF Binary Format & Version Fingerprinting
*(was §5.2)*

All Skyrim NIFs share version `0x14020007` (20.2.0.7). The discriminator that matters is the **stream version** (User Version 2 / BS Version) in the header:

| Game | Stream Version | Geometry block |
|---|---|---|
| Skyrim LE | **83** | `NiTriShape` + `NiTriShapeData` (two separate blocks; full float32 verts) |
| Skyrim SE / AE | **100** | `BSTriShape` (data inline; half-float compressed verts) |

nifly exposes this as `NifFile::GetHeader().GetVersion().IsSK()` / `IsSSE()`.

**`BSTriShape` (SE)** key fields: `VertexDesc` (uint64 bitfield encoding which vertex attributes are present and their sizes), `numVertices` (uint16), `numTriangles` (uint16), then inline `BSVertexData[]` and `Triangle[]`. The `VertexDesc` flags (`VF_VERTEX`, `VF_UV`, `VF_NORMAL`, `VF_SKINNED`, `VF_FULLPREC`, etc.) determine the per-vertex byte layout.

**`BSDynamicTriShape`** extends `BSTriShape` with a `dynamicData` `Vector4[]` — the CPU-side mutable vertex buffer the expression morph system writes into each frame. Compiled NPC head NIFs use this type; plain body/armor NIFs use `BSTriShape`.

### 3.1.3 Mesh Skinning — BSSkin::Instance
*(was §5.3)*

Body and armor mesh NIFs bind to the skeleton via `BSSkin::Instance` (SE) or `NiSkinInstance` (LE) attached to each geometry block:

```
BSSkin::Instance
  targetRef        → skeleton root NiNode
  dataRef          → BSSkinBoneData  (per-bone inverse bind transforms)
  boneRefs[]       → NiNode objects within the mesh NIF (bind-pose only, static)
```

At load time the engine name-matches the mesh NIF's `boneRefs` NiNode names against the live skeleton NIF's NiNode names. The mesh NIF's NiNode objects provide only the inverse bind matrices; all live transforms come from the Havok-driven skeleton NIF.

**Consequence for SCT**: nifly gives us `boneRefs` names + `BSSkinBoneData` inverse bind matrices. We already have live bone world transforms from the Havok pose system (§2.2). The skinning math is a standard linear blend skinning sum — no skeleton NIF required at runtime.

---

## 3.2 FaceGen & Expression Morph Pipeline
*(was §5.4)*

### The Two-Layer Architecture

```
Base head NIF vertices
  + race morph deltas          (baked at CK export — NAM0=0 TRI)
  + chargen morph deltas × weights  (baked at CK export — NAM0=2 TRI × NAM9 floats)
= BSFaceGenBaseMorphExtraData.vertexData  ← what the compiled facegeom NIF contains
  + expression TRI deltas × per-morph float weights  (per-frame at runtime)
= final vertex positions uploaded to GPU
```

**SCT only operates on the second layer.** We read the baked geometry from the compiled NIF and apply expression morph deltas at runtime. No CK-style baking is required or planned.

### Compiled NPC Head NIF Structure

Location: `Meshes\Actors\Character\FaceGenData\FaceGeom\[Plugin.esp]\[FormID_hex8].nif`

```
BSFaceGenNiNode  (root — Bethesda extension of NiNode)
  ├── BSDynamicTriShape  "FaceGeom"      — main face; baked chargen verts
  │     ├── BSLightingShaderProperty     — shader type kFaceGen; uses facetint DDS
  │     └── BSFaceGenBaseMorphExtraData  — base vertex positions for runtime blending
  ├── BSDynamicTriShape  "EyesHeadparts"
  ├── BSDynamicTriShape  "MouthHeadparts"
  └── BSDynamicTriShape  "BrowHeadparts"
```

Each part is a **separate** `BSDynamicTriShape` node — they are never merged. `BSDynamicTriShape` is required (not plain `BSTriShape`) because the engine needs a per-frame writable CPU vertex buffer.

`BSFaceGenBaseMorphExtraData` (a `NiExtraData` subtype attached to each part) stores the baked vertex positions as the base for expression delta blending. This is what nifly exposes as the authoritative vertex data for a compiled head NIF.

### The TRI File Format — FRTRI003

`.tri` files are **not NIFs**. They are FaceGen SDK binary format with magic bytes `FRTRI003`. A NIF parser will reject them immediately.

```
Header (64 bytes):
  [0]  "FRTRI003"       — magic
  [8]  vertexNum        int32   — must match paired NIF vertex count exactly
  [12] faceNum          int32
  [28] extension_flags  int32   — bit 0 = has UVs
  [32] morphNum         int32   — number of difference (delta) morphs
  [36] addMorphNum      int32   — number of modifier (absolute) morphs

Data layout (sequential after header):
  float[vertexNum][3]             — base vertex positions (race-neutral; not used at runtime)
  uint32[faceNum][3]              — triangle indices
  [UV data if flags & 1]

Per difference morph (morphNum entries):
  int32    nameLen               — includes null terminator
  char[]   name                  — e.g. "Aah", "BlinkLeft", "MoodHappy"
  float    baseDiff              — scale factor = maxDelta / 32767
  int16[vertexNum][3]            — signed delta per vertex per axis

Reconstruct: final_pos = base_pos + (delta_i16 × baseDiff)
```

**Vertex count lock**: `vertexNum` in the TRI must exactly equal the vertex count in the paired NIF. Mismatch causes a CTD. The topology never changes between chargen morphs — only positions move — so the compiled NPC NIF and the race-neutral expression TRI always share vertex count.

### Expression Morph Naming — ARKit Standard (52 shapes)

SCT uses **ARKit blend shape names** as morph identifiers, not Skyrim's legacy 56-name phoneme/modifier/mood set. TRI files must be authored with these exact camelCase names for the FaceData lane to function. Morphs are matched by name string, not by index.

| Group | Count | Names |
|---|---|---|
| Brows | 5 | browDownLeft, browDownRight, browInnerUp, browOuterUpLeft, browOuterUpRight |
| Eyes | 14 | eyeBlinkLeft/Right, eyeLookDownLeft/Right, eyeLookInLeft/Right, eyeLookOutLeft/Right, eyeLookUpLeft/Right, eyeSquintLeft/Right, eyeWideLeft/Right |
| Cheeks & Nose | 5 | cheekPuff, cheekSquintLeft/Right, noseSneerLeft/Right |
| Jaw | 4 | jawForward, jawLeft, jawRight, jawOpen |
| Mouth | 23 | mouthClose, mouthFunnel, mouthPucker, mouthLeft, mouthRight, mouthSmileLeft/Right, mouthFrownLeft/Right, mouthDimpleLeft/Right, mouthStretchLeft/Right, mouthRollLower, mouthRollUpper, mouthShrugLower, mouthShrugUpper, mouthPressLeft/Right, mouthLowerDownLeft/Right, mouthUpperUpLeft/Right |
| Tongue | 1 | tongueOut |

ARKit morphs are anatomical — they describe physical face muscle state rather than expressive intent. Any expression or phoneme pose is composable from these components. A TRI file built with legacy Skyrim naming will load without error but all morph weights will silently stay at zero because no names match. See §4.2 for the import validation spec.

### SCT Expression Morph System Design

The runtime blend loop SCT replicates:

```cpp
// per-frame, per BSDynamicTriShape part
for (int i = 0; i < vertexCount; i++) {
    Vector3 pos = baseMorphData[i];           // from BSFaceGenBaseMorphExtraData
    for (int m = 0; m < morphCount; m++) {
        if (weights[m] == 0.0f) continue;
        pos += deltas[m][i] * weights[m];     // delta = int16 × baseDiff, pre-converted at load
    }
    dynamicVerts[i] = pos;                    // written to GPU vertex buffer
}
```

`weights[]` is a 56-element float array (0.0–1.0) driven by the animation system. SCT exposes this as `FaceMorphWeights` in `AppState` per actor, writeable from the FaceData track on the timeline.

### Two TRI Files Per Head

| File | Example | When used |
|---|---|---|
| `actor.tri` | `FemaleHead.tri` | Expression morphs — loaded at runtime, shared across all actors of same race head |
| `actorchargen.tri` | `FemaleHeadNordChargen.tri` | Chargen slider morphs — used by CK at export; **not needed by SCT** |

---

## 3.3 Planned Rendering Pipeline
*(was §6)*

Current viewport rendering is ImGui `ImDrawList` (CPU-projected 2D lines and circles). Full mesh rendering requires GPU involvement.

**Planned approach: render-to-texture**

Render skinned meshes offscreen via OpenGL (already available in the stack) into a framebuffer texture. Composite into the viewport via `ImGui::Image()`. This keeps the ImGui layout system intact while enabling real 3D rendering.

Phased rollout:
1. Static (unskinned) NIF geometry in the render texture — validates the pipeline
2. Skinned mesh rendering using bone matrices from the existing Havok pose system
3. Basic PBR lighting (point lights + directional) to match game appearance
4. Terrain heightfield (see Part 5)

---

## 3.4 Viewer / 3D Viewport
*(was §14.4)*

### Viewport Modes

**Scene Editor** — compact tab strip in the viewport's top-left corner:

| Mode | Shortcut | Description |
|---|---|---|
| **Scene** | `1` | Full scene — all actors, grid, optional mesh/terrain |
| **Face** | `2` | Isolated face — single selected actor fills viewport, morph sliders appear in Inspector |
| **Cameras** | `3` | Camera placement — frustum visualizations, camera path spline |

**Anim Editor**:

| Mode | Shortcut | Description |
|---|---|---|
| **Bones** | `1` | Skeleton with selectable, highlight-able bones |
| **Scene** | `2` | Full scene context (read-only) |

### Render Mode Strip

Bottom-right corner, horizontal icon strip (radio buttons):

```
[Bones ◉]  [Wire □]  [Solid ▣]  [Textured ▤]
```

Wire, Solid, and Textured are grayed out until NIF rendering ships (Phase 3). Hovering shows: "Requires mesh import (Phase 3)."

### Viewport Overlays

- **Axis indicator** — bottom-left corner, 3-axis gizmo (Blender-style) — always visible
- **Playhead time** — top-center, compact: `00:04:12 / 30fps` — fades out after 3s mouse idle
- **Actor name labels** — floating above each actor's root bone, fade at distance
- **Playback badge** — top-right: `▶ PLAYING` (green), `⏸ PAUSED` (gray), `● REC` (red, future)
- **Selection box** — marching-ant dashed rectangle during box-select

### Camera Controls

| Input | Action |
|---|---|
| `MMB drag` | Orbit |
| `Shift+MMB drag` | Pan |
| `Scroll` | Zoom |
| `F` | Frame selected actor |
| `A` | Frame all actors |
| `Numpad 1` | Front orthographic |
| `Numpad 3` | Right orthographic |
| `Numpad 7` | Top orthographic |
| `Numpad 5` | Toggle perspective / orthographic |
| `Numpad 0` | Jump to active camera (Cameras mode) |
| `LMB click` | Select actor (or bone in Anim Editor) |
| `Shift+LMB` | Additive select |
| `Escape` | Deselect all |

### Face Mode

When Face mode is active:
- The selected actor's head fills the viewport (auto-framed to head region).
- The right Inspector panel replaces standard object properties with the Morph Sliders layout (§4.2.4).
- A mini scrubber appears at the bottom of the viewport — playhead and time code only, no lanes.
- Clicking another actor in the Cast tab switches which actor is being previewed.

---

# PART 4 — PLUGIN INTEGRATION

*Phase 4. Mutagen enables NPC character import and plugin export. The Inspector Panel becomes fully functional here — its face morph sliders depend on the NIF rendering from Part 3.*

---

## 4.1 Plugin Integration — Mutagen
*(was §7)*

**Decision: Mutagen over xeditlib.**

xeditlib is GPL v3. GPL copyleft is incompatible with a source-available license that has additional restrictions. The process-boundary argument for dynamic linking is disputed and not worth relying on.

Mutagen is MIT-licensed. It is the library underlying Spriggit and most of the modern Bethesda tooling ecosystem. It provides strongly-typed read/write access to all ESP/ESM/ESL record types for Skyrim SE.

**Integration pattern**: add to existing `SctBridge.dll` — same DotNetHost mechanism already used for HKX conversion. No new integration infrastructure required.

What Mutagen enables:

| Use case | Records needed |
|---|---|
| NPC → Race → skeleton path | `NPC_` → `RACE` → behavior/skeleton SNAM |
| Race → body mesh paths | `RACE` → `ARMO` (skin) → `ARMA` → MODL |
| Facegen NIF path | `{dataFolder}/meshes/actors/character/facegendata/facegeom/{plugin}/{FormID}.nif` |
| NPC face morph values | `NPC_` → `DNAM` subrecord |
| Placed object locations | `REFR` in `CELL`/`WRLD` → DATA (position/rotation) + base object MODL |
| Terrain data | `LAND` in `CELL` → VHGT (heightfield), VCLR (vertex colors), ATXT/VTXT (texture layers) |
| New NPC export | Write `NPC_`, `RACE` refs, facegen, inventory |

---

## 4.2 Inspector Panel
*(was §14.7)*

Context-sensitive — displays different content based on the current selection. **This panel does not currently exist and is the highest-priority missing UI component.**

### 4.2.1 Nothing Selected — Scene Properties

```
┌── Scene ───────────────────────────────┐
│ Name:      [untitled scene        ]    │
│ Duration:  [00:00:30:00           ]    │
│ FPS:       [30fps ▾              ]    │
│ Data Dir:  F:\...\SE\Data  [Browse]    │
│ Export To: (not set)           [Set]   │
├────────────────────────────────────────┤
│ Actors in scene:   2                   │
│ Clips in bin:      7                   │
│ Total duration:    30.0s               │
└────────────────────────────────────────┘
```

### 4.2.2 Actor Selected

```
┌── Actor: Guard01 ──────────────────────┐
│ Name:      [Guard01                ]   │
│ EditorID:  [SkyrimGuard01          ]   │
│ Skeleton:  character  [Change...]      │
│                                        │
│ ─ Transform ──────────────────────    │
│ Position   X [0.00] Y [0.00] Z [0.00] │
│ Rotation   X [0.00] Y [0.00] Z [0.00] │
│ Scale      [1.00]                      │
│                                        │
│ [Frame in Viewport] [Remove from Scene]│
└────────────────────────────────────────┘
```

### 4.2.3 Clip Selected (in timeline)

```
┌── Clip: idle_lookidle_se ──────────────┐
│ Asset:   idle_lookidle_se  [→ Bin]     │
│ Track:   ● AnimClip                    │
│ Actor:   Guard01                       │
├────────────────────────────────────────┤
│ Start:       [00:00:00:00          ]   │
│ Duration:    [00:00:03:29          ]   │
│ ─ Trimming ────────────────────────   │
│ Trim In:     [00:00:00:00          ]   │
│ Trim Out:    [00:00:03:29          ]   │
│ ─ Blending ────────────────────────   │
│ Blend In:    [0.50s]  [Linear ▾    ]  │
│ Blend Out:   [0.50s]  [Linear ▾    ]  │
│ ─ Playback ────────────────────────   │
│ Speed:       [1.00x]                   │
├────────────────────────────────────────┤
│ [Edit in Anim Editor]                  │
└────────────────────────────────────────┘
```

Blend curve dropdown: Linear, Ease In, Ease Out, Ease In/Out, Step.

### 4.2.4 Face Mode — ARKit Morph Sliders

*Depends on Part 3 NIF rendering + FaceGen pipeline being implemented.*

When the viewport is in Face mode, the Inspector becomes the morph editor. All 52 ARKit blend shapes, organized into 6 collapsible anatomical groups. Sliders are 0.0–1.0, live preview.

**Top-of-panel controls:**

```
┌── Face Morphs: Guard01 ────────────────────────────────────────────┐
│  [⟳ Reset All]  [⛓ Symmetric]  [Copy Pose]  [Paste Pose]          │
│  [Key Non-Zero]                [Key All 52]                        │
└────────────────────────────────────────────────────────────────────┘
```

- **Symmetric toggle** — when on, dragging any Left slider simultaneously drives its Right counterpart. A chain-link icon lights up next to each bilateral pair to indicate the link is active.
- **Key Non-Zero** — writes a FaceData keyframe at the current playhead for every morph with a value above 0.001. Keeps the dopesheet sparse and clean.
- **Key All 52** — writes all 52 values as a keyframe regardless of value (useful for blocking a full pose where zeroed morphs are intentional).

**Paired row layout — bilateral morphs:**

Each bilateral pair occupies one row: left slider, label, right slider. The `⛓` button toggles per-pair symmetry when global Symmetric is off.

```
  browDown    [━━━◉━━━] L  0.00  ⛓  0.00  R [━━━◉━━━]
  browOuterUp [━━━━━━◉] L  0.82  ⛓  0.81  R [━━━━━━◉]
```

Left sliders fill left-to-right. Right sliders fill right-to-left. This creates a natural bilateral mirror on screen.

**Single-slider layout — center/unpaired morphs:**

```
  browInnerUp         [━━━━━━◉━━━━] 0.73
  jawOpen             [━━━━━━◉━━━━] 0.60
  tongueOut           [━━━━━━━━━━◉] 1.00
```

**`jawLeft`/`jawRight` and `mouthLeft`/`mouthRight` are intentionally unlinked** — they describe the jaw or mouth sliding laterally in opposite directions, not symmetric bilateral muscles. Moving both simultaneously makes no physical sense. The `⛓` button is absent for these two pairs. Tooltip: "Opposing lateral morph — symmetry link unavailable."

**Full group breakdown:**

```
┌── [▼] Brows  (5)  ···  2 active ──────────────────────────────────┐
│  browDown      [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  browOuterUp   [━━━━━━◉]L 0.82 ⛓ 0.81 R[━━━━━━◉]                 │
│  browInnerUp            [━━━━━◉━━━━━]  0.40                        │
├── [▼] Eyes  (14)  ···  0 active ──────────────────────────────────┤
│  eyeBlink      [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  eyeLookDown   [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  eyeLookIn     [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  eyeLookOut    [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  eyeLookUp     [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  eyeSquint     [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  eyeWide       [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
├── [▼] Cheeks & Nose  (5)  ···  0 active ──────────────────────────┤
│  cheekPuff              [━━━◉━━━━━━━━]  0.00                       │
│  cheekSquint   [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  noseSneer     [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
├── [▼] Jaw  (4)  ···  1 active ────────────────────────────────────┤
│  jawForward             [━━━━━◉━━━━━━]  0.00                       │
│  jawLeft/Right [━━━◉━━━]L 0.00    0.00 R[━━━◉━━━]  (no link)      │
│  jawOpen                [━━━━━━━━━◉━━]  0.60                       │
├── [▼] Mouth  (23)  ···  0 active ─────────────────────────────────┤
│  mouthClose             [━━━◉━━━━━━━━]  0.00                       │
│  mouthFunnel            [━━━◉━━━━━━━━]  0.00                       │
│  mouthPucker            [━━━◉━━━━━━━━]  0.00                       │
│  mouthLeft/Right [━━━◉]L 0.00    0.00 R[◉━━━]       (no link)     │
│  mouthSmile    [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  mouthFrown    [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  mouthDimple   [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  mouthStretch  [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  mouthRollLower         [━━━◉━━━━━━━━]  0.00                       │
│  mouthRollUpper         [━━━◉━━━━━━━━]  0.00                       │
│  mouthShrugLower        [━━━◉━━━━━━━━]  0.00                       │
│  mouthShrugUpper        [━━━◉━━━━━━━━]  0.00                       │
│  mouthPress    [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  mouthLowerDown[━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
│  mouthUpperUp  [━━━◉━━━]L 0.00 ⛓ 0.00 R[━━━◉━━━]                 │
├── [▶] Tongue  (1)  ···  0 active ─────────────────────────────────┤
│   (collapsed)                                                      │
└────────────────────────────────────────────────────────────────────┘
```

**Group header activity indicator** — the `··· N active` badge shows how many morphs in the group have a value above 0.001, allowing a full scan of all 52 values without expanding every group.

**Dopesheet integration** — when a FaceData lane item is selected in the timeline and the viewport is in Face mode, the Inspector pre-populates all sliders from the keyframe data at the playhead. Editing a slider updates the item's keyframe live. Workflow: scrub to time → FaceData clip active → sliders show interpolated values → write new keyframe with Key Non-Zero.

### 4.2.5 Bone Selected (Anim Editor)

*See Part 7 — this view requires the Anim Editor (Phase 8).*

```
┌── Bone: NPC Spine2 [SPINE2] ───────────┐
│ Path: Root > COM > Pelvis > Spine      │
│       Spine1 > Spine2                  │
│ Parent: NPC Spine1 [SPINE1]            │
├────────────────────────────────────────┤
│ ─ Local Transform ─────────────────── │
│ T.X  [0.0000]  T.Y  [0.0000]          │
│ T.Z  [0.0000]                          │
│ R.X  [0.00°]   R.Y  [0.00°]           │
│ R.Z  [0.00°]                           │
│ (Euler XYZ) [▾ mode]                   │
├────────────────────────────────────────┤
│ [Key Position] [Key Rotation] [Key Both]│
│ [Key All Bones]                         │
└────────────────────────────────────────┘
```

---

## 4.3 ARKit Naming Contract and Import Validation
*(was §14.14)*

The TRI files paired with SCT must use the exact camelCase ARKit identifiers (`browDownLeft`, `jawOpen`, `mouthSmileRight`, etc.) as morph names. This is not the Skyrim modding convention — legacy TRI files use names like `Lip-Height` or the phoneme/modifier set. A legacy TRI file will load without error but all morph sliders will show 0.00 because no names match.

The Clip Import path (Phase 3+) should validate on load: after parsing the TRI header, check that at least one morph name matches a known ARKit identifier. If none match, show a Console warning:

```
TRI morphs do not use ARKit naming — FaceData lane will be empty.
Expected names: browDownLeft, jawOpen, mouthSmileLeft, etc.
Found: Aah, BlinkLeft, MoodHappy, ...
```

This surfaces the mismatch immediately rather than leaving the user confused by unresponsive sliders.

**Future consideration — live face capture:** ARKit is Apple's live face tracking standard. The bilateral paired-slider layout and camelCase naming are already the exact schema an iPhone face tracking session produces. The FaceData lane could eventually accept a live ARKit stream as a recording source — performing directly into the timeline the same way a voice actor records audio into a DAW. The UI supports this without changes: sliders become read-only during a live capture take, and the FaceData clip grows in real time as frames are written.

---

# PART 5 — SCENE ENVIRONMENT

*Phase 5. Terrain and static placement depend on Mutagen (Part 4) for LAND and REFR record reading. Props depend on NIF rendering (Part 3) for mesh display.*

---

## 5.1 Terrain & Scene Environment
*(was §8)*

### LAND Record Heightfield
*(was §8.1)*

Each cell = one `LAND` record containing:
- `VHGT` — 33×33 height grid in differential byte format (decode: running sum + offset float)
- `VCLR` — 33×33 RGB vertex colors — sufficient for previs without texture lookups
- `ATXT`/`VTXT` — up to 6 texture layer paint maps per cell quad — deferred

**Plan**: vertex-color-only terrain initially. 5×5 cell radius (~290m) around scene world position. Full 6-layer texture blending is a later enhancement.

### Static NIF Placement
*(was §8.2)*

`REFR` records in surrounding cells contain position, rotation, and a reference to a base object (`STAT`, `FURN`, `ACTI`, etc.) whose `MODL` subrecord gives the NIF path.

Two-tier rendering approach:

| Tier | Objects | Rendering |
|---|---|---|
| Context geometry | Everything in cell radius | Bounding boxes or low-poly proxies; read-only, no interaction |
| Focal assets | Objects the scene actively uses | Full NIF geometry; user-promoted via "Import as Scene Asset" |

"Import as Scene Asset" promotes a context object to a first-class scene participant with its world transform as the default placement. It then behaves identically to any other scene prop.

---

## 5.2 Animated Props, Lights & Cameras
*(was §9)*

### Animated Props
*(was §9.1)*

A prop is structurally an actor without a humanoid skeleton:

```
Actor:  CastEntry → Skeleton (bone hierarchy) → AnimClip → Pose → bone transforms
Prop:   PropEntry → NIF mesh (node hierarchy)  → AnimClip → ??? → node transforms
```

The NLE model is identical. The difference is in evaluation output: props drive NiNode world transforms rather than bone poses. A furniture piece with an "open" animation (e.g. a chair, a door) is a first-class prop with its own `NodeAnim` track type. The humanoid actor's interaction animation is a separate clip on the actor's `AnimClip` lane — the same approach Bethesda uses (paired furniture + actor HKX clips).

`PropEntry` alongside `CastEntry` in AppState — same pattern, different asset type. Timeline gets a `PropTrackGroup` alongside `ActorTrackGroup`.

### Lights
*(was §9.2)*

As **authoring data**: a `Light` track type in the scene-level track group. A light item has color, intensity, radius — keyframeable. Already scaffolded in `TrackType` enum.

As **rendered objects**: requires the GPU render pipeline (Part 3). In the interim, lights render as icons/glyphs in the viewport with a radius indicator circle.

LIGH records from the plugin can be imported as context lights (read their world position from `REFR`, properties from `LIGH`). Promoted lights become keyframeable scene participants.

### Cameras
*(was §9.3)*

No `CAMERA` records in vanilla Skyrim plugins. Cameras in vanilla scenes are expressed through scripting. SCT cameras are a **pure authoring concept** with no import side.

A camera item on the Camera track defines a cut or move: FOV, world transform, optionally animated. Export format TBD — candidate targets are scripted camera, `hkx` path animation, or CK scene camera node.

The Camera track type is already scaffolded. Data model is already correct.

---

# PART 6 — EXPORT & RUNTIME

*Phase 7. The export pipeline and runtime model depend on everything else being in place: animation clips, NIF meshes, plugin records, scene environment, and the camera creature. Build this last among the authoring features.*

---

## 6.1 Scene Graph & Runtime Export Model
*(was §10)*

### The Core Mapping
*(was §10.1)*

Two types of export artifact, cleanly separated:

| SCT concept | Export artifact |
|---|---|
| Beat node timeline | Animation HKX — one per actor per Beat node |
| Scene node graph (canvas of Beats + wait nodes + edges) | Behavior HKX — one per actor per scene |

**The scene node graph is a behavior file. The timeline inside a Beat node is an animation.**

The True Cinematics SKSE runtime applies per-actor behavior files to their actors, routes cross-actor sync events, and dispatches annotation-encoded effects. Havok's state machine execution is the scheduler — no separate scene execution format is required.

### Beat Node → Animation HKX
*(was §10.2)*

Each Beat node's timeline exports as a standard Havok animation clip per actor. All non-animation events on the timeline (audio, camera cuts, face data, environment triggers) are encoded as **annotation track payloads** on the animation:

```
hkaAnnotationTrack:
  t=0.00  "sct:audio:play:guard_line01.wav"
  t=0.00  "sct:face:start:guard01_confrontation.sctface"
  t=1.20  "sct:camera:cut:cam_outdoor_01"
  t=3.50  "sct:audio:stop"
```

Annotations are sparse point events. Face data is referenced by filename rather than inlined — the `.sctface` sidecar is a compact float array (52 ARKit channels × N keyframes) that the TC runtime loads at scene start and samples per-frame. Annotation track density is not a performance concern: 60,000+ annotations in a single HKX has been validated with no measurable overhead.

### Scene Node Graph → Behavior HKX (Per Actor)
*(was §10.3)*

The scene graph topology maps directly to Havok behavior graph node types:

| SCT node type | Havok node |
|---|---|
| Beat node | `hkbClipGenerator` referencing the Beat's animation HKX |
| Sequential edge A → B | `hkbStateMachine` transition from state A to B on clip-end event |
| QTE | `hkbStateMachine`: timer variable + success-event transition + timeout transition |
| Choice | `hkbStateMachine`: N event-driven transitions, one per option |
| Struggle | `hkbStateMachine` + `struggleBlend` float variable driven by TC runtime each frame |

Each actor receives a **completely isolated behavior file** for the scene. No actor's graph has any knowledge of another actor's graph. There is no shared state between actors at the Havok level.

### The Runtime Stack
*(was §10.4)*

Three layers, each with a distinct responsibility:

```
Engine Relay (foundation)
  ├── Puts each actor into a custom Havok user state
  │     character controller off · gravity off · physics off
  │     actor goes exactly where the behavior graph says, full stop
  ├── Dynamically injects per-actor behavior graphs via its own reference relay
  │     no patching of defaultmale.hkx / defaultfemale.hkx required
  └── Subclasses PlayerCamera with a frame-by-frame transform API
        camera creature's root bone world transform → fed in each frame

True Cinematics (coordinator)
  ├── Reads annotation events as they fire, dispatches effects
  ├── Routes cross-actor sync events between isolated behavior graphs
  └── Manages scene lifecycle: spawning, actor placement, cleanup

SCT HKX output (content)
  ├── Per-actor behavior HKX — scene graph topology as hkbStateMachine
  ├── Per-actor animation HKX — Beat timeline as clip + annotation events
  └── Camera creature HKX — bone animation is the camera path
```

The entire game AI stack is bypassed. The only game system that remains active is collision geometry — actors still stand on floors. Everything else is: collision + behavior graph + Engine Relay's miniature engine layer.

### Per-Actor Behavior Injection
*(was §10.5)*

All human NPCs share `defaultmale.hkx` / `defaultfemale.hkx`. Engine Relay's dynamic reference relay handles injection without touching the shared graph — it provides its own `hkbBehaviorReferenceGenerator`-equivalent mechanism that links per-actor scene behavior files at scene start and unlinks them at scene end. Each actor's scene file is self-contained: its own state machine, clip generators, and variable table. No actor has any knowledge of another actor's graph.

### Multi-Actor Synchronization
*(was §10.6)*

True Cinematics handles cross-actor sync. At the behavior graph level each actor is fully isolated; TC coordinates them by routing sync events — when actor A's annotation fires `"sct:sync:transitionB"`, TC dispatches the corresponding behavior event to all other actors in the scene. Event-based soft sync is sufficient for all scene work; sub-frame drift is not perceptible in practice.

### Wait Node Export
*(was §10.7)*

Wait node parameters that are Havok-native export into the behavior HKX. Parameters that are TC-runtime concerns export into the YAML sidecar.

**QTE:**
- `hkbStateMachine` with two outbound transitions
- Success transition fires on `QTESuccess` behavior event (TC writes this when correct input arrives before timer)
- Failure transition fires on `QTETimeout` behavior event (TC writes this when timer expires)
- `qteSlowTimeMult`, `qteWindUpDuration`, input action mapping → YAML sidecar

**Choice:**
- `hkbStateMachine` with N outbound transitions, one per option
- Each transition fires on `ChoiceOption_N` behavior event (TC writes this when player selects)
- Option text, subtitle text, voice file paths → YAML sidecar

**Struggle:**
- `hkbStateMachine` with success/failure outbound transitions
- `struggleBlend` float variable on the behavior graph, written each frame by TC
- Secondary actor (opponent) runs a mirrored graph with `1.0 - struggleBlend`
- `struggleDifficulty`, `struggleNoise`, `struggleNoiseFreq`, `struggleTimeLimit` → YAML sidecar

**Nominal preview duration** — each wait node still carries a nominal duration used by the authoring canvas (node width) and preview playback auto-advance. The runtime ignores it.

### The Camera Creature
*(was §10.8)*

The camera is a custom creature with a single bone and no game-engine baggage. It is a first-class actor in every scene.

**Custom race record:**
- Skeleton: single root bone (no COM, no pelvis, no character hierarchy)
- No body model, no skin — the creature is invisible
- No AI package, no combat style, no NavMesh requirement
- No physics / ragdoll data
- Engine Relay user state: character controller fully disabled

**How it works at runtime:**
1. TC spawns the camera creature at scene start and places it at the initial camera position
2. Engine Relay's `PlayerCamera` subclass accepts a world transform each frame via its API
3. TC feeds the camera creature's root bone world transform into that API each frame
4. The camera follows the bone — camera animation is bone animation, nothing more

**Camera animation = bone animation.** A dolly shot is a translation curve on the root bone. A crane is an arc. A cut between two cameras is an instant position jump at the start of the next Beat animation. No special camera system required — the same HKX pipeline that drives actors drives the camera.

**Scene-level events** (music, weather, environment changes, anything not owned by a specific actor) live as annotations on the camera creature's animation tracks:

```
camera creature Beat annotation track:
  t=0.00  "sct:music:play:scn_confrontation.xwm"
  t=0.00  "sct:weather:set:StormOvercast"
  t=4.50  "sct:music:transition:scn_confrontation_b.xwm"
  t=8.00  "sct:music:stop"
```

This eliminates the need for a separate scene-level track concept. Every event in the scene belongs to some actor's annotation track; the camera creature owns the ones that belong to no one else.

**In SCT's authoring model:** the Cast always contains an implicit Camera entry. The timeline's scene-level track group IS the camera creature's actor track group. The Camera viewport mode (§3.4) renders the camera creature's root bone path as a camera frustum trajectory. Animating the camera is identical to animating any other actor.

The CK has a "camera man" NPC that predates this approach — it carries a full NPC's AI, settling behavior, and physics, which causes well-known instability. The SCT camera creature has none of that. It exists only to carry a transform and fire annotation events.

### YAML Sidecar — Metadata Only
*(was §10.9)*

The YAML format is a thin spawn list. The camera creature is a regular actor entry:

```yaml
scene: confrontation_guard01
actors:
  - id: camera
    behaviorHkx: scene_confrontation_camera.hkx
    worldPos: [50.0, 0.0, 150.0]
    worldRot: 0.0
  - id: guard01
    behaviorHkx: scene_confrontation_guard01.hkx
    worldPos: [0.0, 0.0, 128.0]
    worldRot: 0.0
  - id: lydia
    behaviorHkx: scene_confrontation_lydia.hkx
    worldPos: [100.0, 0.0, 128.0]
    worldRot: 180.0
waitNodes:
  qte_door_escape:
    qteDuration: 3.0
    qteSlowTimeMult: 0.5
    qteWindUpDuration: 0.5
    inputAction: Jump
  choice_confront:
    options:
      - text: "Stand down."
        voice: scn_guard_choice_a.wav
      - text: "You don't want to do this."
        voice: scn_guard_choice_b.wav
```

TC reads this once at scene load — spawns actors, places them, loads `.sctface` files, reads wait node parameters — then Havok and Engine Relay drive everything forward.

### Root Motion
*(was §10.9 duplicate — root motion)*

Root bone translation in each Beat animation HKX carries any actor movement for that Beat. The TC runtime accumulates root bone transforms and applies them via `SetPosition` each frame. No separate root path spline authoring is required for the common case.

For the authoring surface, the Paths viewport tab visualizes the root bone path extracted from the Beat animations rather than a separately authored spline. This is a read-derived display, not a separate data source.

---

# PART 7 — ANIM EDITOR

*Phase 8. The Anim Editor tab and its dopesheet/curve editor depend on the full animation pipeline and the Inspector's bone view being in place.*

---

## 7.1 Anim Editor — Dopesheet and Curve Editor
*(was §14.12)*

### Dopesheet

```
         |0         |5         |10        |15   ...
Root     |
▼ Spine2 |  ◆           ◆
  T.X    |  ◆           ◆
  T.Y    |  ◆           ◆
  R.Z    |              ◆
▼ Head   |        ◆                 ◆
  R.X    |        ◆                 ◆
```

- Rows are bones; sub-rows are individual channels. Bones collapse like timeline actor groups.
- Diamond `◆` = keyframe marker, filled = selected, hollow = unselected
- Box-select encloses diamonds
- `Delete` = delete selected keyframes
- `Ctrl+C` / `Ctrl+V` = copy/paste keyframe selection (paste at playhead)
- Drag diamonds left/right = retime keyframes
- Ruler is frame-indexed, matching the AnimClip's frame count

**Filter bar:** `[Show: All ▾]` — All, Keyed Only, Selected Bones Only. Channel checkboxes: `[T] [R]`.

### Curve Editor (toggle from Dopesheet)

Each visible channel is a separate colored bezier curve. Control points at keyframes with tangent handles.

- Tangent mode per keyframe: Free, Aligned, Auto, Flat, Step
- Y axis auto-scales to visible curve range
- Horizontal zoom independent from Dopesheet zoom

---

# APPENDIX — PLANNING REFERENCE

*Non-linear reference material. Read at any time; not ordered by dependency.*

---

## A.1 Phase Roadmap
*(was §11)*

### Phase 1 — Animation Foundation ✅ Complete
- HKX binary → XML via SctBridge/DotNetHost
- Skeleton loading (HKX + XML)
- Animation clip loading and evaluation
- FK pose solving
- Single-actor viewport with orbit camera
- Basic transport controls (play/pause/loop/scrub)

### Phase 2 — NLE Timeline & Multi-Actor 🔄 In Progress
- [x] IPanel / TabRegistry / TrackRegistry architecture
- [x] AppState replacing SceneState
- [x] Multi-actor support in viewport (works with any creature type automatically)
- [x] NLE timeline: ruler, transport bar, scrubber
- [x] Clip drag-drop from Bin onto actor lanes
- [x] Skeleton type grouping in Clips bin
- [x] Data folder scan + skeleton picker modal
- [x] Type-aware drag feedback + drop rejection
- [x] Cast tab with actor management
- [ ] Scene Graph panel (node canvas, imgui-node-editor)
- [ ] Clip blending UI (blend in/out handles, multi-item overlap visualisation)
- [ ] Sequence save/load (SCT project file format)

### Phase 3 — NIF Mesh Rendering
- [ ] Integrate **nifly** (ousnius/nifly) as CMake dependency — NIF read/write for both LE (stream=83) and SE (stream=100)
- [ ] `BSBehaviorGraphExtraData` reader via nifly (enables invisible skeleton resolution from NIF drop; see §3.1.1)
- [ ] Render-to-texture pipeline (OpenGL FBO → ImGui::Image in viewport)
- [ ] Static (unskinned) NIF rendering — `BSTriShape` vertex + index data
- [ ] Skinned mesh rendering — `BSSkin::Instance` bone names + inverse bind matrices × Havok pose world transforms (§3.1.3)
- [ ] Basic PBR: diffuse texture from `BSLightingShaderProperty` → `BSShaderTextureSet`
- [ ] Compiled facegeom NIF loading — `BSDynamicTriShape` + read `BSFaceGenBaseMorphExtraData` as base vertex positions
- [ ] Custom thin TRI parser (FRTRI003, read-only) — load expression TRI delta arrays by morph name (§3.2)
- [ ] `FaceMorphWeights` in AppState per actor — 52-slot float array keyed by ARKit blend shape name (see §3.2)
- [ ] Per-frame expression blend loop on `BSDynamicTriShape` vertex buffers (§3.2)

### Phase 4 — Plugin Integration (Mutagen)
- [ ] Add Mutagen to SctBridge.dll
- [ ] PluginQuery bridge API: NPC → skeleton path, body NIF paths, facegen NIF path, expression TRI path
- [ ] "Import Character from Plugin" workflow — resolves NPC_ → HDPT → expression TRI (NAM0=1) automatically
- [ ] Facegen NIF loading (compiled facegeom NIF with baked chargen verts in `BSFaceGenBaseMorphExtraData`)
- [ ] Placed object reading (REFR → NIF path + world transform)

### Phase 5 — Scene Environment
- [ ] Terrain: LAND record heightfield via Mutagen (vertex-color only, 5×5 cell radius)
- [ ] Context NIF placement (bounding box proxies for radius)
- [ ] "Import as Scene Asset" promotion for focal objects
- [ ] Prop system (PropEntry, PropTrackGroup, NodeAnim track)
- [ ] Light placement + glyph rendering

### Phase 6 — Camera & Lights
- [ ] Camera items on Camera track (FOV, transform, animated)
- [ ] Light items on Light track (color, intensity, radius, animated)
- [ ] GPU light contribution in render-to-texture viewport
- [ ] Camera export format decision

### Phase 7 — Export
- [ ] Per-actor animation HKX export — one clip per Beat node per actor (via HKX2E/SctBridge)
- [ ] Per-actor behavior HKX export — scene node graph → `hkbStateMachine` topology (via HKX2E/SctBridge)
- [ ] `.sctface` face data export — 52-channel ARKit float array per Beat per actor
- [ ] YAML sidecar export — actor identities, world placements, wait node parameters, face data file references
- [ ] NPC_ record creation via Mutagen+SctBridge
- [ ] Export dialog: per-target checklist (behavior HKX package, animation HKX package, `.sctface` files, YAML sidecar, optional NPC_ record). Progress log in Console panel.

### Phase 8 — Anim Editor Tab
- [ ] Bone Editor viewport mode
- [ ] Per-bone transform editing
- [ ] Clip authoring (create new clips from pose keyframes)
- [ ] AnimatorPanel (state machine visualisation, imgui-node-editor)

---

## A.2 Phase-by-Phase UI Additions
*(was §14.15)*

| Phase | UI Change |
|---|---|
| **Phase 3** (NIF rendering) | Render mode strip (Wire/Solid/Textured) becomes active. Viewport switches to FBO/ImGui::Image. Assets tab in Media Pool becomes live. ARKit TRI import validation added. |
| **Phase 4** (Mutagen/ESP) | "Import from ESP" in Cast tab works. Export dialog appears. Scene Properties gains export path and target ESM field. |
| **Phase 5** (Terrain/props) | Assets tab shows props. Viewport gains terrain heightfield. |
| **Phase 6** (Camera/Lights) | Cameras viewport mode becomes useful. Camera properties in Inspector gain FOV and DoF fields. Light lane added to Scene Tracks. |
| **Phase 7** (Export) | Export dialog: per-target checklist (behavior HKX package, animation HKX package, `.sctface` files, YAML sidecar, optional NPC_ record). Progress log in Console panel. |
| **Phase 8** (Full Anim Editor) | Animator panel node editor live. Bone gizmo in viewport. Full dopesheet and curve editor. FK/IK toggle per limb chain. |

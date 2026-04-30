# SCT Source Reorganization Plan

This document captures the full reorganization plan for the `src/` directory. The goal is
separation of concerns that enables parallel Claude sessions to own non-overlapping surface
areas — one session on UI/UX, another on the renderer, another on the animation export
pipeline, etc.

The codebase is already well-factored (no circular deps, clean domains). This is a rename
operation, not a refactor. The value is making the architecture legible from the directory
tree and giving each future session a clear "this is yours" boundary.

---

## Target Directory Map

```
src/
│
├── app/                   ← application host — top of the dependency graph
│   ├── App.h/cpp          (GLFW window + main loop)
│   ├── AppState.h/cpp     (single source of truth — includes all domains below)
│   ├── ProjectFile.h/cpp  (JSON .sct serialisation / deserialisation)
│   └── main.cpp
│
├── anim/                  ← Havok animation pipeline — pure data, no I/O or rendering
│   ├── AnimClip.h/cpp     (body animation frame data + bone-track binding)
│   ├── FaceClip.h/cpp     (ARKit face morph keyframes extracted from HKX annotations)
│   ├── HavokAnimation.h/cpp  (HKX binary → XML → AnimClip parser)
│   ├── HavokSkeleton.h/cpp   (HKX binary → XML → Skeleton parser)
│   ├── Pose.h/cpp         (live bone pose + SolveFK accumulation)
│   └── Sequence.h/cpp     (NLE document: actor groups, scene tracks, Evaluate dispatcher)
│
├── asset/                 ← file format I/O — reads game files, produces CPU-side data
│   ├── BsaReader.h/cpp    (BSA v104/v105 archive reader with LZ4 decompression)
│   ├── DdsLoader.h/cpp    (BC1/3/5/7/BGRA8 → raw bytes for renderer upload)
│   ├── MuFeeConfig.h/cpp  (MFEE INI parser: vanilla TRI path → ARKit TRI path)
│   ├── NifAnim.h/cpp      (NiControllerManager + NiGeomMorpherController parsing)
│   ├── NifDocument.h/cpp  (full NIF: blocks, shapes, toRoot transforms, skin bindings)
│   ├── NifLoader.h/cpp    (LoadNifDocument / LoadNifDocumentFromBytes entry points)
│   ├── TerrainMesh.h/cpp  (VHGT/VCLR → MeshData; 33×33 grid in Skyrim world space)
│   └── TriDocument.h/cpp  (FRTRI003 morph file parser: diff morphs + ARKit mod morphs)
│
├── scene/                 ← authoring data types (owned by AppState, authored by the user)
│   ├── ActorDocument.h    (identity + resolved asset cache + morph weight overrides)
│   └── CameraShot.h       (named scene camera: eye position, yaw, pitch, FOV)
│
├── plugin/                ← bridge layer — .NET/Mutagen integration, optional at runtime
│   ├── DotNetHost.h/cpp   (.NET 10+ runtime bootstrap + SctBridge.dll P/Invoke)
│   ├── IPluginBackend.h   (abstract backend; defines NpcRecord, LandRecord, etc.)
│   └── MutagenBackend.h/cpp  (concrete Mutagen implementation via DotNetHost)
│
├── export/                ← export pipeline — writes runtime assets for Engine Relay
│   │
│   ├── hkx/               ← per-actor HKX generation
│   │   ├── BehaviorGraph.h     (hkbStateMachine / hkbClipGenerator type definitions)
│   │   ├── AnimationExport.h   (Beat-timeline → HKX clip mapping types)
│   │   └── HkxWriter.h/cpp     (main write pipeline: produces behavior + animation HKX)
│   │
│   ├── yaml/              ← YAML sidecar (spawn list, placements, wait node params)
│   │   ├── ActorPlacement.h    (per-actor world placement type)
│   │   ├── WaitNode.h          (wait node parameter type: blend time, hold duration)
│   │   ├── SpawnList.h         (spawn list entry type: formKey, placement ref)
│   │   └── YamlSidecar.h/cpp   (main sidecar serializer; imports the types above)
│   │
│   └── annotation/        ← HKX annotation track encoding (non-animation event payloads)
│       ├── FaceAnnotation.h    ("MorphFace.<system>|<name>|<weight>" format types)
│       ├── EventAnnotation.h   (audio cue + camera event annotation types)
│       └── AnnotationEncoder.h/cpp  (encodes event timeline → Havok annotation entries)
│
├── env/                   ← ALREADY ORGANIZED — environment streaming
│   ├── CellEnvironmentManager.h/cpp
│   └── TerrainStreamManager.h/cpp
│
├── renderer/              ← ALREADY ORGANIZED — render interface + OpenGL backend
│   ├── ISceneRenderer.h
│   ├── GlSceneRenderer.h/cpp
│   └── ShaderSources.h
│
├── core/                  ← ALREADY ORGANIZED — utilities with no domain dependencies
│   ├── io/
│   │   ├── BoundsSafeReader.h  (safe bounds-checked binary read cursor)
│   │   └── BinaryWriter.h      (counterpart write cursor — needed for HKX export)
│   ├── math/
│   │   └── Interpolation.h     (lerp/slerp, easing, Bezier, Hermite, blend ramps)
│   ├── threading/
│   │   └── BackgroundWorker.h  (one-thread work queue template: Req → Res)
│   └── render/
│       └── GpuAssetCache.h     (deduplicating path → TextureHandle template + LowerPath)
│
└── ui/                    ← ALREADY ORGANIZED — all UI (panels read AppState, never write domain logic)
    ├── Camera.h/cpp            (orbit camera: azimuth, elevation, radius, view/proj matrices)
    ├── ControllerInput.h/cpp   (GLFW gamepad polling → freecam movement)
    ├── IPanel.h
    ├── MainLayout.h/cpp
    ├── StylePalette.h
    ├── TabRegistry.h/cpp
    ├── TrackRegistry.h/cpp
    └── panels/
        ├── ActorRenderCache.h/cpp
        ├── ClipBinPanel.h/cpp
        ├── InspectorPanel.h/cpp
        ├── NifBrowserPanel.h/cpp
        ├── NifEditorPanel.h/cpp
        ├── NifEditorState.h
        ├── NifGraphPanel.h/cpp
        ├── PluginBrowserPanel.h/cpp
        ├── SceneGraphPanel.h/cpp  (stub)
        ├── AnimatorPanel.h/cpp    (stub)
        ├── TimelinePanel.h/cpp
        └── ViewportPanel.h/cpp
```

---

## Session Ownership Boundaries

Once the reorganization is complete, parallel sessions can own non-overlapping directories.
A session reads across boundaries but only *writes* to its own domain.

| Session | Owns (read/write) | Reads (never modifies) |
|---|---|---|
| **Renderer** | `renderer/`, `core/render/` | `ISceneRenderer.h` is the contract — nothing else |
| **Animation** | `anim/` | `scene/`, `core/` |
| **Asset I/O** | `asset/` | `core/io/`, `renderer/ISceneRenderer.h` (MeshData/TextureHandle types only) |
| **Environment** | `env/` | `asset/`, `renderer/`, `plugin/` |
| **Export pipeline** | `export/`, `core/io/BinaryWriter.h` | `anim/`, `scene/`, `asset/` |
| **Plugin bridge** | `plugin/` | `scene/` (IPluginBackend data structs live there) |
| **UI/UX** | `ui/`, `ui/panels/` | Everything above (read-only via AppState) |
| **App / State** | `app/`, `scene/` | All domains (AppState is the integration point) |

`AppState` is the integration point and the one file that touches everything. Panels read it;
domain systems are called from it. If two sessions both need to add AppState fields, they
coordinate through this doc rather than conflicting blindly.

---

## Execution Plan

### Phase 0 — Delete dead code (15 min, zero risk)
These files are no-op shims with no real content. Delete them after verifying nothing still
includes them.

- `src/Renderer.h` — deprecated Phase 2 renderer, superseded by GlSceneRenderer
- `src/CastEntry.h` — 6-line redirect to ActorDocument
- `src/SceneState.h` — 5-line redirect to AppState

Verification: `grep -r "CastEntry.h\|SceneState.h\|Renderer.h" src/`

---

### Phase 1 — `anim/` (highest value, most self-contained)

**Files to move:**
- `AnimClip.h/cpp` → `anim/`
- `FaceClip.h/cpp` → `anim/`
- `HavokAnimation.h/cpp` → `anim/`
- `HavokSkeleton.h/cpp` → `anim/`
- `Pose.h/cpp` → `anim/`
- `Sequence.h/cpp` → `anim/`

**Include updates required** (files that include these headers):
- `AppState.h` (includes AnimClip, FaceClip, HavokSkeleton, Sequence)
- `ui/panels/ActorRenderCache.h` (includes Pose)
- `ui/panels/ActorRenderCache.cpp` (includes Sequence)
- `ui/TrackRegistry.h` (includes Sequence)
- `ui/panels/ViewportPanel.h` (no direct anim includes after ActorRenderCache extracted)
- `env/CellEnvironmentManager.h` (includes NifAnim — stays in asset/, not anim/)

Cross-check: anim/ files only depend on each other and `core/`. No upward deps. Safe to move
independently.

---

### Phase 2 — `asset/`

**Files to move:**
- `BsaReader.h/cpp` → `asset/`
- `DdsLoader.h/cpp` → `asset/`
- `MuFeeConfig.h/cpp` → `asset/`
- `NifAnim.h/cpp` → `asset/`
- `NifDocument.h/cpp` → `asset/`
- `NifLoader.h/cpp` → `asset/`
- `TerrainMesh.h/cpp` → `asset/`
- `TriDocument.h/cpp` → `asset/`

**Include updates required:**
- `AppState.h` (no direct asset includes — comes through ActorDocument → TriDocument)
- `ActorDocument.h` → will move to `scene/` in Phase 4, so update then
- `env/CellEnvironmentManager.h/cpp` (NifAnim, NifDocument, BsaReader)
- `env/TerrainStreamManager.h` (TerrainMesh)
- `ui/panels/ActorRenderCache.cpp` (NifDocument)
- `ui/panels/NifEditorState.h` (NifDocument)
- `ui/panels/NifBrowserPanel.h`, `NifGraphPanel.h` (NifDocument via NifEditorState)

NifDocument depends on NifAnim (same domain — both move together). NifAnim has no external
deps beyond GLM. Clean move.

---

### Phase 3 — `plugin/`

**Files to move:**
- `DotNetHost.h/cpp` → `plugin/`
- `IPluginBackend.h` → `plugin/`
- `MutagenBackend.h/cpp` → `plugin/`

**Include updates required:**
- `AppState.h` (includes IPluginBackend for pluginBackend member)
- `env/CellEnvironmentManager.cpp` (DotNetHost for CellGetRefs, ExteriorCellGetRefs)
- `env/TerrainStreamManager.cpp` (DotNetHost for WorldspaceGetTerrainBulk)
- `ui/panels/PluginBrowserPanel.h` (IPluginBackend)
- `ui/panels/ViewportPanel.cpp` (was DotNetHost — now delegated to TerrainStreamManager)

Note: after Phase 3, `AppState.h` will include `plugin/IPluginBackend.h`. This is correct
— AppState owns the pluginBackend unique_ptr.

---

### Phase 4 — `scene/` + Camera/ControllerInput into `ui/`

**Files to move:**
- `ActorDocument.h` → `scene/`
- `CameraShot.h` → `scene/`
- `Camera.h/cpp` → `ui/`
- `ControllerInput.h/cpp` → `ui/`

**Include updates required:**
- `AppState.h` (includes ActorDocument, CameraShot)
- `ui/TrackRegistry.h` (includes ActorDocument)
- `ui/panels/ActorRenderCache.h` (includes ActorDocument via AppState)
- `ui/panels/ViewportPanel.h` (includes Camera, ControllerInput)
- `ui/panels/NifEditorState.h` (includes Camera)

ActorDocument depends on TriDocument (moving to asset/ in Phase 2 — so Phase 2 must
complete before Phase 4 updates its include path).

---

### Phase 5 — `app/` (last — everything depends on AppState)

**Files to move:**
- `App.h/cpp` → `app/`
- `AppState.h/cpp` → `app/`
- `ProjectFile.h/cpp` → `app/`
- `main.cpp` → `app/`
- `pch.h` → `app/` (or keep at root — precompiled headers are typically root-level)

**Include updates required:**
This is the highest-churn step. `AppState.h` is included by virtually every panel and most
`.cpp` files. The mechanical search-replace is straightforward (`"AppState.h"` →
`"app/AppState.h"`), but touching every file makes this the riskiest phase for merge
conflicts if any other work is in flight.

Recommendation: do this as a single commit with nothing else in flight.

---

### Phase 6 — `export/` scaffold (new files, no moves)

Scaffold the export domain with type-definition headers and stub implementations. No logic
required yet — just the shape of the API so future sessions have a contract to build against.

**New files to create:**

```
export/hkx/
  BehaviorGraph.h        ← hkbStateMachine, hkbClipGenerator, hkbBlendTree node types
  AnimationExport.h      ← types mapping Sequence clips → HKX clip indices
  HkxWriter.h/cpp        ← stub; interface defined, implementation empty

export/yaml/
  ActorPlacement.h       ← { formKey, posX/Y/Z, rotX/Y/Z, scale }
  WaitNode.h             ← { blendInTime, holdDuration, blendOutTime }
  SpawnList.h            ← { vector<ActorPlacement> }
  YamlSidecar.h/cpp      ← stub; Write(path, spawnList, waitParams) declared, not implemented

export/annotation/
  FaceAnnotation.h       ← { morphName, weight, time } — mirrors FaceClip channel format
  EventAnnotation.h      ← { AudioCue, CameraEventAnnotation } types
  AnnotationEncoder.h/cpp ← stub; Encode(FaceClip, audioEvents) → vector<HkxAnnotation>
```

The type definitions are the deliverable here, not the implementations. Once these exist,
the export session can implement them independently without coordinating with UI or anim.

**Also create:**
```
core/io/BinaryWriter.h   ← write cursor counterpart to BoundsSafeReader
                            methods: WriteU8/16/32/64, WriteF32, WriteBytes, WriteString
                            needed by HkxWriter
```

---

## Notes on `pch.h`

The precompiled header currently contains only external dependencies (STL, GLM, ImGui,
nlohmann/json, Windows.h). This is correct — internal headers should never be in PCH.
No changes needed. When files move to subdirectories, the PCH path may need to be adjusted
in VS project settings but the content stays the same.

---

## Completion Criteria

The reorganization is complete when:
1. `src/` root contains only subdirectories (no loose .h/.cpp files except pch.h if kept there)
2. All includes use subdirectory-qualified paths (`"anim/AnimClip.h"`, `"asset/BsaReader.h"`, etc.)
3. The project builds cleanly
4. `Renderer.h`, `CastEntry.h`, `SceneState.h` are deleted
5. `export/` scaffold exists with type-definition headers in place
6. `core/io/BinaryWriter.h` exists

Each phase can be committed and built independently. Phases 1–3 are safe to run in any
order relative to each other (no cross-dependencies between anim/, asset/, plugin/). Phase 4
depends on Phase 2 (ActorDocument → TriDocument path). Phase 5 is always last.

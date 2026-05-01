# SCT Platform Expansion — Design Document

> **Status:** Living document. Originated 2026-04-30 design session.
>
> Captures the strategic shift from "cutscene authoring tool" to "the CK for animators" — a multi-workspace content creation suite for the animator-facing slice of Skyrim modding.
>
> Updated as the design conversation continues. Open questions flagged inline with **OPEN:** prefixes; resolved decisions migrate to §12.

---

## 1. Vision

SCT is a modern content creation suite for the **animator-facing slice of Skyrim modding**. It does not try to be xEdit, the CK's quest/scripting/AI side, or a Papyrus IDE.

It does try to fully replace **NifSkope, the creation-time half of Outfit Studio, HKAnno, and the (nonexistent) good behavior editor**, plus host new workflows like cutscene authoring, procedural NIF construction, custom creature pipelines, and visual scripting that no existing tool offers.

**Mental model:** modern node-graph + viewport-first authoring for everything that touches a mesh, a skeleton, a behavior, or a scene.

**Tagline:** *"The CK for animators."*

### Renaming the implicit scope

The existing tool name "Skyrim Content Tools" is plural. Today the tool is effectively one workflow (cutscene authoring) wearing the suite's name. The expansion makes the name accurate — the same shell hosts multiple tools, each a focused content workflow, sharing infrastructure underneath.

Closest analogue: an IDE (Visual Studio) rather than an app (Premiere). Each "tool" is a workspace; the underlying engine — viewport, plugin reader, asset cache, graph framework — is shared.

---

## 2. Workspace Map

| Workspace | Status | Replaces / Targets | Scope summary |
|---|---|---|---|
| **Cutscene Editor** | Existing (in progress) | No vanilla equivalent; targets TC + ER runtime | Multi-actor NLE, beat graph, wait nodes, camera, audio, face data |
| **NIF Editor** | New | NifSkope (entirely), OS creation half | Procedural NIF construction via geometry-node graph, weight painting, particle/VFX authoring, physics-aware design |
| **NPC Editor** | New | RaceMenu workflow, CK character window, NPC subset of xEdit | Face sculpt, chargen bake, headparts, equipment preview, plugin export |
| **Animation Editor** | Planned (Phase 8) | HKAnno, animation portions of CK | FK/IK posing, dopesheet, curve editor, annotation tracks, retarget, mocap import |
| **Behavior Editor** | New | Havok Behavior Tool (dead/closed), CK's read-only viewer; functionally a visual scripting engine | Visual scripts compiling to behavior HKX + ER bridge code |
| **Cell/World Editor** | Eventual / not committed | Slice of CK's render window | Terrain generation, REFR placement — only what CK does poorly |
| **Custom Creature Pipeline** | Cross-workspace | No coherent existing tool | FBX → skeleton.hkx + skeleton.nif + body NIF + behavior HKX + RACE record |

### Explicitly out of scope

- Quest editing, AI packages, Papyrus authoring, Story Manager scenarios
- Dialogue tree authoring
- Sound descriptor authoring
- Scripted magic effects (MGEF authoring beyond what NPC/visual workflows touch)
- Navmesh editing
- Mesh sculpting / hard-surface modeling from scratch (route via FBX import instead)
- UV unwrapping
- Texture creation/painting
- General xEdit-style record browsing

### What replaces vs. what we touch

Workspaces touch xEdit-domain records when the visual workflow demands it (NPC_, RACE, ARMA, MODL, HDPT, FACT, etc.) — but never as a general-purpose record browser. xEdit remains the right tool for general plugin work.

---

## 3. Per-Workspace Scope Detail

### 3.1 Cutscene Editor

The existing surface. Expansion narrows what it is *not*:
- Does not target vanilla SceneGenerator
- Does not generate CK Scene records
- Does not author quest stages

What it does touch from CK's domain:
- Trigger conditions (story manager triggers that fire scenes) — **OPEN:** in-scope authoring vs. external CK setup?
- Marker-based actor placement (XMarker / XMarkerHeading REFRs as scene anchors) — **OPEN:** consume only, or also create/edit?

The runtime target is True Cinematics + Engine Relay. Cutscene work assumes the SCT runtime stack at play time, not vanilla.

### 3.2 NIF Editor

Three orthogonal layers, married into one tool:

**(a) NifSkope layer** — block/property inspection and editing. Block tree, property pane, hex view as fallback. Every block type round-trips. Floor capability.

**(b) Outfit Studio layer** — mesh-on-skeleton authoring. Weight painting, weight transfer, weight smoothing. Body-fitting tools. Multi-mesh outfit composition. Live preview on a skinned body.

**(c) Geometry Nodes layer** — the procedural workflow. Node graph whose data type is "NIF geometry + skinning + materials + collision + particles."

```
Source nodes:        Load NIF, primitive shapes, import OBJ/FBX/glTF (via assimp)
Transform nodes:     Subdivide, decimate, mirror, array, boolean
Skinning nodes:      Auto-bind to skeleton, transfer weights, smooth, project from another mesh
Material nodes:      Assign shader, set textures, configure flags
Collision nodes:     Convex hull, decomposition, AABB, bone-bound
VFX nodes:           Particle emitter, modifier chain, spawn shape, lifetime curves
Sink:                Write NIF
```

The procedural model is what differentiates this from NifSkope. "Take this body shape, apply these straps, generate this collision, weight-paint to skeleton X with smoothing radius Y" becomes a re-runnable graph — change the body, the armor regenerates.

**Physics simulation in viewport.** Bullet (zlib license, same engine SMP uses) integrated for live cloth simulation during weight painting. Adjust weights → see cloth fall in real time. SMP XML config readable/writable as a graph node.

**Particle/VFX authoring.** NiParticleSystem authoring through the same node graph paradigm. Live preview in viewport.

**Scope decisions:**
- Sculpting: **OUT.** Use ZBrush/Blender, route through FBX import.
- UV unwrapping: **OUT.** Viewing yes, editing no.
- Texture painting: **OUT.** Use Substance/Photoshop.
- Hard-surface modeling beyond primitives + booleans: **OUT.** Blender's lane.
- BodySlide preset application (player-facing morph distribution): **OUT.** Runtime concern, not authoring.
- BSEffectShader-based VFX (shader effects that aren't NiParticleSystem): **OPEN.**

### 3.3 NPC Editor

Specifically a "facegen + body + equipment + plugin export" surface — not a general NPC record editor. Touches xEdit-domain data only where the visual workflow requires it (NAM9, PNAM, NAMA, QNAM, FTST, HCLF). Stays out of factions, AI packages, perks, spell lists, voice type, dialogue.

Includes the chargen TRI bake — the one piece of CK behavior worth replicating. Generate compiled facegeom NIFs from authored face state by applying NAM9-weighted deltas + chargen morphs to the base head NIF and saving the result.

**Open scope questions:**
- **OPEN:** Player character editing — same workspace as NPC, or separate?
- **OPEN:** Tint layer authoring — brush/painting tools, or numeric only?

### 3.4 Animation Editor

Maps to existing Phase 8 plans. FK/IK posing, dopesheet, curve editor, annotation tracks, skeleton view, retargeting, mocap import via FBX/BVH.

Replaces HKAnno (annotation editing) and the (nonexistent) good HKX authoring tool. Internal use of HKX2E (binary↔XML conversion) continues.

Out of scope: behavior graph editing (separate workspace), animation generation (KiMoDo is a separate integration).

### 3.5 Behavior Editor

This is the workspace that expanded most dramatically in this design discussion. Originally framed as "Havok behavior graph editor"; the discussion reframed it as a **visual scripting engine** where Havok behavior graphs are the execution substrate (one of two — the other being Engine Relay).

See §6 for the visual scripting engine deep-dive.

### 3.6 Cell/World Editor

Deliberately underscoped for now. Terrain generation eventually, REFR placement maybe. Revisit after NIF + Behavior workspaces ship.

**OPEN:** Confirm deferred entirely until NIF + Behavior ship.

### 3.7 Custom Creature Pipeline

Not a workspace per se — a cross-workspace workflow. See §5 for the FBX → NIF bridge that enables this.

---

## 4. The Graph Paradigm — A Unifying Primitive

Three workspaces want graph editing. They should share one framework:

1. **NIF Editor** — geometry nodes (procedural mesh/skin/material/VFX construction)
2. **Behavior Editor** — visual scripting graph (compiles to Havok behavior + ER bridges)
3. **Cutscene Editor — Scene Graph panel** — beat nodes + wait nodes + transitions (already planned with imgui-node-editor)

Build **one node-graph framework** with:
- Typed sockets and a per-domain node library (NIF nodes, behavior/script nodes, scene nodes)
- Storage / evaluation / dependency tracking / caching
- A shared editor UI (imgui-node-editor)
- Per-domain serialization formats

Meaningful infrastructure work. Smaller than Blender's geometry nodes (no general field math — typed pipelines per domain). Realistic. Payoff: all three graph workspaces share the editor, evaluation engine, undo/redo plumbing, and the user's mental model.

**Meta-nodes are first-class.** Users define sub-graphs, name them, package them as libraries, reuse them across scripts/NIFs/scenes. This is the SCT-side equivalent of "custom Havok node types" — see §7.

This makes the graph framework a first-class infrastructure pillar alongside the renderer, plugin bridge, and asset I/O.

**OPEN:** One graph framework for all three graph workspaces, or per-workspace? (Strong recommendation: one framework.)

---

## 5. The FBX → NIF Bridge

Instead of building a sculptor inside SCT, route external DCC tool output through SCT's "adapt to NIF" workflow. SCT becomes the **bridge** between Blender/Max/ZBrush/Substance/Unreal/Unity asset libraries and Skyrim's NIF format.

Fits the geometry nodes paradigm cleanly:

```
[Import FBX] ──▶ [Map Skeleton] ──▶ [Transfer Weights] ──▶ [Assign BSLightingShader]
                       │                                              │
                       ▼                                              ▼
                [Generate Collision] ───────────────▶ [Write NIF]
```

Source nodes for external formats (FBX, OBJ, glTF) are first-class entry points via assimp (permissive, mature). Adaptation nodes:
- **Map Skeleton** — bind source armature bones to a target Skyrim HKX skeleton by name + manual override table
- **Transfer Weights** — reweight against the target skeleton, optionally projected from a reference body
- **Assign Shader** — translate PBR materials → BSLightingShaderProperty + texture assignment
- **Generate Collision** — convex hull / decomposition / AABB
- **Fix Coordinate System** — handle FBX (Y-up or Z-up) → Skyrim Z-up

### Custom Creature Pipeline (the showcase)

Custom creatures today require 3ds Max (Bethesda's HKX exporter is Max-only), manual skeleton authoring, manual NIF setup, hand-built behavior HKX, CK race records — a multi-tool, multi-day workflow.

If SCT can:
1. Import a custom skeleton + skinned mesh from FBX
2. Generate `skeleton.hkx` via HKBuild's HKX2E
3. Generate matching `skeleton.nif`
4. Generate body NIF with proper `BSSkin::Instance` binding
5. Author a behavior HKX (Behavior Editor) or fork an existing one
6. Export RACE + NPC_ records via Mutagen

…then **the entire custom creature pipeline collapses into one tool.** Currently no coherent workflow exists for this.

This is the natural showcase for SCT's "everything one tool" pitch — touches NIF Editor + Animation Editor + Behavior Editor + NPC Editor.

---

## 6. Behavior Editor as Visual Scripting Engine

### Reframe

The Behavior Editor isn't an animation tool. It's a **game-logic authoring tool** that happens to use Havok behavior graphs as its execution substrate. Animation control is one use case among several.

**Visual scripting doesn't exist in Skyrim modding today.** Papyrus is text. SKSE is C++. CK fragments are text-in-records. Story Manager is a half-graph half-text condition tree. If SCT does visual scripting, it's new ground — and the audience (every Skyrim modder who's wished Papyrus was less painful) is much larger than cutscene authors.

### Why this is feasible (Engine Relay is the key)

Without ER, "visual scripting" would be limited to what the 2011 Havok behavior runtime natively expresses — rich for animation, thin for game logic. ER's hooks (custom Havok user state, owned behavior injection, frame-by-frame transform API, isolated runtime) bridge the graph into Skyrim's actual game state.

### Compilation model

A visual script compiles to three coordinated artifacts:

```
Visual Script Graph
        │
        ├──▶ Behavior HKX             (Havok-native ops: state machines, transitions, blend trees)
        │
        ├──▶ ER Consumer Registration (which ER channels the script opts into: behavior / user state / camera)
        │
        └──▶ Bridge Code (optional)   (anything Havok can't express: world queries, papyrus calls)
                    │
                    └──▶ Wired together via Havok variables + annotation events
```

Each node routes across these artifacts:
- **Pure Havok node** — standard `hkbNode` subclasses, runs on the Havok tick
- **Pure bridge node** — C++ in SKSE space, communicates via behavior variables/events
- **Hybrid node** — both pieces; bridge writes a variable each frame, Havok branch reads it
- **Channel-implying node** — placement implies a consumer registration opt-in (Camera Cut → camera channel; Custom Behavior State → behavior channel)

User authors one unified graph. The compiler splits it. See §8 for the ER consumer model that the registration artifact targets.

### Node categories

**Flow** *(Havok)* — State Machine, Sequence, Branch, Wait, Loop
**Animation** *(Havok)* — Play Clip, Blend, Layer, IK Modifier
**Events** *(Havok + annotations)* — On Annotation, Send Event, Send Annotation
**Variables** *(Havok + ER bridges)* — Get/Set primitives, Compare, Math; bridged: Player Distance, Game Time, Quest Stage, Health, Weather
**World queries** *(ER)* — Find Reference, Cast Ray, Get/Set Position, Cell/Worldspace, Is In Combat / Dialogue / Sneaking
**Effects** *(ER + annotations)* — Play Sound, Play VFX, Apply Magic Effect, Camera Cut/Shake/FOV
**Game state** *(ER)* — Get/Set Quest Stage, Story Manager Event, Inventory ops, Equip, Force Greet
**Custom (meta-node)** *(compile-time)* — user sub-graphs, reusable, packageable as libraries

### Comparison to Unreal Blueprints

**Holds:** visual graph as primary authoring surface, typed sockets, event-driven dispatch, sub-graphs/functions/macros for abstraction, live debugging by stepping through nodes.

**Doesn't hold:**
- UE Blueprints run on UE's frame tick with full game-state reflection. SCT runs on Havok tick with ER-mediated state access — slower, narrower, but sufficient.
- UE Blueprints are Turing-complete. SCT Havok-side is finite-state — no `for (int i; ...)` directly. Loops are ER-side or expressed via state-machine repetition.
- UE has unlimited extensibility. SCT is bounded by `(Havok node set ∪ ER bridge set)`. But the bridge set is YOUR code — language extends as fast as you write bridges.

The 2011 Havok constraint is real but bounded. The "scripting" parts users want for game logic almost all fall on the ER side, where C++ is unconstrained.

### Open questions

- **OPEN:** Per-actor or scene-level scripts? Probably both — actor-level (this NPC's behavior) and scene-level (this quest's logic). Scene-level might ride on the camera creature's behavior graph.
- **OPEN:** Where does a scene-level script live in the runtime stack? ER plugin alongside Havok, or hidden helper actor whose graph IS the script?
- **OPEN:** Trigger model — TC scene begin, Story Manager event, player interaction, dialogue choice, quest stage change, on-cell-load? Defines what kinds of scripts users can author.
- **OPEN:** Debugger surface — live node highlighting, variable inspector, event log, breakpoints, step-through, watches? (Strong recommendation: real debugger early, not bolted on.)

---

## 7. Custom Havok Node Types — The Honest Answer

Short version: **truly custom node types in the strict sense are likely impossible without engine work.**

Four layers, most-feasible to least:

**Layer 1 — Author-time meta-nodes (definitely doable).** SCT authors graphs with abstractions that don't exist in Havok and *compiles them down* to compositions of standard nodes at export time. Runtime sees only stock types. Same approach a compiler takes — language-level abstractions that don't survive past compilation. No engine work.

**Layer 2 — SKSE-driven behavior (Engine Relay, already used).** Standard Havok nodes drive the graph; SKSE plugins read variables/events, run arbitrary logic, write back. This is what TC + ER already do. Startlingly complex things possible without introducing a new node type. SKSE plugin architecture already exists.

**Layer 3 — Bethesda's own custom nodes (`BSI*` classes).** Bethesda extended Havok with their own classes (`BSiStateTagger`, custom modifiers). Baked into Skyrim's Havok runtime — not user-extensible. We can use them, not add to them.

**Layer 4 — Truly new C++ node types registered at runtime.** Would require either reverse-engineering Skyrim's Havok class registry and patching new entries via SKSE, or shipping a parallel behavior runtime alongside Havok. Not aware of anyone having done either. Havok 2010 SDK is closed-source; runtime registration would need to be monkey-patched or replaced. Possible? Maybe — research project, not feature.

**Verdict:** Layers 1 + 2 cover ~95% of what users want from "custom node types." The remaining 5% is deep-engine-modification territory and probably out of scope for SCT to chase.

The SCT-side answer is **meta-nodes (Layer 1) + Engine Relay bridges (Layer 2).** Both are first-class concepts in the Behavior Editor.

---

## 8. Engine Relay's Architecture — Platform and Consumers

### Architectural correction

Earlier drafts of this doc treated ER + TC as a bundled "cutscene runtime" and asked whether it should evolve into a general runtime. That framing was wrong.

**ER is not coupled to TC.** ER is a lower-level state-machine platform; TC is the first consumer. ER is *already* general by design — anyone can register a state against it.

### What ER actually provides

ER exposes a state-machine platform across three orthogonal orchestration channels:

| Channel | What ER manages | Example state |
|---|---|---|
| **Behavior** | Dynamic linking of behavior graphs onto actors | "use this behavior instead of defaultmale" |
| **Havok user state** | Custom Havok user states (CC, physics, AI gating) | "no character controller, no gravity, no AI" |
| **TESCamera** | PlayerCamera subclass with frame-by-frame transform API | "camera transform driven by external API this frame" |

A consumer **registers a state** with ER and opts into whatever subset of the three channels it needs. ER doesn't know what the consumer does — it's pure orchestration.

### True Cinematics is the first consumer

TC registers a state with ER that opts into all three channels (behavior + user state + camera). TC also does its own dynamic behavior linking at a second level — sub-behaviors per actor per scene — mirroring ER's pattern. Cutscene execution is a **two-chain of dynamically linked behaviors**:

```
ER → TC's runtime behavior → per-scene per-actor sub-behavior
```

Layer 1 (ER) and layer 2 (TC) both do the same dynamic-link trick. The pattern is composable.

### What this means for the design question

The "should ER become a general runtime" question dissolves — it already is one. The actual SCT design question is:

> **What other consumers of ER does SCT help authors create?**

Different content types want different opt-in profiles:

| Consumer type | Behavior | User state | Camera | Notes |
|---|---|---|---|---|
| **Cutscene** (TC, existing) | ✓ | ✓ | ✓ | Full lockout, full control |
| **NPC routine override** | ✓ | — | — | Custom NPC behavior, vanilla physics + no camera takeover |
| **Custom ability** | ✓ | — | — | Transient duration tied to ability equip/cast |
| **Cinematic camera mod** | — | — | ✓ | Camera-only takeover for non-scripted moments |
| **Boss-fight phase lockout** | ✓ | partial | — | Replace AI behavior + selectively gate physics |

Each is its own ER consumer registration. SCT's job is to make authoring those registrations as fluid as authoring TC scenes is today.

### Visual scripting fits this model

A SCT visual script compiles to three things:

1. **Behavior HKX** — the Havok side of the script (sub-behavior of whatever runtime layer it targets)
2. **Consumer registration** — which ER channels the script opts into
3. **Optional bridge code** — C++ for nodes that can't be expressed in Havok (world queries, papyrus calls, etc.)

The compiler determines the consumer registration from which nodes the user has placed (Camera Cut node → camera channel implied), or the user declares opt-in explicitly. Both UX patterns are viable.

### The two-chain pattern reused

Visual scripting could mirror TC's two-chain structure:

```
ER → SCT visual script runtime behavior → user-authored script sub-behavior
```

The "SCT visual script runtime" is a generic layer-1 behavior shared across all scripts; user-authored content is the layer-2 sub-behavior. Makes the runtime evolvable without breaking authored content, and is the same pattern TC already uses.

Alternatively each script registers its own top-level behavior with ER directly — less code reuse, more flexibility per script.

### Open questions

- **OPEN:** Shared layer-1 runtime for all visual scripts, or each script registers its own top-level behavior?
- **OPEN:** Does SCT only help target *existing* ER consumers (TC, future visual-script runtime), or does it help authors create *new* consumer types from scratch (e.g., "my mod ships its own runtime alongside TC")?
- **OPEN:** Behavior Editor output — constrained to {TC scene, visual script}, or also {raw behavior HKX, new consumer type definition}?
- **OPEN:** How are consumer registrations expressed in the script — implicit from node usage, explicit declaration, or per-node opt-in flags?
- **OPEN:** Conflict resolution between consumers — what happens when two consumers want to take camera at the same time? (ER's responsibility, but SCT needs to know the policy to surface it during authoring.)
- **OPEN:** Per-frame cost of ER-driven actor vs vanilla actor — when do we benchmark to validate scaling assumptions?

### What this means for sequencing

The "Phase A → B → C → D" framing in the previous draft of this section was wrong — it assumed ER itself was being built up over time, but ER is already a complete platform. The actual sequence is about which consumer types SCT supports authoring for:

| Stage | SCT-side consumer authoring |
|---|---|
| **Today** | TC scenes only |
| **Next** | Visual scripts (as a new internal ER consumer type — "SCT visual script runtime") |
| **Future** | Authoring tools for users to define their own ER consumer types |

The first transition is concrete and near-term. The second is much further out and may never be needed if the visual script consumer type is general enough.

---

## 9. Cross-Cutting Infrastructure

Six pillars, in rough order of foundationality:

1. **Document/workspace registry** — shift from single-doc AppState to a registry of open documents owned by their workspace. Each document type owns its own three-layer state (the pattern `ActorDocument` already established).

2. **Graph framework** — typed sockets, evaluation, caching, shared editor (new — for NIF, Behavior, Scene workspaces). See §4.

3. **Physics backend** — Bullet for cloth/SMP in NIF Editor; reusable for ragdoll preview in Behavior Editor.

4. **Behavior HKX I/O** — read + write `hkbBehaviorGraph` via HKBuild's HKX2E. Already partially in place; needs the read direction fleshed out for Behavior Editor.

5. **Particle system rendering** — viewport support for NiParticleSystem evaluation; reuses existing FBO renderer.

6. **Pluggable export pipelines** — each workspace has its own target (cutscene → TC bundle, NIF Editor → .nif, NPC → ESP+facegen, Behavior → behavior HKX + ER bundle, Animation → animation HKX).

### Project / file structure

**Project file vs per-artifact files.** A `.sctproject` describes "what's loaded"; individual artifacts (NIFs, HKXs, ESPs, .sctscene, .sctnpc) save as standalone files. SCT becomes the IDE; the files are the source code. Matches how mod authors actually distribute work.

**OPEN:** Documents-as-files-on-disk vs. project-bundle — confirm.

---

## 10. Master Open Questions List

Consolidated from everywhere above:

### NIF Editor
- BSEffectShader-based VFX in or out?
- Is Bullet physics integration acceptable as a multi-megabyte dependency?

### NPC Editor
- Player character editing same workspace, or separate?
- Tint layer authoring: brush/painting vs numeric only?

### Cutscene Editor
- Marker-based placement: consume only, or also create/edit?
- Trigger conditions: in-scope authoring, or external CK setup?

### Behavior Editor (visual scripting)
- Per-actor or scene-level scripts (probably both)?
- Where does a scene-level script live in the runtime stack?
- Trigger model — TC scene begin, Story Manager event, player interaction, dialogue choice, quest stage change, on-cell-load?
- Debugger surface — full UE-Blueprint-style debugger, or minimal?

### Cell/World Editor
- Confirm: deferred entirely until NIF + Behavior ship?

### Engine Relay consumer model (§8)
- Shared layer-1 runtime behavior for all visual scripts, or each script registers its own top-level behavior?
- Does SCT only help target *existing* ER consumers, or also help authors create *new* consumer types from scratch?
- Behavior Editor output — constrained to {TC scene, visual script}, or also {raw behavior HKX, new consumer type definition}?
- Consumer registration expression — implicit from node usage, explicit declaration, per-node opt-in flags?
- Conflict resolution between consumers — what happens when two want the camera channel at the same time?
- Per-frame cost of ER-driven actor vs vanilla actor — when do we benchmark?
- Community platform direction — does SCT eventually serve mod authors beyond the original user?

### Cross-cutting infrastructure
- One graph framework for all three graph workspaces, or per-workspace?
- Documents-as-files-on-disk vs. project bundle?

---

## 11. Recommended Sequencing

Once open questions resolve, natural order:

1. **Lock the document/workspace model first.** Refactor AppState into a registry; promote existing tabs to workspaces conceptually; settle project file vs per-artifact file question. Foundational — doing other workspaces first means redoing them.

2. **Build the graph framework as a horizontal pillar**, validated against one consumer first. Scene Graph panel is the lowest-stakes consumer (already on roadmap, simpler typed sockets than NIF nodes). Use it to shake out the framework, then port NIF and Behavior editors onto it.

3. **NIF Editor v0** as first major workspace. Highest leverage because it covers the most existing infrastructure, the "no good existing tool" gap is real, and the geometry-nodes payoff is differentiating. Start without physics simulation; add Bullet once the editor is usable for non-physics workflows.

4. **Behavior Editor v0** in parallel where it doesn't conflict — dependencies are mostly HKBuild-side, can move forward independently of NIF work.

5. **NPC Editor** as the highest-reuse "small scope, high user count" workspace — probably the first to feel "shippable" because it leans entirely on existing infrastructure plus the workspace pattern.

6. **Custom Creature Pipeline** comes naturally once NIF + Behavior + Animation editors exist — workflow built on top of those tools, not a separate workspace.

7. **Cutscene Editor** continues as scheduled — existing phases 6–9 don't change. Infrastructure shifts above improve it (graph framework powers Scene Graph; document model lets it coexist with other open docs).

8. **SCT-side authoring of new ER consumer registrations** — Behavior Editor learns to emit not just behavior HKX but also consumer registrations targeting ER channels. Gating step for visual scripting beyond cutscenes. ER itself needs no new work — it's already a general platform; this is purely SCT-side compiler/UI work.

9. **Cell/World Editor** revisited only after all of the above ship.

The trap to avoid: starting NIF Editor before workspace model and graph framework are settled. The geometry-nodes paradigm is too large to retrofit later.

---

## 12. Decisions Already Made

These are settled for this design (reversible if needed but not currently in question):

- **Tagline:** "The CK for animators."
- **xEdit replacement:** Not a target; touch xEdit-domain records only as workspaces require.
- **NifSkope replacement:** Full target.
- **Outfit Studio replacement:** Creation/design half only; runtime body morph distribution stays out.
- **CK competition:** Don't compete with what CK does well; replace what CK does poorly (animator-facing surface).
- **Sculpting:** Out of scope. Use Blender/ZBrush + FBX import.
- **Behavior Editor framing:** Visual scripting engine, not just an animation behavior editor.
- **ER architecture:** ER is a state-registration platform across three channels (behavior, Havok user state, TESCamera); TC is the first consumer. SCT's expansion is about helping authors target additional consumer types — *not* about evolving ER itself, which is already general by design.
- **Graph paradigm:** Unified framework across NIF, Behavior, and Scene workspaces.
- **Custom creature pipeline:** First-class workflow target.
- **Custom Havok nodes:** Layers 1 + 2 only (meta-nodes + ER bridges); Layer 4 (true new C++ node types) deemed out of reach without engine work.

---

## 13. Document Maintenance

This is a living scoping document. Edit it as the design conversation evolves:
- Move resolved open questions out of §10 and into §12 (decisions made)
- Add new open questions inline with **OPEN:** prefix
- Update workspace status in §2 as workspaces get committed/built
- Add per-workspace detail to §3 as scope sharpens

**Originated:** 2026-04-30 design session.
**Current revision:** initial draft.

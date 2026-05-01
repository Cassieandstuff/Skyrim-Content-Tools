# Motion Pipeline Research — Synthesis & Capture

> **Status:** Research artifact. Originated 2026-04-29/30 design session.
>
> Captures investigation into two candidate inputs for SCT's animation pipeline: AI motion synthesis (NVIDIA KiMoDo) and video-based motion capture (markerless / hybrid). Includes a novel concept — hybrid colored-marker mocap — that emerged from the research and appears to fill a real gap in the open-source ecosystem.
>
> **No build commitment.** Hardware budget is not currently available for the capture pipeline. This document preserves the research so the work is not lost when budget or contributors materialize. The synthesis-side work (KiMoDo wrapper, retargeter) is software-only and could begin without hardware investment.
>
> Open questions flagged inline with **OPEN:** prefixes; resolved decisions migrate to §10.

---

## 1. Scope and Why

SCT's roadmap (Phase 8 — Animation Editor) eventually needs ways to *get animation in* beyond importing existing HKX. The two candidate sources investigated here:

1. **AI motion synthesis** — generate animation from text prompts, path constraints, keyframes (KiMoDo)
2. **Video-based motion capture** — capture real performance and convert to animation (markerless, marker-based, hybrid)

Both terminate in the same downstream problem: producing Bip01 animation data ready for HKX export. The retargeting infrastructure is shared between them, which is the architectural insight that organizes this document.

**Out of scope for this research:**

- Marker-based commercial mocap (Vicon/OptiTrack — wrong price tier)
- IMU-based suits (Rokoko Smartsuit, Xsens — different problem domain, no novel research value)
- Real-time pose driving (live performance → game) — the goal is offline animation authoring
- Audio-driven animation (lip sync from audio) — covered by FaceClip annotation work

---

## 2. KiMoDo — Motion Synthesis Findings

NVIDIA's kinematic motion diffusion model. Generates 3D human animation from text prompts plus spatial constraints.

**Repo:** https://github.com/nv-tlabs/kimodo  
**Paper:** https://research.nvidia.com/labs/sil/projects/kimodo/  
**License:** Apache 2.0 code, NVIDIA Open Model License weights (commercial OK for SOMA models, R&D only for SMPL-X)

### 2.1 Inputs / Outputs

| Input | Notes |
|---|---|
| Text prompt | "A person walks cautiously forward" |
| 2D root waypoints (XZ) | Sparse path constraints — the killer feature for SCT |
| Full-body keyframes | Pin pose at specific frames |
| End-effector constraints | Hand/foot world positions |
| Foot contact patterns | Optional |

| Output | Format |
|---|---|
| `local_rot_mats` | `[T, J, 3, 3]` joint rotations parent-relative |
| `global_rot_mats` | `[T, J, 3, 3]` joint rotations world space |
| `posed_joints` | `[T, J, 3]` global joint positions |
| `root_positions` | `[T, 3]` root trajectory |
| `foot_contacts` | `[T, 4]` boolean contact flags |
| BVH export | SOMA skeleton only |

### 2.2 API Surface

- **Python API**: `from kimodo import load_model; model(prompts, num_frames, constraint_lst=[...])`
- **CLI**: `kimodo_gen` command
- **Gradio demo**: `kimodo_demo` (web UI)
- **No REST API** — would need a wrapper for out-of-process use

### 2.3 Skeleton Variants

| Model | Joints | License | Use for SCT? |
|---|---|---|---|
| Kimodo-SOMA-RP-v1.1 | 77 (full body + fingers + face) | Open Model | ✅ Recommended |
| Kimodo-SOMA-SEED-v1.1 | 77 | Open Model | ✅ Backup, smaller dataset |
| Kimodo-G1-* | 34 (Unitree G1 robot) | Open Model | ❌ Wrong morphology |
| Kimodo-SMPLX-* | 22 | R&D only | ❌ License blocks distribution |

### 2.4 Path/Waypoint Fit

This is the strong signal for SCT. The `Root2DConstraintSet` API is essentially what you'd design if asked to write a constraint format for a path editor:

```python
constraint = Root2DConstraintSet(
    skeleton=skeleton,
    frame_indices=torch.tensor([0, 50, 100, 150]),
    smooth_root_2d=torch.tensor([[0,0], [2,0.5], [4,1], [5,1.5]]),
    global_root_heading=None
)
output = model(prompts="walks forward", num_frames=150, constraint_lst=[constraint])
```

The mapping from SCT's planned path/waypoint editor is direct: drawn waypoints → `Root2DConstraintSet`. Path edits regenerate animation.

### 2.5 Hardware

| Configuration | VRAM | Latency |
|---|---|---|
| Full GPU inference | ~17 GB | 20–30 sec @ 100 diffusion steps |
| Text encoder on CPU | ~3 GB | +5–10 sec |
| Models + text encoder download | ~25 GB disk | First run only |

### 2.6 Training Infrastructure (Critical Gap)

- **Repo is inference-only.** No `train.py`, no trainer class, no training configs in version control.
- **Weights are loadable** (`.safetensors`, standard PyTorch) — fine-tuning is *possible* once a training loop is written.
- **Training data**: BONES-SEED is public (288 hours BVH @ 120fps, hosted at `huggingface.co/datasets/bones-studio/seed`). BONES-RP is proprietary (700 hours).
- **Hyperparameters** live inside each downloaded checkpoint's `config.yaml`, not in the git repo.

This means any fork-based approach (§4) requires reverse-engineering the training procedure from the technical report.

---

## 3. Transparent Retargeting Architecture

The retargeting layer sits between KiMoDo (or any tool that outputs SOMA-style skeletons) and SCT's Bip01-native data path. From SCT's perspective the wrapper looks like a native Bip01 motion source — the SOMA representation is an implementation detail.

### 3.1 Wrapper Pattern

```
SCT Path Editor
  → SkyrimMotionGenerator
      ↓ 1. Convert constraints: Skyrim space → SOMA space
      ↓ 2. KiMoDo.generate()
      ↓ 3. Retarget: SOMA 77 → Bip01 (rotations + scale)
      ↓ 4. Bip01-native AnimationData
  → HKX export
```

```python
generator = SkyrimMotionGenerator(
    skeleton_hkx="meshes/actors/character/character assets/skeleton.hkx",
    model_name="Kimodo-SOMA-RP-v1.1"
)
anim = generator.generate(
    prompt="A person walks cautiously forward",
    path_waypoints=[(0,0), (700,200), (1400,100)],  # Skyrim units
    duration_sec=5.0
)
# anim.bones["Bip01 L Thigh"] → quaternion track in Skyrim space
```

### 3.2 The Three Conversion Problems

**Scale** — SOMA in approximate meters; Skyrim in Havok units (~70/m). Waypoints in → divide. Root motion out → multiply. Trivial but critical for path fidelity.

**Rest pose compensation** — SOMA's rest pose differs from Bip01's A-pose. Direct rotation copy produces systemic offset. Correct formula operates in *global* space (which is why KiMoDo's `global_rot_mats` output matters):

```
G_bip01[j] = R_bip01_rest[j] · R_soma_rest[j]ᵀ · G_soma[j]
L_bip01[j] = G_bip01[parent(j)]ᵀ · G_bip01[j]
```

The `R_bip01_rest · R_soma_rest^T` product can be cached per joint pair — initialization cost only.

**Joint correspondence** — The hand-built table:

| Category | Treatment |
|---|---|
| 1:1 direct (spine, pelvis, neck, head) | Map with rest compensation |
| SOMA has more detail (fingers) | Sum/interpolate into available Bip01 joints |
| Bip01 has no SOMA equivalent (`Weapon`, `MagicNode`, NPC-specific) | Hold at bind pose |
| Structural mismatch (different spine count) | Distribute rotation across extra bones |

Estimated 50-60 actively-mapped entries. Modest manual work, one-time.

### 3.3 Initialization vs Per-Generation Cost

- **Init**: Read Bip01 rest pose from HKX, build correspondence table, compute cached rest-product matrices. Once per skeleton variant.
- **Per generation**: Matrix multiply pass over frame sequence. Cheap relative to model inference.

### 3.4 OPEN: Skeleton Variant Handling

Skyrim has multiple skeleton variants (default human, beast race, child, custom creature). SOMA's bone proportions are fixed (trained-in). Stride length on a beast-race character may visually differ from a human-proportion KiMoDo output even with rotation correctly retargeted. Acceptable initial behavior; could be addressed later with per-skeleton waypoint scale factor.

---

## 4. Forking KiMoDo (Native Skeleton Fine-Tune)

Architecturally cleaner than retargeting — the model would output Bip01 directly. Practically much more expensive.

### 4.1 What's Skeleton-Specific in the Model

| Component | Skeleton-specific? |
|---|---|
| Diffusion process | ❌ Reusable |
| Text encoder (LLM2Vec) | ❌ Reusable |
| Transformer denoiser bulk | ⚠️ Mostly reusable |
| Output projection layer | ✅ Hardcoded to 77 joints |
| Forward kinematics layer | ✅ Encodes SOMA hierarchy |
| Skeleton conditioning embedding | ✅ Bone lengths, chain topology |
| Foot contact head | ❌ Mostly reusable (4 values) |

The transformer's learned motion priors (text understanding, path following, temporal coherence) live in layers that are *not* skeleton-specific. This is what makes a fine-tune viable rather than a full retrain.

### 4.2 Fine-Tune Approach

1. Replace `SOMASkeleton77` with `BipSkeleton`
2. Re-initialize output projection for Bip01 joint count
3. Build training data via the retargeter from §3 (data pipeline application)
4. Fine-tune with most of the transformer frozen

**LoRA option**: Add LoRA adapters to attention layers + re-init output head. Lowest compute. Could ship LoRA weights as a small add-on to base KiMoDo install.

### 4.3 Training Data Sources

- **BONES-SEED retargeted to Bip01** — 288 hours, public, BVH @ 120fps
- **Existing Skyrim HKX animations** — game animations (not mocap), but teach Skyrim-specific motion aesthetics (sneak cycles, combat idles, two-handed carry)

### 4.4 Blockers

- **No training code shipped.** Reverse-engineering the diffusion training loop from the technical report and inference code is the major unknown.
- **GPU compute** for fine-tuning: A100-class for full fine-tune, possibly tractable on consumer GPU for LoRA adapters only.
- **Maintenance burden** — KiMoDo is actively developed; fork must merge upstream.

### 4.5 Recommendation

Fine-tune is a *Phase 2+ option*, not a starting point. Build the retargeter first; the retargeter doubles as the data pipeline if the fine-tune is later pursued.

---

## 5. Markerless Mocap Landscape (early 2026)

Surveyed for the eventual multi-actor video capture goal. Landscape moves fast; this snapshot is current as of the research session.

### 5.1 Tool Comparison

| Tool | Cameras | Multi-person | Output skeleton | License | Verdict |
|---|---|---|---|---|---|
| **4D-Humans + PHALP** | Single | ✅ ID-tracked | SMPL | MIT | **Best multi-actor pick** |
| **MeTRAbs** | Single | ⚠️ Per-frame, no ID | 23+ formats inc. SMPL/COCO/H36M | MIT code / NC models | Strong single-actor, fast |
| **TRAM** | Single | ✅ | SMPL + global trajectory | MIT | Best for world-grounded motion |
| **TRACE** | Single | ✅ One-stage | SMPL | Research | Strong dynamic-camera multi-person |
| **WHAM** | Single | Single-focus | SMPL | Open | Fast, foot contact |
| **GVHMR** | Single | Single-focus | SMPL gravity-view | Open | Single-camera world-grounded |
| **SLAHMR** | Single | ✅ | SMPL | MIT | Too slow (~200 min / 1000 frames) |
| **FreeMoCap** | Multi | ⚠️ Limited | MediaPipe-based | AGPL-3.0 | Hobbyist, single-actor focused |
| **EasyMocap** | Multi-view | ✅ MVMP | SMPL family | Open | Research multi-view multi-person |
| **Move.ai Pro** | Multi | ✅ | FBX | Commercial ($$$$) | Production, out of price range |
| **Rokoko Vision** | 1–2 | ❌ | FBX/BVH | Commercial | Single-actor only |
| **DeepMotion Animate 3D** | Single | ✅ paid tier | FBX/BVH/GLB | Commercial credits | Prosumer cloud |

### 5.2 Multi-Actor Reality Check

- **Typical occlusions handled** — PHALP's 3D appearance prediction does well for one actor walking behind another
- **Close interaction still fails** — hugging, grappling, dancing in contact remain open problems. The Harmony4D dataset (2024) was created specifically to address this gap; current methods produce ~5cm avg vertex error even after fine-tuning on it.
- **ID switches** — long occlusions or similar-clothing actors cause swaps. Not fully solved by any tool.
- **Foot sliding** — present in all monocular methods. WHAM/TRAM mitigate via foot contact prediction. None eliminate.

### 5.3 The Bottleneck

SMPL → Bip01 retargeting is the same problem as SOMA77 → Bip01 retargeting (§3). **The retargeter built for KiMoDo serves the mocap pipeline too.** This is the architectural insight — SCT's animation input layer has one retargeting infrastructure regardless of whether motion comes from synthesis or capture.

---

## 6. Novel Concept — Hybrid Colored-Marker Mocap

Emerged from the design conversation. Concept attribution: tool author (this session). Documented here because prior-art search confirms this combination does not exist in productized form, in either open source or sub-$5k commercial tiers.

### 6.1 The Concept

A markerless-style mocap rig that solves the multi-actor identity problem deterministically by giving each actor a different *color* of physical marker — red pearls/balls for actor 1, blue for actor 2, etc. — captured by a ring of consumer RGB webcams and triangulated to 3D. Optional hybrid: combine with ML pose estimation where markers serve as drift-free identity-anchored points and ML fills in joints between markers.

### 6.2 Why It's Architecturally Sound

- **Identity by construction** — no tracker, no PHALP, no ID switches. Color discrimination ≡ actor identity.
- **Bip01-direct output** — IK from marker positions to Bip01 bones skips SMPL entirely. No retargeting.
- **All math is solved** — multi-view triangulation, ChArUco calibration, HSV color thresholding, IK skeleton fitting all in OpenCV (BSD) and well-documented since the 1990s.
- **Hardware curve collapsed recently** — 6–12 USB webcams + a gaming PC is now in modder budget range. Was not 5 years ago.

### 6.3 Architecture

```
6–8 webcams in ring around capture volume
    ↓
ChArUco calibration → camera intrinsics + extrinsics
    ↓
Per frame, per camera:
    HSV threshold per pearl color → 2D blob centroids
    ↓
Multi-view triangulation → 3D position per pearl per frame
    ↓
T-pose calibration → pearl-to-bone offset table per actor
    ↓
IK solve (Bip01 directly) → joint rotations
    ↓ (optional)
Hybrid: blend ML pose estimate (4D-Humans) for joints between markers
    ↓
Temporal smoothing (1€ filter)
    ↓
Output: per-actor Bip01 animation, identity correct by construction
```

### 6.4 Hybrid Sweet Spot

| Pearls give | ML estimation gives |
|---|---|
| Sub-cm precision at marker locations | Full-body coverage between markers |
| Rock-solid identity | Skeleton structure inference |
| Zero ML inference for tracked points | Recovery when pearls occlude |
| No drift over time | Reasonable estimates from one camera |

Pearls placed only at ambiguous joints (wrists, ankles, head, sacrum, shoulders). ML fills elbows, knees, spine. Pearls anchor the ML estimate to absolute world positions — eliminates drift and provides identity. Documented in research literature as "sparse marker-augmented pose estimation" but **no widely-used open-source implementation exists**.

### 6.5 Prior Art (Search Findings)

Closest existing projects, all missing critical pieces:

| Project | Same as concept? | Missing |
|---|---|---|
| Joshua Bird's Low-Cost-Mocap | Rig topology | IR LED markers, tracks drones not humans, no per-actor ID |
| DeMoCap (IJCV 2021) | Marker-based learning | Uses RealSense depth, retroreflective markers, single actor |
| SnowMocap | RGB webcams + Blender | Pure markerless |
| FreeMoCap | Multi-cam ChArUco | Pure markerless, single-actor focused |
| Pose2Sim, EasyMocap, Caliscope | Multi-cam | All pure markerless |
| OpenMoCap, SnakeStrike | Color thresholding markers | Single-subject animal/specimen rigs |
| OpenCap "marker enhancer" | Markers + ML hybrid | Software-only, virtual markers, no physical anchors |
| RoMo (arxiv 2410.02788) | Color ball labeling | Inputs are Vicon/OptiTrack-grade, not webcams |
| Vicon "Fusing Marker and Markerless" | Hybrid concept | Commercial pitch, $$$$, closed |
| PhaseSpace Impulse X2E | Per-marker ID | Active LED, frequency modulation, $$$$ |

**Verdict:** The exact combination — RGB webcams + per-actor colored ball ID + ChArUco + optional ML hybrid as drift-anchored backbone — appears unbuilt in open source and absent from sub-$5k commercial. Hybrid-anchor research direction essentially unexplored in indie-accessible pipeline.

### 6.6 Why The Gap Exists

Considered honestly:

1. **Incentive misalignment** — Academia rewards generality (markerless on any video). Commercial mocap defends premium pricing tiers. Open-source ML hobbyists chase pure markerless because that's what gets stars. Nobody's incentivized to build the unglamorous middle.
2. **Cultural prejudice against "old" CV** — Marker-based + HSV thresholding feels like 1995. Doesn't get fresh papers/blogs. ML community considers it boring. But "boring + solved + open" is exactly what reliability-critical applications need.
3. **Economics only recently aligned** — 6–8 quality USB webcams + compute became modder-affordable in roughly the last 3–4 years.
4. **Cross-domain synthesis is rare** — Arriving at this requires simultaneously knowing: that 4D-Humans/PHALP exists, that multi-actor identity is the actual hard problem, that OpenCV color tracking is reliable, that PhaseSpace exists but is closed, and that the modder community has this specific budget bracket.

### 6.7 Hardware Budget Tiers

Documented for future planning even though current budget is $0 for hardware.

**Tier 1 — Proof-of-concept bootstrap: $600–900**
- 6× Logitech C920 (used market): $250–360
- USB 3.0 PCIe expansion: $50
- Powered USB hubs ×2: $40
- Tripods or PVC rig: $80–150
- Basic LED panel lights ×2: $80–120
- Painted ping-pong balls + Velcro + compression suit: $50–80
- ChArUco home-printed: $15

Validates concept with 1–2 actors at moderate motion speeds. Software sync drift (~10–15ms) limits fast-motion fidelity.

**Tier 2 — Solid hobbyist/modder grade: $1,800–2,800**
- 8× Arducam/ELP USB cameras with hardware trigger, 60fps@1080p: $640–1,200
- Trigger box (Arduino DIY): $40–80
- 2× USB 3.0 PCIe cards: $100
- Aluminum extrusion ring rig: $200–400
- LED panel lighting ×4: $250–400
- Quality matte foam markers + custom Velcro suits ×2: $200–350
- Quality ChArUco board (laminated): $80
- Backdrop muslin: $100
- Storage upgrade (fast SSD): $150

Reliable multi-actor capture at game-relevant motion speeds. Production-quality animation suitable for distribution as part of an SCT-built mod.

**Tier 3 — Pro-adjacent: $9,000–16,000**
Listed for completeness only. Beyond this competes with Vicon used market.

### 6.8 Hidden Gotchas

- **USB bandwidth** — Each USB 3.0 controller saturates at 2–3 high-bitrate cameras. PCIe USB expansion non-optional past 4 cameras.
- **Lighting consistency, not brightness** — Color discrimination breaks under uneven light. Two matched LED panels beat one expensive light. Avoid windows.
- **Calibration is per-session if cameras move** — Spend on rigidity (PVC sags; aluminum extrusion doesn't) before camera count.
- **Marker placement repeatability** — Velcro-on-skin shifts. Compression suits with sewn-in marker pockets are night-and-day better.
- **PC not free** — Running 4D-Humans + KiMoDo together needs 12–24 GB VRAM. RTX 3060 12GB (~$300 used) is the floor.

### 6.9 OPEN: Active Marker Variant

LEDs blinking unique temporal patterns (PhaseSpace's commercial trick) instead of static colored balls. Infinite "color space" via temporal codes; lighting variation stops mattering. Needs global shutter cameras at 240fps+ and microcontroller-per-marker. Adafruit-tractable for a research-grade build but adds significant DIY scope.

---

## 7. The Shared Bottleneck — Architectural Insight

The retargeter from §3 is the shared dependency between every input source SCT might support:

```
KiMoDo synthesis     ─┐
4D-Humans mocap      ─┤
MeTRAbs mocap        ─┼─→ SMPL/SOMA → Bip01 retargeter ─→ HKX export
TRAM mocap           ─┤
Hybrid colored mocap ─┘  (or direct to Bip01, skipping retargeter)
```

The hybrid colored-marker pipeline (§6) skips the retargeter entirely — IK solves directly to Bip01. Every other input source needs the retargeter.

**Implication:** The retargeter is the most leveraged piece of software in the motion pipeline. Building it once unblocks every input source.

---

## 8. Forward Path

What can move now (zero hardware cost):

- ✅ Build the SOMA77 → Bip01 retargeter (§3). Software-only. Unblocks everything else.
- ✅ Build a thin Python-side wrapper for KiMoDo that produces BVH or NPZ. Validates path/waypoint workflow even before retargeter exists.
- ✅ Document SMPL → Bip01 mapping (different correspondence table; same architecture).

What requires hardware budget (deferred):

- ⏸️ Multi-camera capture rig for hybrid colored-marker concept (§6)
- ⏸️ Validation of hybrid pipeline end-to-end
- ⏸️ Skyrim-specific motion dataset capture for fine-tune option (§4)

What requires GPU upgrade or cloud compute:

- ⏸️ KiMoDo fine-tune / LoRA on Bip01 (Phase 2+ option)
- ⏸️ Local 4D-Humans inference at production scale

---

## 9. Open Questions

- **OPEN: §3.4** Skeleton variant handling — beast race, child skeleton, custom creatures. Per-skeleton waypoint scale factor or accept stride mismatch?
- **OPEN: §6.9** Active marker variant (LED + temporal codes) vs. static colored balls. DIY scope creep tradeoff.
- **OPEN:** Where does the wrapper live? Standalone Python service called over IPC? Embedded via DotNetHost-style bridge? .NET ML.NET reimplementation? (Likely Python service — KiMoDo is PyTorch.)
- **OPEN:** Distribution model for KiMoDo integration. KiMoDo is ~25 GB download. Bundle? On-demand fetch? Optional plugin?
- **OPEN:** Licensing audit for distribution. Apache 2.0 KiMoDo code is fine. NVIDIA Open Model License needs notice. SMPL-X models excluded entirely. 4D-Humans (MIT) fine. FreeMoCap (AGPL-3.0) avoided.
- **OPEN:** Hybrid-mocap publication strategy. The concept is novel enough to warrant a writeup; whether a paper, blog post, or just open-source release is the right format depends on contributor goals.

---

## 10. Decision Log

(Populated as decisions are made. Currently empty — this is a research artifact, not a build plan.)

---

## Appendix A — Tool Repos & References

### Motion Synthesis
- KiMoDo: https://github.com/nv-tlabs/kimodo
- KiMoDo paper: https://research.nvidia.com/labs/sil/projects/kimodo/
- BONES-SEED dataset: https://huggingface.co/datasets/bones-studio/seed
- LLM2Vec text encoder: McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp

### Markerless Mocap
- 4D-Humans: https://github.com/shubham-goel/4D-Humans
- PHALP: https://github.com/brjathu/PHALP
- MeTRAbs: https://github.com/isarandi/metrabs
- TRAM: https://github.com/yufu-wang/tram
- TRACE: http://www.yusun.work/TRACE/
- WHAM: https://github.com/yohanshin/WHAM
- GVHMR: https://github.com/zju3dv/GVHMR
- FreeMoCap: https://github.com/freemocap/freemocap
- EasyMocap: https://github.com/zju3dv/EasyMocap

### Hybrid Concept Adjacent
- Joshua Bird Low-Cost-Mocap: https://github.com/jyjblrd/Low-Cost-Mocap
- DeMoCap: https://github.com/tofis/democap
- OpenCap marker enhancer: https://pmc.ncbi.nlm.nih.gov/articles/PMC12198419/
- RoMo: https://arxiv.org/html/2410.02788v1
- PhaseSpace Impulse X2E: https://phasespace.com/x2e-motion-capture/

### Multi-Person Research
- Harmony4D (close-interaction dataset): https://arxiv.org/html/2410.20294v1
- MAMMA (2025 multi-person markerless): https://arxiv.org/html/2506.13040v2
- Benchmarking 3D HPE under occlusions (2025): https://arxiv.org/html/2504.10350v2

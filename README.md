# FleshRing Plugin

[![English](https://img.shields.io/badge/lang-English-blue.svg)](README.md) [![한국어](https://img.shields.io/badge/lang-한국어-red.svg)](README_KR.md)

> **GPU-accelerated flesh compression simulation for Unreal Engine 5.7**

FleshRing creates realistic skin deformation when tight objects—bands, straps, garters, stockings—press against character meshes. Using Compute Shaders and Signed Distance Fields (SDF), it delivers production-quality results with zero runtime cost through a bake-based workflow.

![FleshRing Effect](Docs/Images/FleshRing_Effect.gif)

---

## Key Features

- **Bake-Based Workflow** — Design in editor, deploy with zero runtime GPU cost
- **SDF-Driven Deformation** — Precise compression matching actual ring mesh shape
- **Tightness & Bulge Effects** — Realistic flesh compression with natural spillover
- **Advanced Smoothing** — Laplacian, Taubin, PBD constraints for artifact-free results
- **Material Layer Control** — Per-material inclusion/exclusion via layer masking
- **Multiple Rings** — Unlimited compression points per asset
- **Virtual Ring/Band Modes** — Quick setup without custom meshes
- **Modular Character Support** — Works with UE modular skeletal mesh systems
- **Real-time Editor Preview** — Instant GPU-powered feedback while editing
- **Dedicated Asset Editor** — 3D viewport, transform gizmos, skeleton bone tree
- **Full Blueprint & C++ API** — Runtime asset swapping supported

---

## Available on FAB

Download from FAB Marketplace:
👉 [FleshRing on FAB](https://www.fab.com/ko/portal/listings/4aa9ceae-7c7d-4e70-b6fb-3d5f28dcf2f6/preview)

---

## Requirements

| | |
|---|---|
| **Engine** | Unreal Engine 5.7 |
| **Platform** | Windows 64-bit (Win64) |
| **GPU** | Compute Shader support (SM 5.0+) |

---

## Installation

1. **Locate** your project's root folder (where `YourProject.uproject` is located)
2. **Create** a `Plugins` folder if it doesn't exist
3. **Copy** the `FleshRingPlugin` folder into `Plugins/`
4. **Launch** Unreal Editor → `Edit → Plugins` → enable **FleshRing**
5. **Restart** the editor when prompted

---

## Quick Start

1. **Create Asset:** Right-click Content Browser → `Miscellaneous → FleshRing Asset`
    ![Step 1](Docs/Images/QuickStart_01_CreateAsset.png)

2. **Set Target:** In Details panel → **Target** category → set **Target Skeletal Mesh**

3. **Add Ring:** In Skeleton Tree panel → right-click a bone → `Add Ring to [BoneName]`
    ![Step 3](Docs/Images/QuickStart_03_AddRing.png)

    ![Step 3](Docs/Images/QuickStart_03_AddRing_1.png)

4. **Configure:** In Details panel → **Ring** category → adjust **Tightness Strength**, **Bulge Intensity**

5. **Bake:** In Toolbar → click **"Bake"** button (required for runtime!)
    ![Step 5](Docs/Images/QuickStart_05_GenerateBake.png)

    > **Important:** Without baking, editor preview won't appear at runtime.

6. **Apply:** Add **Flesh Ring** component (search "FleshRing") to your character Blueprint → set **Flesh Ring Asset**

### Result

![Before/After Result](Docs/Images/Result_BeforeAfter.gif)



---

## Documentation

For detailed parameter reference, tutorials, and troubleshooting:

**[Full Technical Documentation](Docs/FleshRing_Documentation.md)**

---

## Support

| | |
|---|---|
| **Documentation** | See `Docs/FleshRing_Documentation.md` |
| **Email** | kraftontechlablcg@gmail.com |
| **Bug Reports** | Submit via FAB product page |

---

## License

MIT License — Free for personal and commercial use.

Distributed through FAB Marketplace.

---

**Developed by LgThx**

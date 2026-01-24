# FleshRing Plugin Documentation

## Overview
FleshRing is a GPU-accelerated mesh deformation plugin for Unreal Engine that simulates realistic flesh compression effects. Using Compute Shaders and Signed Distance Fields (SDF), it creates believable skin deformation when objects like bands, straps, or tight clothing press against character meshes. This plugin is ideal for character customization systems, fashion simulation, and any scenario requiring realistic soft-body interaction with skeletal meshes.

## Features

### GPU-Accelerated Deformation Pipeline
- All deformation calculations run entirely on the GPU using Compute Shaders
- Real-time performance suitable for games and interactive applications
- Optimized partial skinning for affected vertices only
- Per-Ring dirty flag system for selective updates

### SDF-Based Influence Calculation
- Generates 3D Signed Distance Fields from Ring meshes
- Accurate distance-based influence falloff
- Supports multiple falloff curves: Linear, Quadratic, Hermite (S-Curve)
- Oriented Bounding Box (OBB) optimization for efficient sampling

### Tightness Effect
- Pulls vertices toward the Ring center based on SDF distance
- Configurable influence radius and strength
- Per-vertex weight interpolation for smooth transitions
- Supports bone-weighted deformation preservation

### Bulge Effect
- Creates realistic flesh displacement around compressed areas
- Automatic or manual direction detection (positive/negative/bidirectional)
- Exponential falloff from Ring boundaries
- Independent control for bulge amount and radius

### Laplacian Smoothing
- Removes harsh deformation artifacts
- Taubin λ-μ algorithm option to prevent mesh shrinkage
- Configurable iteration count and smoothing strength
- Topology-based hop distance blending at boundaries

### PBD Edge Constraints
- Position-Based Dynamics for volume preservation
- Maintains edge lengths during deformation
- Prevents mesh collapse under heavy compression
- GPU-parallel constraint solving

### Normal & Tangent Recomputation
- Geometric normal calculation from deformed faces
- Surface rotation method preserving original smooth normals
- Gram-Schmidt tangent orthonormalization
- Hop-based blending for seamless boundary transitions

### Layer Penetration Resolution
- Automatic material layer detection (Skin, Stocking, Underwear, Outerwear)
- GPU-based penetration solving between layers
- Configurable layer ordering and spacing
- Prevents clothing clipping through skin

### Multiple Ring Support
- Add unlimited Rings per component
- Independent settings for each Ring
- Ring hierarchy with transform gizmos
- Selective Ring update optimization

### Influence Modes
- **Mesh Based**: SDF calculated from actual Ring mesh geometry
- **Virtual Ring**: Manual radius specification without mesh
- **Virtual Band**: Capsule-shaped influence for stockings/bands

### Editor Tools
- Dedicated FleshRing Asset Editor with 3D preview viewport
- Interactive Ring transform gizmos (position, rotation, scale)
- Real-time deformation preview in editor
- Skeleton bone hierarchy tree view
- Material layer mapping UI
- SDF slice visualization for debugging

### Blueprint Integration
- Full Blueprint support for runtime manipulation
- Exposed properties for animation and gameplay systems
- Runtime asset swapping without animation interruption
- Event delegates for deformation state changes

## Installation

### Requirements
- Unreal Engine 5.5 or later
- Windows operating system (DirectX 11/12)
- GPU with Compute Shader support (SM 5.0+)

### Installation Steps
1. Download the FleshRing plugin package
2. Extract to your project's `Plugins` folder
3. The folder structure should be: `YourProject/Plugins/FleshRingPlugin/`
4. Open your Unreal Engine project
5. Enable the plugin via Edit > Plugins > FleshRing
6. Restart the Unreal Editor when prompted
7. Verify installation by checking Window > FleshRing category

## Usage

### Quick Start
1. Create a FleshRing Asset: Right-click in Content Browser > FleshRing > FleshRing Asset
2. Assign your target Skeletal Mesh in the Asset's Target property
3. Add a Ring by clicking the "Add Ring" button in the Asset Editor
4. Configure the Ring's bone attachment, position, and mesh
5. Adjust Tightness, Bulge, and Smoothing parameters
6. **Click "Generate Bake" button** to create the deformed mesh for runtime use
7. Save the Asset (Bake is also auto-generated on save)
8. Add FleshRing Component to your character Blueprint
9. Assign the FleshRing Asset to the component

> **Important**: The deformation is only applied at runtime after baking. Without baking, changes made in the editor will not appear in-game.

### Creating a FleshRing Asset
1. Right-click in Content Browser
2. Navigate to FleshRing > FleshRing Asset
3. Name your asset (e.g., "FRA_CharacterBase")
4. Double-click to open the FleshRing Asset Editor

### Asset Editor Workflow
1. **Set Target Mesh**: Assign the Skeletal Mesh to deform
2. **Add Rings**: Each Ring represents a compression point
3. **Configure Ring Settings**:
   - Attached Bone: The bone the Ring follows
   - Ring Mesh: Static mesh defining the compression shape
   - Influence Mode: Mesh Based, Virtual Ring, or Virtual Band
4. **Adjust Deformation Parameters**:
   - Tightness: Compression strength (0-1)
   - Bulge Amount: Flesh displacement magnitude
   - Smoothing Iterations: Surface smoothing passes
5. **Preview**: Use the 3D viewport to see real-time results
6. **Generate Subdivision** (Optional): Click "Generate" for higher mesh detail in affected areas
7. **Generate Bake**: Click "Generate Bake" button to bake the deformed mesh
   - This creates a pre-deformed Skeletal Mesh stored within the Asset
   - The baked mesh is used at runtime for optimal performance
   - Re-bake whenever you modify Ring settings

### Baking Workflow (Critical for Runtime)
The FleshRing plugin uses a **bake-based workflow**:

1. **Editor Preview**: Real-time GPU deformation for instant feedback while editing
2. **Bake**: Converts GPU-computed deformation into a static Skeletal Mesh
3. **Runtime**: Uses the baked mesh instead of real-time GPU computation

**Why Baking?**
- Zero runtime GPU overhead for deformation
- Consistent results across all hardware
- Animation system compatibility
- Reduced memory bandwidth

**When to Rebake:**
- After modifying any Ring transform (position, rotation, scale)
- After changing Tightness, Bulge, or Smoothing values
- After adding or removing Rings
- The editor will indicate when rebaking is needed

### Runtime Setup
```cpp
// C++ Example
UFleshRingComponent* FleshRingComp = CreateDefaultSubobject<UFleshRingComponent>(TEXT("FleshRing"));
FleshRingComp->FleshRingAsset = MyFleshRingAsset;
FleshRingComp->ApplyAsset();
```

```
// Blueprint: Add FleshRing Component to your Actor
// Set FleshRing Asset property
// Call ApplyAsset() if changed at runtime
```

### Runtime Asset Swapping
```cpp
// Swap between different FleshRing configurations
FleshRingComp->SwapFleshRingAsset(NewFleshRingAsset);
```

## Technical Details

### Module Structure
```
FleshRingPlugin/
├── Source/
│   ├── FleshRingRuntime/       # Runtime module
│   │   ├── Public/
│   │   │   ├── FleshRingComponent.h
│   │   │   ├── FleshRingAsset.h
│   │   │   ├── FleshRingTypes.h
│   │   │   ├── FleshRingDeformer.h
│   │   │   ├── FleshRingDeformerInstance.h
│   │   │   └── ... (40+ header files)
│   │   └── Private/
│   │       └── ... (implementation files)
│   └── FleshRingEditor/        # Editor module
│       ├── Public/
│       │   ├── FleshRingEditor.h
│       │   ├── FleshRingAssetFactory.h
│       │   └── ... (customization headers)
│       └── Private/
│           ├── FleshRingAssetEditor.cpp
│           ├── SFleshRingEditorViewport.cpp
│           └── ... (editor implementations)
├── Shaders/                    # HLSL Compute Shaders
│   ├── FleshRingTightnessCS.usf
│   ├── FleshRingBulgeCS.usf
│   ├── FleshRingLaplacianCS.usf
│   ├── FleshRingPBDEdgeCS.usf
│   ├── FleshRingNormalRecomputeCS.usf
│   ├── FleshRingSkinningCS.usf
│   ├── FleshRingSDFGenerate.usf
│   └── ... (15+ shader files)
├── Resources/
│   └── Icon128.png
└── FleshRingPlugin.uplugin
```

### Key Classes

| Class | Description |
|-------|-------------|
| `UFleshRingAsset` | Data asset storing Ring configurations |
| `UFleshRingComponent` | Actor component managing deformation |
| `UFleshRingDeformer` | Mesh Deformer interface for UE's deformation system |
| `UFleshRingDeformerInstance` | Per-instance deformation state |
| `FFleshRingSettings` | Per-Ring configuration struct |
| `FFleshRingAffectedVerticesManager` | Manages affected vertex selection |

### Compute Shader Pipeline

The deformation pipeline executes in the following order:

1. **Skinning CS**: Transforms vertices to world space (partial, affected only)
2. **SDF Generation**: Creates distance field from Ring mesh
3. **Tightness CS**: Applies compression deformation
4. **Bulge CS**: Adds flesh displacement
5. **Laplacian CS**: Smooths deformation artifacts
6. **PBD Edge CS**: Applies edge length constraints
7. **Layer Penetration CS**: Resolves clothing layer intersections
8. **Normal Recompute CS**: Recalculates vertex normals
9. **Tangent Recompute CS**: Updates tangent vectors

### Dependencies
- **ProceduralMeshComponent**: Used for editor visualization
- **RenderCore**: Required for Compute Shader dispatch
- **RHI**: Low-level rendering interface

### Performance Considerations
- Deformation cost scales with affected vertex count, not total mesh vertices
- SDF resolution affects quality vs. performance (default: 64³)
- Laplacian iterations: 3-5 recommended for balance
- PBD iterations: 2-4 sufficient for most cases
- Use "Generate Baked Mesh" for static poses to eliminate runtime cost

## Troubleshooting

### Common Issues

1. **No visible deformation at runtime**
   - **Most common cause**: Baked mesh not generated - click "Generate Bake" in Asset Editor
   - Verify FleshRing Asset is assigned to the component
   - Check that Target Skeletal Mesh matches the character's mesh
   - Ensure at least one Ring is configured with valid settings
   - Confirm Tightness value is greater than 0

2. **Deformation visible in editor but not in game**
   - This means baking was not performed
   - Open the FleshRing Asset Editor and click "Generate Bake"
   - Save the asset after baking
   - Note: Saving the asset also triggers auto-bake

3. **Mesh artifacts or spikes**
   - Increase Smoothing Iterations
   - Enable PBD Edge Constraints
   - Check for non-manifold geometry in source mesh
   - Verify Ring mesh is properly closed

4. **Performance issues**
   - Reduce SDF Resolution (32 or 48 instead of 64)
   - Decrease Smoothing/PBD iterations
   - Use "Generate Subdivision" to limit affected area
   - Consider baking for static configurations

5. **Clothing clipping through skin**
   - Enable Layer Penetration Resolution in Asset settings
   - Verify Material Layer Mappings are correctly assigned
   - Adjust layer spacing values

6. **Ring not following bone correctly**
   - Check Attached Bone name matches skeleton
   - Verify bone exists in target Skeletal Mesh
   - Use Asset Editor's bone tree to select correct bone

### Debug Visualization
- Enable "Show Debug Points" in component settings
- Use SDF Visualizer Blueprint function to inspect distance fields
- Check Output Log for FleshRing-related warnings

### Support
For technical support and bug reports, please contact the developer or submit issues through the designated support channel.

## Version History

### Version 1.0.0
- Initial release
- Core deformation pipeline (Tightness, Bulge, Smoothing)
- SDF-based influence calculation
- Multiple Ring support
- Editor tools (Asset Editor, Viewport, Gizmos)
- Blueprint integration
- Layer penetration resolution
- Normal/Tangent recomputation

## License
This plugin is provided under the MIT License. You are free to use, modify, and distribute this plugin for both personal and commercial projects. See the LICENSE file for full terms.

## Credits
- Developed by: LgThx
- Engine: Unreal Engine 5.5+

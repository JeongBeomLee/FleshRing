// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingFalloff.h"
#include "FleshRingTypes.generated.h"

class UStaticMesh;
class USkeletalMesh;

// =====================================
// Enum Definitions
// =====================================

/** Ring selection type (for editor) */
UENUM()
enum class EFleshRingSelectionType : uint8
{
	None,		// No selection
	Gizmo,		// Ring gizmo selection (move + scale to adjust radius)
	Mesh		// Ring mesh selection (mesh move/rotate)
};

/** Virtual Band section type (for individual editing) */
enum class EBandSection : uint8
{
	None,		// No section selected (entire band)
	Upper,		// Upper cap (Upper.Radius, Upper.Height)
	MidUpper,	// Band upper boundary (MidUpperRadius)
	MidLower,	// Band lower boundary (MidLowerRadius)
	Lower		// Lower cap (Lower.Radius, Lower.Height)
};

/** Ring influence range determination method */
UENUM(BlueprintType)
enum class EFleshRingInfluenceMode : uint8
{
	/** Mesh-based influence range calculation (SDF) */
	MeshBased	UMETA(DisplayName = "Mesh Based"),

	/** Manual Radius specification (virtual ring) */
	VirtualRing	UMETA(DisplayName = "Virtual Ring"),

	/** Virtual band (virtual frame for stockings/tights) */
	VirtualBand	UMETA(DisplayName = "Virtual Band"),

	/** [DEPRECATED] Auto renamed to MeshBased, kept for legacy asset compatibility */
	Auto = MeshBased	UMETA(Hidden)
};

/** Falloff curve type */
UENUM(BlueprintType)
enum class EFalloffType : uint8
{
	/** Linear falloff (sharp boundary) */
	Linear		UMETA(DisplayName = "Linear"),

	/** Quadratic curve (smooth) */
	Quadratic	UMETA(DisplayName = "Quadratic"),

	/** S-curve (smoothest, recommended) */
	Hermite		UMETA(DisplayName = "Hermite (S-Curve)")
};

/** Smoothing volume selection mode */
UENUM(BlueprintType)
enum class ESmoothingVolumeMode : uint8
{
	/** Z-axis bounds expansion (uses SmoothingBoundsZTop/Bottom) */
	BoundsExpand	UMETA(DisplayName = "Bounds Expand (Z)"),

	/** Topology-based hop propagation (from seed to N hops) */
	HopBased		UMETA(DisplayName = "Depth-Based (Topology)")
};

/** Laplacian smoothing algorithm type */
UENUM(BlueprintType)
enum class ELaplacianSmoothingType : uint8
{
	/** Standard Laplacian (shrinks over iterations) */
	Laplacian	UMETA(DisplayName = "Standard"),

	/** Taubin lambda-mu smoothing (prevents shrinkage) */
	Taubin		UMETA(DisplayName = "Volume Preserving")
};

/** Bulge direction mode */
UENUM(BlueprintType)
enum class EBulgeDirectionMode : uint8
{
	/** Auto-detect via SDF boundary vertices (bidirectional for closed meshes) */
	Auto		UMETA(DisplayName = "Auto (Boundary Detection)"),

	/** Bidirectional Bulge (donut-shaped ring, closed mesh) */
	Bidirectional	UMETA(DisplayName = "Bidirectional (Both)"),

	/** Force +Z direction (upward) */
	Positive	UMETA(DisplayName = "Positive (+Z)"),

	/** Force -Z direction (downward) */
	Negative	UMETA(DisplayName = "Negative (-Z)")
};

/**
 * Mesh layer type (clothing hierarchy)
 * Auto-detected from material name or manually specified
 * Used for layer penetration resolution on GPU
 *
 * NOTE [Migration]: UE enum serialization is name-based!
 *       - Changing value order breaks existing assets
 *       - Changing names also breaks existing assets (stored by name)
 *       - Always add new types at the end
 *       - When renaming, keep old name as Hidden alias
 *       (Unknown renamed to Other, Unknown = Other kept as alias)
 */
UENUM(BlueprintType)
enum class EFleshRingLayerType : uint8
{
	/** Skin/flesh layer (innermost, pushes out other layers on penetration) */
	Skin		UMETA(DisplayName = "Skin (Base Layer)"),

	/** Stocking/tights layer (directly above skin, always outside skin) */
	Stocking	UMETA(DisplayName = "Stocking"),

	/** Underwear layer (above stocking) */
	Underwear	UMETA(DisplayName = "Underwear"),

	/** Outerwear layer (outermost) */
	Outerwear	UMETA(DisplayName = "Outerwear"),

	/** Other/unclassified (default when auto-detection fails, control via AffectedLayerMask) */
	Other		UMETA(DisplayName = "Other"),

	/**
	 * Exclude (no tightness effect applied)
	 * Always excluded regardless of AffectedLayerMask
	 * Use for materials that don't need tightness: eyes, hair, accessories, etc.
	 */
	Exclude		UMETA(DisplayName = "Exclude (Never Affected)"),

	/**
	 * [DEPRECATED] Unknown renamed to Other
	 * Kept for legacy asset deserialization compatibility (UE uses name-based serialization)
	 * Hidden in editor, use Other for new assets
	 */
	Unknown = Other	UMETA(Hidden)
};

/**
 * Layer selection bitmask (specifies layers affected by tightness)
 * Multiple layers can be selected simultaneously (e.g., Skin | Stocking)
 *
 * NOTE [Migration]: Adding/changing bits affects existing asset AffectedLayerMask values!
 *       Migration code in PostLoad() required when adding new bits.
 *       (Other bit added + PostLoad migration implemented)
 */
UENUM(BlueprintType, Meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EFleshRingLayerMask : uint8
{
	None       = 0        UMETA(Hidden),
	Skin       = 1 << 0,  // 0x01
	Stocking   = 1 << 1,  // 0x02
	Underwear  = 1 << 2,  // 0x04
	Outerwear  = 1 << 3,  // 0x08
	Other      = 1 << 4,  // 0x10 - Unclassified layer
	All        = Skin | Stocking | Underwear | Outerwear | Other UMETA(Hidden)
};
ENUM_CLASS_FLAGS(EFleshRingLayerMask);

/**
 * Normal recomputation method
 * Trade-off between TBN accuracy vs smoothness
 */
UENUM(BlueprintType)
enum class ENormalRecomputeMethod : uint8
{
	/**
	 * Geometric Normal (Face Normal average)
	 * - Computes normals from actual deformed geometry
	 * - TBN matches surface exactly -> accurate Normal Map transformation
	 */
	Geometric	UMETA(DisplayName = "Geometric (TBN Accurate)"),

	/**
	 * Surface Rotation (default)
	 * - Rotates original Smooth Normal by face rotation amount
	 * - Preserves Smooth Normal characteristics, smoother result
	 */
	SurfaceRotation	UMETA(DisplayName = "Surface Rotation")
};

// =====================================
// Struct Definitions
// =====================================

/**
 * Subdivision settings (editor preview + runtime)
 * Used by UFleshRingAsset, can be grouped via IPropertyTypeCustomization
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FSubdivisionSettings
{
	GENERATED_BODY()

	// ===== Common Settings =====

	/**
	 * Enable Subdivision
	 * - ON: Subdivide mesh to improve deformation quality
	 * - OFF: Use original mesh
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings")
	bool bEnableSubdivision = true;

	/** Minimum edge length in cm (subdivision stops below this) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "0.1", DisplayName = "Min Edge Length"))
	float MinEdgeLength = 1.0f;

	// ===== Editor Preview Settings =====

	/** Subdivision level for editor preview */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "1", ClampMax = "4"))
	int32 PreviewSubdivisionLevel = 2;

	/**
	 * Neighbor bone search depth (0 = target bone only, 1 = parent+child, 2 = grandparent+grandchild included)
	 * - If 0, only BoneWeightThreshold determines the region
	 * - Higher values expand subdivision region but increase performance cost
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "0", ClampMax = "3", DisplayName = "Bone Search Depth"))
	int32 PreviewBoneHopCount = 0;

	/**
	 * Bone weight threshold (0.0-1.0)
	 * Only vertices with influence >= this value are subdivision candidates
	 * Higher values narrow subdivision region, improving performance
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "0.01", ClampMax = "0.7", DisplayName = "Min Bone Influence"))
	float PreviewBoneWeightThreshold = 0.1f;

	// ===== Runtime Settings =====

	/** Maximum Subdivision level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "1", ClampMax = "6", EditCondition = "bEnableSubdivision"))
	int32 MaxSubdivisionLevel = 2;

	// ===== Generated Mesh (Runtime) =====

	/**
	 * Subdivided mesh (for runtime)
	 * - Only Ring region subdivided (narrower than preview)
	 * - Generated via Generate Subdivided Mesh button
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skeletal Mesh Detail Settings", meta = (EditCondition = "bEnableSubdivision"), NonTransactional)
	TObjectPtr<USkeletalMesh> SubdividedMesh;

	/** Parameter hash at subdivision generation time (for determining if regeneration needed) */
	UPROPERTY()
	uint32 SubdivisionParamsHash = 0;

	// ===== Baked Mesh (Runtime, deformation applied) =====

	/**
	 * Baked mesh (for runtime)
	 * - Final state with Tightness + Bulge + Smoothing applied
	 * - Unlike preview, only Ring region has deformation/smoothing applied
	 * - Generated via Generate Baked Mesh button
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Baked Mesh", NonTransactional)
	TObjectPtr<USkeletalMesh> BakedMesh;

	/**
	 * Baked Ring transform array (for ring mesh placement)
	 * Ring meshes are placed at these positions at runtime
	 */
	UPROPERTY()
	TArray<FTransform> BakedRingTransforms;

	/**
	 * Parameter hash at bake time
	 * Includes all parameters: Ring settings, Tightness, Bulge, etc.
	 * For determining if regeneration needed
	 */
	UPROPERTY()
	uint32 BakeParamsHash = 0;

	FSubdivisionSettings()
		: bEnableSubdivision(true)
		, MinEdgeLength(1.0f)
		, PreviewSubdivisionLevel(2)
		, PreviewBoneHopCount(0)
		, PreviewBoneWeightThreshold(0.1f)
		, MaxSubdivisionLevel(2)
		, SubdividedMesh(nullptr)
		, SubdivisionParamsHash(0)
		, BakedMesh(nullptr)
		, BakeParamsHash(0)
	{
	}
};

/**
 * Material-Layer mapping
 * - Defines which layer each material belongs to
 * - Resolves penetration so stockings always render above skin
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FMaterialLayerMapping
{
	GENERATED_BODY()

	/**
	 * Material slot index
	 * - Auto-set, not editable
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Layer")
	int32 MaterialSlotIndex = 0;

	/**
	 * Material slot name
	 * - For display, auto-set
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Layer")
	FName MaterialSlotName;

	/**
	 * Layer type
	 * - Skin: Skin/flesh (innermost)
	 * - Stocking: Stockings/tights
	 * - Underwear/Outerwear: Underwear/outerwear
	 * - Other: Unclassified
	 * - Exclude: Excluded from tightness
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	EFleshRingLayerType LayerType = EFleshRingLayerType::Other;

	FMaterialLayerMapping()
		: MaterialSlotIndex(0)
		, MaterialSlotName(NAME_None)
		, LayerType(EFleshRingLayerType::Other)
	{
	}

	FMaterialLayerMapping(int32 InSlotIndex, FName InSlotName, EFleshRingLayerType InLayerType)
		: MaterialSlotIndex(InSlotIndex)
		, MaterialSlotName(InSlotName)
		, LayerType(InLayerType)
	{
	}
};

// =====================================
// Virtual Band Settings (for stockings/tights)
// =====================================

/** Upper/lower section settings for virtual band */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FVirtualBandSection
{
	GENERATED_BODY()

	/**
	 * Section end radius (cm)
	 * - If larger than MidRadius, it flares outward (Bulge region)
	 * - If equal to MidRadius, it forms a straight line (maintains tightness)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (ClampMin = "0.1"))
	float Radius = 10.0f;

	/**
	 * Section height (cm)
	 * - 0: No section (ends directly at band boundary)
	 * - Higher values create a gentler slope
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (ClampMin = "0.0"))
	float Height = 2.0f;

	FVirtualBandSection()
		: Radius(10.0f)
		, Height(2.0f)
	{
	}

	FVirtualBandSection(float InRadius, float InHeight)
		: Radius(InRadius)
		, Height(InHeight)
	{
	}
};

/**
 * Virtual band full settings (asymmetric cylinder for stockings/tights)
 *
 * Cross-section diagram (shape determined by 4 radii):
 *
 *       ══════════════      <- Upper.Radius (upper end, flesh bulge)
 *        ╲          ╱       <- Upper Section (slope)
 *         ╔══════╗          <- MidUpperRadius (band top)
 *         ╚══════╝          <- MidLowerRadius (band bottom)
 *        ╱          ╲       <- Lower Section (slope)
 *       ══════════════      <- Lower.Radius (lower end, stocking region)
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FVirtualBandSettings
{
	GENERATED_BODY()

	// ===== Band Transform =====

	/** Band position offset relative to bone */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector BandOffset = FVector::ZeroVector;

	/** Band rotation (Euler, for UI editing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (DisplayName = "Band Rotation"))
	FRotator BandEulerRotation = FRotator(-90.0f, 0.0f, 0.0f);

	/** Band rotation (Quaternion, for internal calculations) */
	UPROPERTY()
	FQuat BandRotation = FQuat(FRotator(-90.0f, 0.0f, 0.0f));

	// ===== Band Body (Tightening Point) =====

	/**
	 * Band top radius (cm)
	 * - Tightening point at Upper Section boundary
	 * - Must be smaller than Upper.Radius for upward Bulge
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1", DisplayName = "Band Top Radius"))
	float MidUpperRadius = 8.0f;

	/**
	 * Band bottom radius (cm)
	 * - Tightening point at Lower Section boundary
	 * - Must be smaller than Lower.Radius for downward Bulge
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1", DisplayName = "Band Bottom Radius"))
	float MidLowerRadius = 8.0f;

	/**
	 * Band body height (cm)
	 * - Vertical length of tightening region
	 * - Shorter = sharper tightening, Longer = wider tightening
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float BandHeight = 2.0f;

	/**
	 * Band thickness (cm)
	 * - Radial width of influence range
	 * - Thinner = sharper boundary, Thicker = smoother fade
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float BandThickness = 1.0f;

	// ===== Upper Section (Flesh bulge region) =====

	/**
	 * Upper Bulge zone settings
	 * - When Radius > MidUpperRadius, flesh bulges upward
	 * - Height controls slope smoothness
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upper Bulge Zone", meta = (DisplayName = "Upper Bulge Zone"))
	FVirtualBandSection Upper;

	// ===== Lower Section (Stocking coverage region) =====

	/**
	 * Lower Bulge zone settings
	 * - When Radius >= MidLowerRadius, stocking covers downward
	 * - Height controls slope smoothness
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lower Bulge Zone", meta = (DisplayName = "Lower Bulge Zone"))
	FVirtualBandSection Lower;

	FVirtualBandSettings()
		: BandOffset(FVector::ZeroVector)
		, BandEulerRotation(FRotator(-90.0f, 0.0f, 0.0f))
		, BandRotation(FQuat(FRotator(-90.0f, 0.0f, 0.0f)))
		, MidUpperRadius(8.0f)
		, MidLowerRadius(8.0f)
		, BandHeight(2.0f)
		, BandThickness(1.0f)
		, Upper(11.0f, 2.0f)
		, Lower(9.0f, 1.0f)
	{
	}

	/** Calculate total height (lower + band + upper) */
	float GetTotalHeight() const
	{
		return Lower.Height + BandHeight + Upper.Height;
	}

	/** Calculate maximum radius (for bounding) */
	float GetMaxRadius() const
	{
		return FMath::Max(FMath::Max(MidUpperRadius, MidLowerRadius), FMath::Max(Upper.Radius, Lower.Radius));
	}

	/**
	 * Return Z offset relative to Mid Band center
	 * New coordinate system: Z=0 is Mid Band center
	 * Internal coordinate system: Z=0 is Lower bottom
	 * Conversion: InternalZ = LocalZ + GetMidOffset()
	 */
	float GetMidOffset() const
	{
		return Lower.Height + BandHeight * 0.5f;
	}

	/**
	 * Calculate radius at height using Catmull-Rom spline
	 * Connects 4 control points (Lower.Radius -> MidLowerRadius -> MidUpperRadius -> Upper.Radius)
	 * with smooth curve
	 *
	 * Coordinate system: Z=0 is Mid Band center (center of tightening region)
	 *   - Z > 0: Upper direction (Upper section)
	 *   - Z < 0: Lower direction (Lower section)
	 *   - Z = -BandHeight/2: Band lower boundary (MidLowerRadius)
	 *   - Z = +BandHeight/2: Band upper boundary (MidUpperRadius)
	 *
	 * @param LocalZ - Height in band local coordinate system (0 = Mid Band center)
	 * @return Radius at that height
	 */
	float GetRadiusAtHeight(float LocalZ) const
	{
		const float TotalHeight = GetTotalHeight();
		if (TotalHeight <= KINDA_SMALL_NUMBER)
		{
			return MidLowerRadius;
		}

		// New coordinate system -> Internal coordinate system conversion
		// Internal: Z=0 is Lower bottom, Z=TotalHeight is Upper top
		const float MidOffset = GetMidOffset();
		const float InternalZ = LocalZ + MidOffset;

		// 4 control points (internal coordinate system height, radius)
		const float H[4] = { 0.0f, Lower.Height, Lower.Height + BandHeight, TotalHeight };
		const float R[4] = { Lower.Radius, MidLowerRadius, MidUpperRadius, Upper.Radius };

		// Clamp InternalZ (internal coordinate system range)
		const float Z = FMath::Clamp(InternalZ, 0.0f, TotalHeight);

		// Find which segment (0: H0~H1, 1: H1~H2, 2: H2~H3)
		int32 Segment = 0;
		if (Z >= H[2]) Segment = 2;
		else if (Z >= H[1]) Segment = 1;

		// Calculate normalized t within segment
		const float SegmentStart = H[Segment];
		const float SegmentEnd = H[Segment + 1];
		const float SegmentLength = SegmentEnd - SegmentStart;
		const float t = (SegmentLength > KINDA_SMALL_NUMBER) ? (Z - SegmentStart) / SegmentLength : 0.0f;

		// 4 radii needed for Catmull-Rom (P0, P1, P2, P3)
		// Interpolate P1~P2 segment, P0 and P3 are neighbor control points (endpoints duplicated)
		float P0, P1, P2, P3;
		if (Segment == 0)      { P0 = R[0]; P1 = R[0]; P2 = R[1]; P3 = R[2]; }
		else if (Segment == 1) { P0 = R[0]; P1 = R[1]; P2 = R[2]; P3 = R[3]; }
		else                   { P0 = R[1]; P1 = R[2]; P2 = R[3]; P3 = R[3]; }

		// Catmull-Rom spline calculation
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float Result = 0.5f * (
			(2.0f * P1) +
			(-P0 + P2) * t +
			(2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * t2 +
			(-P0 + 3.0f * P1 - 3.0f * P2 + P3) * t3
		);

		// Clamp to prevent overshoot
		const float MinRadius = FMath::Min(FMath::Min(R[0], R[1]), FMath::Min(R[2], R[3]));
		const float MaxRadius = FMath::Max(FMath::Max(R[0], R[1]), FMath::Max(R[2], R[3]));
		return FMath::Clamp(Result, MinRadius, MaxRadius);
	}

};

/** Individual Ring settings */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingSettings
{
	GENERATED_BODY()

	/** Target bone name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	FName BoneName;

	/** Ring custom name (uses "FleshRing_Index" format if empty) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	FName RingName;

	/** Ring mesh (visual representation + SDF source) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	TSoftObjectPtr<UStaticMesh> RingMesh;

	/**
	 * Influence range determination method
	 * - Mesh Based: Auto-calculate via Ring mesh SDF (most accurate)
	 * - Virtual Ring: Manual radius specification (simple cylinder)
	 * - Virtual Band: Variable cylinder for stockings/tights
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (DisplayName = "Effect Range Mode"))
	EFleshRingInfluenceMode InfluenceMode = EFleshRingInfluenceMode::MeshBased;

	/** Ring visibility in editor (includes Mesh, Gizmo, Debug) - controlled via eye icon only */
	UPROPERTY()
	bool bEditorVisible = true;

	/**
	 * Ring radius (cm)
	 * - Inner radius of tightening
	 * - Smaller = tighter squeeze, Larger = looser grip
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", ClampMin = "0.1", ClampMax = "100.0"))
	float RingRadius = 5.0f;

	/**
	 * Ring thickness (cm)
	 * - Radial width of influence range
	 * - Thinner = sharper boundary, Thicker = smoother fade
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", ClampMin = "0.1", ClampMax = "20.0"))
	float RingThickness = 1.0f;

	/**
	 * Ring height (cm)
	 * - Axial influence range
	 * - Affects Height/2 above and below center
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", ClampMin = "0.1", ClampMax = "50.0"))
	float RingHeight = 2.0f;

	/** Ring position offset relative to bone (deformation region) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing"))
	FVector RingOffset = FVector::ZeroVector;

	/** Ring rotation Euler angles (for UI editing, no limits) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", DisplayName = "Ring Rotation"))
	FRotator RingEulerRotation = FRotator(-90.0f, 0.0f, 0.0f);

	/**
	 * Enable Bulge effect
	 * - ON: Surrounding area bulges by volume displaced by Tightness (volume preservation)
	 * - OFF: Pure Tightness only (volume loss)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	bool bEnableBulge = true;

	/**
	 * Bulge direction mode
	 * - Auto: SDF boundary detection (bidirectional for closed meshes)
	 * - Bidirectional: Both up and down (donut-shaped Ring)
	 * - Positive/Negative: One direction only (+Z/-Z)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge"))
	EBulgeDirectionMode BulgeDirection = EBulgeDirectionMode::Auto;

	/**
	 * Bulge falloff curve
	 * - How Bulge influence decreases with distance
	 * - Recommended: WendlandC2 (most natural)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge"))
	EFleshRingFalloffType BulgeFalloff = EFleshRingFalloffType::WendlandC2;

	/**
	 * Bulge intensity multiplier
	 * - 0: No Bulge, 1: Default, 2+: Exaggerated effect
	 * - Recommended: 0.8~1.2
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0"))
	float BulgeIntensity = 1.0f;

	/**
	 * Bulge vertical spread range (multiplier of Ring height)
	 * - 1: Ring height only, 8: Up to 8x Ring height
	 * - Recommended: 3~5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "8.0", DisplayName = "Bulge Vertical Spread"))
	float BulgeAxialRange = 5.0f;

	/**
	 * Bulge horizontal spread range (multiplier of Ring radius)
	 * - 1: Ring radius only, 3: Up to 3x Ring radius
	 * - Recommended: 1~1.5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "3.0", DisplayName = "Bulge Horizontal Spread"))
	float BulgeRadialRange = 1.0f;

	/**
	 * Axial taper coefficient for Bulge collection range
	 * Controls how radial range changes with distance from Ring
	 * - Negative: Contracts (narrows further from ring)
	 * - 0: Cylindrical (constant radius)
	 * - Positive: Expands (widens further from ring, legacy behavior)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge && InfluenceMode != EFleshRingInfluenceMode::VirtualBand", ClampMin = "-1.0", ClampMax = "1.0", DisplayName = "Bulge Horizontal Taper"))
	float BulgeRadialTaper = 0.5f;

	/**
	 * Upper Bulge strength (above Ring)
	 * - 0: Upper Bulge disabled, 1: Default, 2: 2x strength
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "2.0"))
	float UpperBulgeStrength = 1.0f;

	/**
	 * Lower Bulge strength (below Ring)
	 * - 0: Lower Bulge disabled, 1: Default, 2: 2x strength
	 * - Stocking effect: Set lower to 0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "2.0"))
	float LowerBulgeStrength = 1.0f;

	/**
	 * Bulge direction ratio (0 = axial only, 1 = radial only, 0.7 = default)
	 * 0.0: Expand in axial (up/down) direction only
	 * 1.0: Expand in radial (outward) direction only
	 * 0.7: Default - 70% radial, 30% axial
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "Bulge Direction Bias"))
	float BulgeRadialRatio = 0.7f;

	/**
	 * Tightness strength (squeezing)
	 * - 0: No effect, 1: Default, 3: Strong compression
	 * - Recommended: 0.5~1.5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float TightnessStrength = 1.5f;

	/**
	 * Effect bounds X-direction expansion (cm)
	 * - Expands SDF and vertex filtering bounds in X direction
	 * - Use when small Ring needs to cover wider area
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "Effect Bounds Expand X"))
	float SDFBoundsExpandX = 1.0f;

	/**
	 * Effect bounds Y-direction expansion (cm)
	 * - Expands SDF and vertex filtering bounds in Y direction
	 * - Use when small Ring needs to cover wider area
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "Effect Bounds Expand Y"))
	float SDFBoundsExpandY = 1.0f;

	/**
	 * Tightness falloff curve
	 * - Linear: Linear falloff (sharp boundary)
	 * - Quadratic: Quadratic curve (smooth)
	 * - Hermite: S-curve (smoothest, recommended)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (DisplayName = "Tightness Falloff"))
	EFalloffType FalloffType = EFalloffType::Hermite;

	/**
	 * Target layers for Tightness (bitmask)
	 * - Only checked layers are affected by Ring
	 * - Skin: Skin/flesh
	 * - Stocking: Stockings/tights
	 * - Underwear/Outerwear: Underwear/outerwear
	 * - Other: Unclassified materials
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring",
		meta = (DisplayName = "Target Material Layers", Bitmask, BitmaskEnum = "/Script/FleshRingRuntime.EFleshRingLayerMask"))
	int32 AffectedLayerMask = static_cast<int32>(EFleshRingLayerMask::Skin) | static_cast<int32>(EFleshRingLayerMask::Other);

	/** Virtual band settings (used only in VirtualBand mode) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualBand"))
	FVirtualBandSettings VirtualBand;

	/** Ring rotation (actual quaternion applied, used at runtime) */
	UPROPERTY()
	FQuat RingRotation = FRotator(-90.0f, 0.0f, 0.0f).Quaternion();

	/** Mesh position offset relative to bone (visual + SDF) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector MeshOffset = FVector::ZeroVector;

	/** Mesh rotation (actual quaternion applied, used at runtime) */
	UPROPERTY()
	FQuat MeshRotation = FRotator(-90.0f, 0.0f, 0.0f).Quaternion();

	/** Mesh rotation Euler angles (for UI editing, no limits) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (DisplayName = "Mesh Rotation"))
	FRotator MeshEulerRotation = FRotator(-90.0f, 0.0f, 0.0f);

	/** Mesh scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (ClampMin = "0.01"))
	FVector MeshScale = FVector::OneVector;

	// ===== Post Process (Overall Post-processing Control) =====

	/**
	 * Enable post-processing
	 * - ON: Apply Smoothing, Edge Length Preservation, etc.
	 * - OFF: Disable all post-processing
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process")
	bool bEnablePostProcess = true;

	// ===== Smoothing Volume (Post-processing Region) =====

	/**
	 * Smoothing region selection mode
	 * - Bounds Expand: Z-axis bounds expansion (simple)
	 * - Depth-Based: Topology-based depth propagation (precise)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess"))
	ESmoothingVolumeMode SmoothingVolumeMode = ESmoothingVolumeMode::HopBased;

	/**
	 * Maximum smoothing depth
	 * - How many hops from Seed (deformed vertices) to apply smoothing
	 * - 1: Minimum, 100: Maximum
	 * - Recommended: Low-res 5~10, High-res 3~5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (DisplayName = "Max Smoothing Depth", EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides, ClampMin = "1", ClampMax = "20"))
	int32 MaxSmoothingHops = 10;

	/**
	 * Smoothing intensity falloff curve by depth
	 * - Linear: Linear falloff (sharp boundary)
	 * - Quadratic: Quadratic curve (smooth)
	 * - Hermite: S-curve (smoothest, recommended)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (DisplayName = "Depth Falloff", EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides))
	EFalloffType HopFalloffType = EFalloffType::Hermite;

	/**
	 * Smoothing region top expansion distance (cm)
	 * - Additional smoothing range above Ring bounds
	 * - 0: No expansion, 50: Maximum expansion
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand", EditConditionHides, ClampMin = "0.0", ClampMax = "50.0", DisplayName = "Bounds Expand Top (cm)"))
	float SmoothingBoundsZTop = 5.0f;

	/**
	 * Smoothing region bottom expansion distance (cm)
	 * - Additional smoothing range below Ring bounds
	 * - 0: No expansion, 50: Maximum expansion
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand", EditConditionHides, ClampMin = "0.0", ClampMax = "50.0", DisplayName = "Bounds Expand Bottom (cm)"))
	float SmoothingBoundsZBottom = 5.0f;

	// ===== Deformation Spread =====

	/**
	 * Enable deformation spread
	 * - ON: Gradually spread Seed's deformation to surrounding area
	 * - OFF: Only Seed deforms, surroundings stay original
	 * - Execution order: After Radial Smoothing, before Surface Smoothing
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deformation Spread", meta = (DisplayName = "Enable Deformation Spread", EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides))
	bool bEnableHeatPropagation = false;

	/**
	 * Deformation spread iterations
	 * - 1: Minimum spread, 50: Maximum spread
	 * - Recommended: 5~20
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deformation Spread", meta = (DisplayName = "Spread Iterations", EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && bEnableHeatPropagation", EditConditionHides, ClampMin = "1", ClampMax = "50"))
	int32 HeatPropagationIterations = 10;

	/**
	 * Deformation spread strength
	 * - 0.1: Slow spread, 0.9: Fast spread
	 * - Recommended: 0.5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deformation Spread", meta = (DisplayName = "Spread Strength", EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && bEnableHeatPropagation", EditConditionHides, ClampMin = "0.1", ClampMax = "0.9"))
	float HeatPropagationLambda = 0.5f;

	/**
	 * Include Bulge vertices as spread seeds
	 * - ON: Spread both Tightness + Bulge deformation
	 * - OFF: Spread Tightness deformation only
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deformation Spread", meta = (DisplayName = "Spread From Bulge", EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && bEnableHeatPropagation", EditConditionHides))
	bool bIncludeBulgeVerticesAsSeeds = true;

	// ===== Smoothing (Overall Smoothing Control) =====

	/**
	 * Enable smoothing
	 * - ON: Apply Radial + Surface smoothing
	 * - OFF: Disable all smoothing
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess"))
	bool bEnableSmoothing = true;

	// ===== Radial Smoothing (Radius Uniformization) =====

	/**
	 * Enable radial uniformization smoothing
	 * - ON: Uniformize vertices at same height to have same radius
	 * - OFF: Keep individual vertex radius
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing", EditConditionHides))
	bool bEnableRadialSmoothing = true;

	/**
	 * Radial uniformization strength
	 * - 0: No effect (keep original), 1: Full uniformization
	 * - Recommended: 0.8~1.0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableRadialSmoothing", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float RadialBlendStrength = 0.8f;

	/**
	 * Radial uniformization slice height (cm)
	 * - Vertices within same slice are treated as same radius
	 * - 0.1: Precise (high-density mesh), 10: Coarse (low-density mesh)
	 * - Recommended: High-density 0.5, Low-density 2.0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableRadialSmoothing", EditConditionHides, ClampMin = "0.1", ClampMax = "10.0"))
	float RadialSliceHeight = 0.5f;

	// ===== Surface Smoothing Settings =====

	/**
	 * Enable Surface Smoothing
	 * - ON: Smooth mesh surface using Laplacian algorithm
	 * - OFF: No surface smoothing
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (DisplayName = "Enable Surface Smoothing", EditCondition = "bEnablePostProcess && bEnableSmoothing", EditConditionHides))
	bool bEnableLaplacianSmoothing = true;

	/**
	 * Surface Smoothing algorithm
	 * - Standard: Normal smoothing (shrinkage with iterations)
	 * - Volume Preserving: Volume-preserving smoothing (prevents shrinkage, recommended)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (DisplayName = "Smoothing Type", EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides))
	ELaplacianSmoothingType LaplacianSmoothingType = ELaplacianSmoothingType::Laplacian;

	/**
	 * Smoothing strength
	 * - 0.1: Weak, 0.8: Strong (unstable above 0.8)
	 * - Recommended: 0.3~0.7
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (DisplayName = "Smoothing Strength", EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides, ClampMin = "0.1", ClampMax = "1.0", UIMin = "0.1", UIMax = "1.0"))
	float SmoothingLambda = 0.8f;

	/**
	 * Volume Preserving mode expansion coefficient (negative value)
	 * - -1.0: Strong expansion, 0: Auto-calculate
	 * - Condition: |mu| > lambda required to prevent shrinkage
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing && LaplacianSmoothingType == ELaplacianSmoothingType::Taubin", EditConditionHides, AdvancedDisplay, ClampMin = "-1.0", ClampMax = "0.0"))
	float TaubinMu = -0.53f;

	/**
	 * Smoothing iterations
	 * - 1: Minimum, 20: Maximum (Volume Preserving: each iteration = 2 passes)
	 * - Recommended: 2~5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (DisplayName = "Smoothing Iterations", EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides, ClampMin = "1", ClampMax = "30"))
	int32 SmoothingIterations = 20;

	/**
	 * Lock deformed vertices
	 * - ON: Tightness region vertices are locked, only extended region smoothed
	 * - OFF: All vertices smoothed freely
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (DisplayName = "Lock Deformed Vertices", EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides))
	bool bAnchorDeformedVertices = false;

	// ===== Edge Length Preservation Settings =====

	/**
	 * Enable edge length preservation
	 * - ON: Restore stretched/compressed edges from deformation to original length
	 * - OFF: No edge length constraints
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Length Preservation", meta = (DisplayName = "Enable Edge Length Preservation", EditCondition = "bEnablePostProcess"))
	bool bEnablePBDEdgeConstraint = true;

	/**
	 * Constraint strength
	 * - 0: Weak, 1: Strong
	 * - Recommended: 0.5~0.9
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Length Preservation", meta = (DisplayName = "Constraint Strength", EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float PBDStiffness = 0.8f;

	/**
	 * Constraint iterations
	 * - 1: Minimum, 100: Maximum
	 * - Recommended: 3~10
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Length Preservation", meta = (DisplayName = "Constraint Iterations", EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides, ClampMin = "1", ClampMax = "100"))
	int32 PBDIterations = 5;

	/**
	 * Tolerance ratio (deadzone)
	 * - Deformations within this range are preserved
	 * - 0: Correct all deformations, 0.5: Allow up to 50% deformation
	 * - Example: 0.2 = 80%~120% range not corrected
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Length Preservation", meta = (DisplayName = "Stretch Tolerance", EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides, ClampMin = "0.0", ClampMax = "0.5"))
	float PBDTolerance = 0.2f;

	/**
	 * Lock deformed vertices
	 * - ON: Tightness region vertices are locked, only extended region length-corrected
	 * - OFF: All vertices move freely
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Length Preservation", meta = (DisplayName = "Lock Deformed Vertices", EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides))
	bool bPBDAnchorAffectedVertices = true;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::MeshBased)
		, RingRadius(5.0f)
		, RingThickness(1.0f)
		, RingHeight(2.0f)
		, bEnableBulge(true)
		, BulgeFalloff(EFleshRingFalloffType::WendlandC2)
		, BulgeIntensity(1.0f)
		, BulgeAxialRange(5.0f)
		, BulgeRadialRange(1.0f)
		, UpperBulgeStrength(1.0f)
		, LowerBulgeStrength(1.0f)
		, TightnessStrength(1.5f)
		, FalloffType(EFalloffType::Hermite)
		, AffectedLayerMask(static_cast<int32>(EFleshRingLayerMask::Skin) | static_cast<int32>(EFleshRingLayerMask::Other))
		, bEnablePostProcess(true)
		, SmoothingVolumeMode(ESmoothingVolumeMode::HopBased)
		, MaxSmoothingHops(10)
		, HopFalloffType(EFalloffType::Hermite)
		, SmoothingBoundsZTop(5.0f)
		, SmoothingBoundsZBottom(5.0f)
		, bEnableHeatPropagation(false)
		, HeatPropagationIterations(10)
		, HeatPropagationLambda(0.5f)
		, bIncludeBulgeVerticesAsSeeds(true)
		, bEnableSmoothing(true)
		, bEnableRadialSmoothing(true)
		, bEnableLaplacianSmoothing(true)
		, LaplacianSmoothingType(ELaplacianSmoothingType::Laplacian)
		, SmoothingLambda(0.8f)
		, TaubinMu(-0.53f)
		, SmoothingIterations(20)
		, bEnablePBDEdgeConstraint(true)
		, PBDStiffness(0.8f)
		, PBDIterations(5)
		, PBDTolerance(0.2f)
		, bPBDAnchorAffectedVertices(true)
	{
	}

	/**
	 * Calculate Ring mesh world transform
	 * @param BoneTransform Bone's component space transform
	 * @return Ring mesh world transform (Location, Rotation, Scale)
	 */
	FTransform CalculateWorldTransform(const FTransform& BoneTransform) const
	{
		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(MeshOffset);
		const FQuat WorldRotation = BoneRotation * MeshRotation;

		return FTransform(WorldRotation, WorldLocation, MeshScale);
	}

	/**
	 * Get display name for Ring
	 * @param Index Array index (fallback when no custom name)
	 * @return Custom name or "FleshRing_Index" format
	 */
	FString GetDisplayName(int32 Index) const
	{
		return RingName.IsNone() ? FString::Printf(TEXT("FleshRing_%d"), Index) : RingName.ToString();
	}

	/**
	 * Check if layer type is included in AffectedLayerMask
	 * @param LayerType Layer type to check
	 * @return true if vertices of this layer are affected by Tightness
	 */
	bool IsLayerAffected(EFleshRingLayerType LayerType) const
	{
		switch (LayerType)
		{
		case EFleshRingLayerType::Skin:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Skin)) != 0;
		case EFleshRingLayerType::Stocking:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Stocking)) != 0;
		case EFleshRingLayerType::Underwear:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Underwear)) != 0;
		case EFleshRingLayerType::Outerwear:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Outerwear)) != 0;
		case EFleshRingLayerType::Other:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Other)) != 0;
		case EFleshRingLayerType::Exclude:
			// Exclude is always excluded regardless of mask
			return false;
		default:
			// NOTE: If reaching here when adding new layer type, add case
			return false;
		}
	}
};

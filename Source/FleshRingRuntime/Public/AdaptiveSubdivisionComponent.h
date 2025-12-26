// AdaptiveSubdivisionComponent.h
// Runtime adaptive mesh subdivision with ring deformation

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ProceduralMeshComponent.h"
#include "HalfEdgeMesh.h"
#include "AdaptiveSubdivisionComponent.generated.h"

// Subdivision method type
UENUM(BlueprintType)
enum class ESubdivisionMethod : uint8
{
	Uniform			UMETA(DisplayName = "Uniform (Loop)"),
	Adaptive		UMETA(DisplayName = "Adaptive (Legacy - has T-junctions)"),
	LEB				UMETA(DisplayName = "LEB (Crack-free adaptive)")
};

// Mesh type for base geometry
UENUM(BlueprintType)
enum class EBaseMeshType : uint8
{
	Plane		UMETA(DisplayName = "Plane"),
	Cube		UMETA(DisplayName = "Cube"),
	Sphere		UMETA(DisplayName = "Sphere (UV)"),
	Cylinder	UMETA(DisplayName = "Cylinder (Limb)")
};

// Ring profile type for deformation shape
UENUM(BlueprintType)
enum class ERingProfileType : uint8
{
	Cylinder	UMETA(DisplayName = "Cylinder (Flat ends)"),
	Torus		UMETA(DisplayName = "Torus (Round profile)"),
	Cone		UMETA(DisplayName = "Cone (Tapered)")
};

// Simple mesh data structure for subdivision
USTRUCT(BlueprintType)
struct FSubdivisionMeshData
{
	GENERATED_BODY()

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> VertexColors;

	// Edge data for subdivision (vertex index pairs)
	TMap<TPair<int32, int32>, int32> EdgeToMidpoint;

	void Clear()
	{
		Vertices.Empty();
		Triangles.Empty();
		Normals.Empty();
		UVs.Empty();
		VertexColors.Empty();
		EdgeToMidpoint.Empty();
	}

	int32 GetTriangleCount() const { return Triangles.Num() / 3; }
};

/**
 * Component that performs adaptive mesh subdivision at runtime.
 *
 * Key Features:
 * - Creates a subdivided mesh from a simple plane or static mesh
 * - Adaptive subdivision based on ring proximity
 * - Ring-based deformation (compression effect)
 * - Real-time updates with ProceduralMeshComponent
 *
 * Usage:
 * 1. Add this component to an Actor
 * 2. Set up ring parameters (position, radius, direction)
 * 3. Call GenerateMesh() or enable auto-update
 * 4. The mesh will adaptively subdivide near the ring and deform
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class FLESHRINGRUNTIME_API UAdaptiveSubdivisionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAdaptiveSubdivisionComponent();

	// ========== Mesh Generation ==========

	// Type of base mesh to generate
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation")
	EBaseMeshType MeshType = EBaseMeshType::Cube;

	// Size of the generated plane (X, Y)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Plane"))
	FVector2D PlaneSize = FVector2D(100.0f, 100.0f);

	// Size of the generated cube
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Cube"))
	float CubeSize = 50.0f;

	// Sphere radius and segments
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Sphere"))
	float SphereRadius = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Sphere", ClampMin = "4", ClampMax = "32"))
	int32 SphereSegments = 8;

	// Cylinder radius
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Cylinder", ClampMin = "1.0"))
	float CylinderRadius = 20.0f;

	// Cylinder height (along Y axis, like a thigh bone)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Cylinder", ClampMin = "1.0"))
	float CylinderHeight = 100.0f;

	// Cylinder radial segments
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Cylinder", ClampMin = "6", ClampMax = "64"))
	int32 CylinderRadialSegments = 16;

	// Cylinder height segments
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Cylinder", ClampMin = "1", ClampMax = "32"))
	int32 CylinderHeightSegments = 8;

	// Include end caps
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (EditCondition = "MeshType == EBaseMeshType::Cylinder"))
	bool bCylinderCaps = true;

	// Initial grid subdivision (before adaptive)
	// Auto-limited based on MaxTriangleCount to prevent freezing
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (ClampMin = "0", ClampMax = "8"))
	int32 InitialSubdivisions = 1;

	// Maximum adaptive subdivision levels
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (ClampMin = "0", ClampMax = "6"))
	int32 MaxAdaptiveLevel = 3;

	// Maximum triangle count limit (prevents freeze from excessive subdivision)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh|Generation", meta = (ClampMin = "1000", ClampMax = "2000000"))
	int32 MaxTriangleCount = 500000;

	// ========== Ring Parameters ==========

	// Ring profile shape
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Profile")
	ERingProfileType RingProfile = ERingProfileType::Torus;

	// Ring center position (local space)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Transform")
	FVector RingCenter = FVector(0, 0, 0);

	// Ring direction (axis of the ring - should align with the "height" axis of the mesh being squeezed)
	// For Cylinder mesh (Y-axis aligned), use (0,1,0). For horizontal plane, use (0,0,1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Transform")
	FVector RingDirection = FVector(0, 1, 0);

	// ========== Ring Size (Profile-dependent) ==========

	// [Torus] Major radius - distance from ring center to tube center
	// For a cylinder with radius R, set MajorRadius slightly larger than R (e.g., R+2 to R+5)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Size", meta = (ClampMin = "0.1", EditCondition = "RingProfile == ERingProfileType::Torus"))
	float TorusMajorRadius = 22.0f;

	// [Torus] Minor radius - tube thickness (cross-section radius)
	// Compression target (InnerEdge) = MajorRadius - MinorRadius
	// For gentle compression, keep MinorRadius small (3-8)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Size", meta = (ClampMin = "0.1", EditCondition = "RingProfile == ERingProfileType::Torus"))
	float TorusMinorRadius = 5.0f;

	// [Cylinder/Cone] Inner radius (where maximum compression occurs)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Size", meta = (ClampMin = "0.1", EditCondition = "RingProfile != ERingProfileType::Torus"))
	float RingInnerRadius = 5.0f;

	// [Cylinder/Cone] Outer radius (influence falloff boundary)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Size", meta = (ClampMin = "0.1", EditCondition = "RingProfile != ERingProfileType::Torus"))
	float RingOuterRadius = 30.0f;

	// [Cylinder/Cone] Thickness along ring axis
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Size", meta = (ClampMin = "0.1", EditCondition = "RingProfile != ERingProfileType::Torus"))
	float RingThickness = 10.0f;

	// [Cone] Taper ratio - top radius multiplier (1.0 = cylinder, 0.5 = half size at top)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Size", meta = (ClampMin = "0.1", ClampMax = "2.0", EditCondition = "RingProfile == ERingProfileType::Cone"))
	float ConeTaperRatio = 0.5f;

	// ========== Deformation ==========

	// Deformation strength (0 = no deform, 1 = full compression)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Deformation", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float DeformStrength = 1.0f;

	// Falloff distance beyond ring surface
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Deformation", meta = (ClampMin = "0.1"))
	float DeformFalloff = 20.0f;

	// Enable Laplacian smoothing after deformation
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Deformation")
	bool bEnableSmoothing = true;

	// Smoothing strength (0 = none, 1 = full average)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Deformation", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableSmoothing"))
	float SmoothingStrength = 0.5f;

	// Number of smoothing iterations (more = smoother but slower)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring|Deformation", meta = (ClampMin = "1", ClampMax = "10", EditCondition = "bEnableSmoothing"))
	int32 SmoothingIterations = 2;

	// ========== Adaptive Settings ==========

	// Subdivision method to use
	// - Uniform: Subdivides entire mesh uniformly (no adaptive)
	// - Adaptive: Legacy method with T-junction artifacts
	// - LEB: Longest Edge Bisection - crack-free adaptive subdivision (RECOMMENDED)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adaptive")
	ESubdivisionMethod SubdivisionMethod = ESubdivisionMethod::LEB;

	// Distance threshold for triggering subdivision (Adaptive/LEB)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adaptive")
	float SubdivisionTriggerDistance = 50.0f;

	// [Legacy] Enable adaptive subdivision (if false, uniform subdivision)
	// WARNING: Adaptive subdivision can cause T-junction artifacts (gaps between triangles)
	// Use LEB method instead for crack-free results
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adaptive", meta = (EditCondition = "SubdivisionMethod == ESubdivisionMethod::Adaptive"))
	bool bEnableAdaptive = false;

	// ========== LEB Settings ==========

	// Maximum subdivision depth for LEB (higher = more detail, slower)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adaptive|LEB", meta = (ClampMin = "1", ClampMax = "8", EditCondition = "SubdivisionMethod == ESubdivisionMethod::LEB"))
	int32 LEBMaxLevel = 4;

	// Minimum edge length before stopping subdivision
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adaptive|LEB", meta = (ClampMin = "0.1", EditCondition = "SubdivisionMethod == ESubdivisionMethod::LEB"))
	float LEBMinEdgeLength = 2.0f;

	// Influence radius multiplier for LEB (relative to torus size)
	// Higher values = subdivide a larger area around the torus
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adaptive|LEB", meta = (ClampMin = "1.0", ClampMax = "3.0", EditCondition = "SubdivisionMethod == ESubdivisionMethod::LEB"))
	float LEBInfluenceMultiplier = 1.5f;

	// ========== Runtime Settings ==========

	// Auto-update mesh every frame (WARNING: can be expensive, disable in editor)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bAutoUpdate = false;

	// Show debug visualization
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bShowDebug = true;

	// Reference to the procedural mesh component (auto-created)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Runtime")
	TObjectPtr<UProceduralMeshComponent> ProceduralMesh;

	// ========== Blueprint Callable Functions ==========

	// Generate/regenerate the mesh
	UFUNCTION(BlueprintCallable, Category = "Adaptive Subdivision")
	void GenerateMesh();

	// Update deformation only (no subdivision recalc)
	UFUNCTION(BlueprintCallable, Category = "Adaptive Subdivision")
	void UpdateDeformation();

	// Set ring transform from world location
	UFUNCTION(BlueprintCallable, Category = "Adaptive Subdivision")
	void SetRingFromWorldTransform(FVector WorldCenter, FVector WorldDirection);

	// Get current vertex count
	UFUNCTION(BlueprintPure, Category = "Adaptive Subdivision")
	int32 GetCurrentVertexCount() const { return CurrentMeshData.Vertices.Num(); }

	// Get current triangle count
	UFUNCTION(BlueprintPure, Category = "Adaptive Subdivision")
	int32 GetCurrentTriangleCount() const { return CurrentMeshData.GetTriangleCount(); }

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	// ========== Internal Mesh Operations ==========

	// Create initial mesh based on MeshType
	void CreateBaseMesh(FSubdivisionMeshData& OutMesh);

	// Create initial plane mesh
	void CreateBasePlane(FSubdivisionMeshData& OutMesh);

	// Create initial cube mesh (24 vertices for flat shading)
	void CreateBaseCube(FSubdivisionMeshData& OutMesh);

	// Create initial UV sphere mesh
	void CreateBaseSphere(FSubdivisionMeshData& OutMesh);

	// Create initial cylinder mesh (for limbs like thighs)
	void CreateBaseCylinder(FSubdivisionMeshData& OutMesh);

	// Helper: Add a quad as 2 triangles (CW winding)
	void AddQuad(FSubdivisionMeshData& Mesh, int32 V0, int32 V1, int32 V2, int32 V3);

	// Perform one level of Loop subdivision on the entire mesh
	void LoopSubdivide(FSubdivisionMeshData& InOutMesh);

	// Perform adaptive subdivision based on ring proximity
	void AdaptiveSubdivide(FSubdivisionMeshData& InOutMesh, int32 Level);

	// Check if a triangle needs subdivision based on ring distance
	bool ShouldSubdivideTriangle(const FSubdivisionMeshData& Mesh, int32 TriIndex, int32 CurrentLevel);

	// Apply ring deformation to vertices
	void ApplyRingDeformation(FSubdivisionMeshData& InOutMesh);

	// Apply Laplacian smoothing to bulge boundary (between bulge and unaffected area)
	void ApplyLaplacianSmoothing(FSubdivisionMeshData& InOutMesh, const TSet<int32>& BulgeVertices, const TSet<int32>& CompressionVertices);

	// Recalculate normals
	void RecalculateNormals(FSubdivisionMeshData& InOutMesh);

	// Update the procedural mesh component
	void UpdateProceduralMesh();

	// Get or create edge midpoint vertex
	int32 GetOrCreateEdgeMidpoint(FSubdivisionMeshData& Mesh, int32 V0, int32 V1);

	// Calculate distance from vertex to ring
	float CalculateRingDistance(const FVector& Position) const;

	// Draw debug visualization
	void DrawDebugVisualization();

	// ========== LEB Subdivision ==========

	// Perform LEB subdivision on the mesh
	void PerformLEBSubdivision(FSubdivisionMeshData& InOutMesh);

	// Calculate influence region for LEB based on ring profile
	void GetLEBInfluenceRegion(FVector& OutCenter, float& OutRadius) const;

	// ========== Cached Data ==========

	FSubdivisionMeshData BaseMeshData;     // Original undeformed mesh
	FSubdivisionMeshData CurrentMeshData;  // Current deformed mesh

	// Half-edge mesh for LEB subdivision
	FHalfEdgeMesh HalfEdgeMeshData;

	// Cached LEB result (for when ring doesn't move in local space)
	bool bLEBCached = false;
	FVector CachedRingCenter = FVector::ZeroVector;
	float CachedInfluenceRadius = 0.0f;

	bool bMeshDirty = true;
};

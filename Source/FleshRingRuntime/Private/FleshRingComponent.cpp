// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingMeshExtractor.h"
#include "FleshRingSDF.h"
#include "FleshRingVirtualBandMesh.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingBulgeTypes.h"
#include "FleshRingFalloff.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/VolumeTexture.h"
#include "Animation/Skeleton.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/StaticMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "FleshRingDebugPointComponent.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingComponent, Log, All);


// Helper: Get bone's bind pose transform (in component space)
static FTransform GetBoneBindPoseTransform(USkeletalMeshComponent* SkelMesh, FName BoneName)
{
	if (!SkelMesh || BoneName.IsNone())
	{
		return FTransform::Identity;
	}

	const USkeletalMesh* SkeletalMesh = SkelMesh->GetSkeletalMeshAsset();
	if (!SkeletalMesh)
	{
		return FTransform::Identity;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);

	if (BoneIndex == INDEX_NONE)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("GetBoneBindPoseTransform: Bone '%s' not found"), *BoneName.ToString());
		return FTransform::Identity;
	}

	// Calculate Component Space Transform (including parent chain)
	// RefBonePose is local relative to parent, so accumulate along the chain
	FTransform ComponentSpaceTransform = FTransform::Identity;
	int32 CurrentIndex = BoneIndex;

	while (CurrentIndex != INDEX_NONE)
	{
		const FTransform& LocalTransform = RefSkeleton.GetRefBonePose()[CurrentIndex];
		ComponentSpaceTransform = ComponentSpaceTransform * LocalTransform;
		CurrentIndex = RefSkeleton.GetParentIndex(CurrentIndex);
	}

	return ComponentSpaceTransform;
}

// Helper: Validate skeletal mesh (common utility wrapper)
static bool IsSkeletalMeshSkeletonValid(USkeletalMesh* Mesh)
{
	return FleshRingUtils::IsSkeletalMeshValid(Mesh, /*bLogWarnings=*/ true);
}

bool UFleshRingComponent::HasAnyNonSDFRings() const
{
	if (!FleshRingAsset)
	{
		return false;
	}
	for (const FFleshRingSettings& RingSettings : FleshRingAsset->Rings)
	{
		// VirtualRing or VirtualBand modes work without SDF (distance-based logic)
		if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualRing ||
			RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
		{
			return true;
		}
	}
	return false;
}

UFleshRingComponent::UFleshRingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UFleshRingComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bEnableFleshRing && FleshRingAsset && FleshRingAsset->HasBakedMesh())
	{
		if (!ResolvedTargetMesh.IsValid())
		{
			FindTargetMeshOnly();
		}

		// Explicit merged mesh mode detection (set by RebuildMergedMesh)
		if (bCreatedForMergedMesh)
		{
			// Merged mesh mode: ring visuals only (SetupRingMeshes already done in OnRegister)
			ApplyBakedRingTransforms();
			bUsingBakedMesh = true;
			return;
		}

		// Normal mode: apply baked mesh
		ApplyBakedMesh();
	}
}

void UFleshRingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Ring meshes are cleaned up in OnUnregister()
	CleanupDeformer();

#if WITH_EDITOR
	CleanupDebugResources();
#endif

	Super::EndPlay(EndPlayReason);
}

void UFleshRingComponent::BeginDestroy()
{
	// Ensure Deformer cleanup at GC time
	// Prevents FMeshBatch validity issues during asset transitions
	CleanupDeformer();

	Super::BeginDestroy();
}

void UFleshRingComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// Subscribe to asset change delegate
	BindToAssetDelegate();
#endif

	// Setup Ring meshes in both editor and runtime
	// OnRegister is called when component is registered to world (including editor)
	//
	// * In game world, only find target without changing mesh
	// Calling SetSkeletalMesh() during OnRegister disrupts animation initialization
	// Mesh changes should be handled in BeginPlay if needed
	bool bIsGameWorld = GetWorld() && GetWorld()->IsGameWorld();

	if (bIsGameWorld)
	{
		// Find target mesh only (no mesh change)
		FindTargetMeshOnly();
	}
	else
	{
		// Editor: full processing (apply preview mesh, etc.)
		ResolveTargetMesh();
	}
	SetupRingMeshes();
}

void UFleshRingComponent::OnUnregister()
{
#if WITH_EDITOR
	// Unsubscribe from asset change delegate
	UnbindFromAssetDelegate();
#endif

	CleanupRingMeshes();
	Super::OnUnregister();
}

#if WITH_EDITOR
void UFleshRingComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Reconfigure Ring meshes when FleshRingAsset or related properties change
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, FleshRingAsset) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, bEnableFleshRing))
	{
		// Rebind delegate on asset change
		UnbindFromAssetDelegate();
		BindToAssetDelegate();

		ResolveTargetMesh();
		SetupRingMeshes();
	}

	// Ring mesh visibility change
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, bShowRingMesh))
	{
		UpdateRingMeshVisibility();
	}

	// Invalidate cache when Bulge Heatmap is enabled (for immediate debug point display)
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, bShowBulgeHeatmap))
	{
		if (bShowBulgeHeatmap && InternalDeformer)
		{
			if (UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance())
			{
				DeformerInstance->InvalidateTightnessCache();
			}
		}
	}
}

void UFleshRingComponent::BindToAssetDelegate()
{
	if (FleshRingAsset && !AssetChangedDelegateHandle.IsValid())
	{
		AssetChangedDelegateHandle = FleshRingAsset->OnAssetChanged.AddUObject(
			this, &UFleshRingComponent::OnFleshRingAssetChanged);
	}
}

void UFleshRingComponent::UnbindFromAssetDelegate()
{
	if (FleshRingAsset && AssetChangedDelegateHandle.IsValid())
	{
		FleshRingAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
		AssetChangedDelegateHandle.Reset();
	}
}

void UFleshRingComponent::OnFleshRingAssetChanged(UFleshRingAsset* ChangedAsset)
{
	// Check if it's the same asset
	if (ChangedAsset == FleshRingAsset)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Asset changed, reapplying..."));

		// Full reset (including SubdividedMesh application)
		ApplyAsset();
	}
}
#endif

void UFleshRingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnableFleshRing)
	{
		return;
	}
	
	// NOTE: MarkRenderDynamicDataDirty/MarkRenderTransformDirty is not called in TickComponent
	// Optimus approach: Engine's SendRenderDynamicData_Concurrent() automatically calls deformer's EnqueueWork
	// Only call MarkRenderStateDirty/MarkRenderDynamicDataDirty at initialization time (SetupDeformer)

#if WITH_EDITOR
	// Debug visualization
	DrawDebugVisualization();
#endif
}

void UFleshRingComponent::SetTargetMesh(USkeletalMeshComponent* InTargetMesh)
{
	ManualTargetMesh = InTargetMesh;  // Caching (for restoration after CleanupDeformer)
	ResolvedTargetMesh = InTargetMesh;
	bManualTargetSet = (InTargetMesh != nullptr);
	if (InTargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SetTargetMesh called with '%s'"),
			*InTargetMesh->GetName());
	}
}

void UFleshRingComponent::FindTargetMeshOnly()
{
	// Manual target mode: Restore from value set by SetTargetMesh()
	// Even if ResolvedTargetMesh is reset in CleanupDeformer(), restore from ManualTargetMesh
	if (bManualTargetSet)
	{
		ResolvedTargetMesh = ManualTargetMesh;
		return;
	}

	// Auto-discovery mode: Find SkeletalMeshComponent from Owner
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: No owner actor found"));
		return;
	}

	// Search for SkeletalMeshComponent among all Owner's components
	TArray<USkeletalMeshComponent*> SkeletalMeshComponents;
	Owner->GetComponents<USkeletalMeshComponent>(SkeletalMeshComponents);

	if (SkeletalMeshComponents.Num() == 0)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: No SkeletalMeshComponent found on owner '%s'"),
			*Owner->GetName());
		return;
	}

	// Auto-matching: Find component matching FleshRingAsset->TargetSkeletalMesh
	USkeletalMeshComponent* MatchedComponent = nullptr;
	if (FleshRingAsset && !FleshRingAsset->TargetSkeletalMesh.IsNull())
	{
		USkeletalMesh* TargetMesh = FleshRingAsset->TargetSkeletalMesh.LoadSynchronous();
		UE_LOG(LogFleshRingComponent, Log,
			TEXT("[%s] Auto-matching: Looking for TargetSkeletalMesh '%s' among %d components"),
			*GetName(), TargetMesh ? *TargetMesh->GetName() : TEXT("null"), SkeletalMeshComponents.Num());

		if (TargetMesh)
		{
			for (USkeletalMeshComponent* Comp : SkeletalMeshComponents)
			{
				USkeletalMesh* CompMesh = Comp ? Comp->GetSkeletalMeshAsset() : nullptr;
				UE_LOG(LogFleshRingComponent, Log,
					TEXT("[%s]   Checking '%s' -> Mesh='%s' (Match=%d)"),
					*GetName(), Comp ? *Comp->GetName() : TEXT("null"),
					CompMesh ? *CompMesh->GetName() : TEXT("null"),
					CompMesh == TargetMesh);

				if (Comp && CompMesh == TargetMesh)
				{
					MatchedComponent = Comp;
					UE_LOG(LogFleshRingComponent, Log,
						TEXT("[%s] ★ Auto-matched! Component='%s', TargetMesh='%s'"),
						*GetName(), *Comp->GetName(), *TargetMesh->GetName());
					break;
				}
			}
		}
	}
	else
	{
		UE_LOG(LogFleshRingComponent, Warning,
			TEXT("[%s] Auto-matching skipped: FleshRingAsset=%p, TargetSkeletalMesh.IsNull=%d"),
			*GetName(), FleshRingAsset.Get(),
			FleshRingAsset ? FleshRingAsset->TargetSkeletalMesh.IsNull() : -1);
	}

	if (MatchedComponent)
	{
		ResolvedTargetMesh = MatchedComponent;
	}
	else
	{
		// When matching fails, use first SkeletalMeshComponent (legacy behavior)
		ResolvedTargetMesh = SkeletalMeshComponents[0];
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: No matching mesh found, using first one '%s' on owner '%s'"),
			*SkeletalMeshComponents[0]->GetName(), *Owner->GetName());

		if (SkeletalMeshComponents.Num() > 1)
		{
			UE_LOG(LogFleshRingComponent, Warning,
				TEXT("FleshRingComponent: Found %d SkeletalMeshComponents but none matched TargetSkeletalMesh. Using first one."),
				SkeletalMeshComponents.Num());
		}
	}
}

void UFleshRingComponent::ResolveTargetMesh()
{
	// Find target mesh only - don't change the mesh
	// World components use the mesh already set on their SkeletalMeshComponent
	// At runtime, BakedMesh is applied via ApplyBakedMesh() in BeginPlay
	// SubdividedMesh is only used in editor preview scene during bake process
	FindTargetMeshOnly();

	if (ResolvedTargetMesh.IsValid())
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("ResolveTargetMesh: Found target mesh '%s'"),
			ResolvedTargetMesh->GetSkeletalMeshAsset() ? *ResolvedTargetMesh->GetSkeletalMeshAsset()->GetName() : TEXT("null"));
	}
}

void UFleshRingComponent::SetupDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Cannot setup deformer - no target mesh"));
		return;
	}

	// Create internal Deformer
	InternalDeformer = NewObject<UFleshRingDeformer>(this, TEXT("InternalFleshRingDeformer"));
	if (!InternalDeformer)
	{
		UE_LOG(LogFleshRingComponent, Error, TEXT("FleshRingComponent: Failed to create internal deformer"));
		return;
	}

	// Set Owner FleshRingComponent (supports multi-component environment)
	// Correct FleshRingComponent is passed to DeformerInstance at CreateInstance() time
	InternalDeformer->SetOwnerFleshRingComponent(this);

	// Register Deformer to SkeletalMeshComponent
	TargetMesh->SetMeshDeformer(InternalDeformer);

	// * CL 320 restored: Request render state update at initialization like Optimus
	// - MarkRenderStateDirty: Recreate render state for PassthroughVertexFactory creation
	// - MarkRenderDynamicDataDirty: Request dynamic data update
	// Note: Not called in TickComponent (engine handles automatically)
	TargetMesh->MarkRenderStateDirty();
	TargetMesh->MarkRenderDynamicDataDirty();

	// Extend bounds: Deformer deformation may exceed original bounds, so
	// extend to ensure bounds-based caching systems like VSM (Virtual Shadow Maps) work correctly
	TargetMesh->SetBoundsScale(BoundsScale);

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer registered to target mesh '%s'"),
		*TargetMesh->GetName());
}

void UFleshRingComponent::CleanupDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (TargetMesh && InternalDeformer)
	{
		// 1. First wait for ongoing render operations to complete
		FlushRenderingCommands();

		// 2. Explicitly destroy previous DeformerInstance (prevents memory leak)
		// SetMeshDeformer(nullptr) only releases pointer without destroying Instance
		if (UMeshDeformerInstance* OldInstance = TargetMesh->GetMeshDeformerInstance())
		{
			OldInstance->MarkAsGarbage();
			OldInstance->ConditionalBeginDestroy();
		}

		// 3. Release Deformer
		TargetMesh->SetMeshDeformer(nullptr);

		// 4. Mark Render State dirty to trigger Scene Proxy recreation
		// Ensures VertexFactory is properly reinitialized
		TargetMesh->MarkRenderStateDirty();

		// 5. Wait until new render state is applied
		// Prevents FMeshBatch validity issues
		FlushRenderingCommands();

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer unregistered from target mesh"));
	}

	// Restore original mesh (if SubdividedMesh was applied)
	if (TargetMesh && CachedOriginalMesh.IsValid())
	{
		USkeletalMesh* CurrentMesh = TargetMesh->GetSkeletalMeshAsset();
		USkeletalMesh* OriginalMesh = CachedOriginalMesh.Get();

		// Restore only if current mesh differs from original (SubdividedMesh applied state)
		if (CurrentMesh != OriginalMesh)
		{
			TargetMesh->SetSkeletalMesh(OriginalMesh);
			TargetMesh->MarkRenderStateDirty();
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Restored original mesh '%s' on cleanup"),
				OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
		}
	}
	CachedOriginalMesh.Reset();

	InternalDeformer = nullptr;
	ResolvedTargetMesh.Reset();

	// Release SDF cache (IPooledRenderTarget is not UPROPERTY, requires manual release)
	for (FRingSDFCache& Cache : RingSDFCaches)
	{
		Cache.Reset();
	}
	RingSDFCaches.Empty();

	// Reset bake mode flag
	bUsingBakedMesh = false;
}

#if WITH_EDITOR
void UFleshRingComponent::ReinitializeDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("ReinitializeDeformer: No target mesh"));
		return;
	}

	// 1. Wait for ongoing render operations to complete
	FlushRenderingCommands();

	// 2. Explicitly destroy previous DeformerInstance
	if (InternalDeformer)
	{
		if (UMeshDeformerInstance* OldInstance = TargetMesh->GetMeshDeformerInstance())
		{
			OldInstance->MarkAsGarbage();
			OldInstance->ConditionalBeginDestroy();
		}
		TargetMesh->SetMeshDeformer(nullptr);
	}

	// 3. Trigger Render State recreation
	TargetMesh->MarkRenderStateDirty();
	FlushRenderingCommands();

	// 4. Create new Deformer (create fresh instead of reusing existing InternalDeformer object)
	InternalDeformer = NewObject<UFleshRingDeformer>(this, TEXT("InternalFleshRingDeformer"));
	if (!InternalDeformer)
	{
		UE_LOG(LogFleshRingComponent, Error, TEXT("ReinitializeDeformer: Failed to create new deformer"));
		return;
	}

	// 5. Register new Deformer
	TargetMesh->SetMeshDeformer(InternalDeformer);
	TargetMesh->SetBoundsScale(BoundsScale);
	TargetMesh->MarkRenderStateDirty();
	TargetMesh->MarkRenderDynamicDataDirty();

	UE_LOG(LogFleshRingComponent, Log, TEXT("ReinitializeDeformer: Deformer recreated for mesh '%s' (%d vertices)"),
		*TargetMesh->GetSkeletalMeshAsset()->GetName(),
		TargetMesh->GetSkeletalMeshAsset()->GetResourceForRendering()->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());
}
#endif // WITH_EDITOR

void UFleshRingComponent::GenerateSDF()
{
	// Wait for previous render commands to complete
	FlushRenderingCommands();

	if (!FleshRingAsset)
	{
		return;
	}

	// Initialize existing SDF cache
	for (FRingSDFCache& Cache : RingSDFCaches)
	{
		Cache.Reset();
	}
	RingSDFCaches.Empty();

	// Pre-allocate cache array for number of Rings (accessed by index on render thread)
	RingSDFCaches.SetNum(FleshRingAsset->Rings.Num());

	// Generate SDF from RingMesh or VirtualBand for each Ring
	for (int32 RingIndex = 0; RingIndex < FleshRingAsset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

		// ===== VirtualBand mode: No SDF needed, skip (uses distance-based logic) =====
		// VirtualBand mode uses FVirtualBandVertexSelector/FVirtualBandInfluenceProvider
		// to directly use BandSettings parameters for distance-based Tight/Bulge calculation
		// Operates without SDF texture, so skip generation
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
		{
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] is VirtualBand mode, SDF generation skipped (using distance-based logic)"), RingIndex);
			continue;
		}

		// ===== VirtualRing mode: No SDF needed, skip =====
		// VirtualRing mode only uses Ring parameters (RingOffset/RingRotation/RingRadius, etc.)
		// Should not generate SDF even if Ring Mesh exists (mesh is only for visualization)
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
		{
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] is VirtualRing mode, SDF generation skipped"), RingIndex);
			continue;
		}

		// ===== Auto mode: Generate SDF from StaticMesh =====
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Ring[%d] has no valid RingMesh"), RingIndex);
			continue;
		}

		// 1. Extract vertex/index/normal data from StaticMesh (RingMesh)
		FFleshRingMeshData MeshData;
		if (!UFleshRingMeshExtractor::ExtractMeshData(RingMesh, MeshData))
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Failed to extract mesh data from Ring[%d] mesh '%s'"),
				RingIndex, *RingMesh->GetName());
			continue;
		}

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] extracted %d vertices, %d triangles from '%s'"),
			RingIndex, MeshData.GetVertexCount(), MeshData.GetTriangleCount(), *RingMesh->GetName());

		// 2. OBB approach: Keep local space, store transform separately
		// Ring Mesh Local -> MeshTransform -> BoneTransform -> Component Space
		FTransform LocalToComponentTransform;
		{
			// Mesh Transform (Ring Local -> Bone Local)
			FTransform MeshTransform;
			MeshTransform.SetLocation(Ring.MeshOffset);
			MeshTransform.SetRotation(FQuat(Ring.MeshRotation));
			MeshTransform.SetScale3D(Ring.MeshScale);

			// Bone Transform (Bone Local -> Component Space)
			FTransform BoneTransform = GetBoneBindPoseTransform(ResolvedTargetMesh.Get(), Ring.BoneName);

			// Full Transform: Ring Local -> Component Space (saved for OBB)
			LocalToComponentTransform = MeshTransform * BoneTransform;

			// Don't transform vertices (keep local space)
			// SDF is generated in local space, use inverse transform when sampling

			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] OBB Transform saved. Local Bounds: (%s) to (%s)"),
				RingIndex, *MeshData.Bounds.Min.ToString(), *MeshData.Bounds.Max.ToString());
		}

		// 3. Determine SDF resolution (fixed value 64)
		const int32 Resolution = 64;
		const FIntVector SDFResolution(Resolution, Resolution, Resolution);

		// 4. Calculate bounds (for SDF texture - keep original bounds)
		// NOTE: SDFBoundsExpandX/Y is not applied to SDF texture bounds
		// Reason 1: Regenerating SDF every time Expand value is adjusted in editor -> performance/memory issues
		// Reason 2: Bound expansion reduces SDF resolution density -> ring shape quality degradation
		// Reason 3: Padding causes Flood Fill failure on thin rings (walls become thin and leak)
		// Tangent area issue: Solved with minimum step in shader (FleshRingTightnessCS.usf)
		FVector3f BoundsMin = MeshData.Bounds.Min;
		FVector3f BoundsMax = MeshData.Bounds.Max;

		// 5. GPU SDF generation (executed on render thread)
		// Capture MeshData by value (pass to render thread)
		TArray<FVector3f> CapturedVertices = MoveTemp(MeshData.Vertices);
		TArray<uint32> CapturedIndices = MoveTemp(MeshData.Indices);
		FIntVector CapturedResolution = SDFResolution;
		FVector3f CapturedBoundsMin = BoundsMin;
		FVector3f CapturedBoundsMax = BoundsMax;

		// Capture cache pointer (update directly on render thread)
		// TRefCountPtr is thread-safe so can reference directly
		FRingSDFCache* CachePtr = &RingSDFCaches[RingIndex];

		// Pre-set metadata (on game thread)
		CachePtr->BoundsMin = BoundsMin;
		CachePtr->BoundsMax = BoundsMax;
		CachePtr->Resolution = SDFResolution;
		CachePtr->LocalToComponent = LocalToComponentTransform;

		// Auto-detect Bulge direction based on boundary vertices (CPU)
		// SDF center = (BoundsMin + BoundsMax) / 2
		const FVector3f SDFCenter = (BoundsMin + BoundsMax) * 0.5f;
		CachePtr->DetectedBulgeDirection = FBulgeDirectionDetector::DetectFromBoundaryVertices(
			CapturedVertices,
			CapturedIndices,
			SDFCenter
		);

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] Bulge direction auto-detected: %d (SDFCenter: %s)"),
			RingIndex, CachePtr->DetectedBulgeDirection, *SDFCenter.ToString());

		ENQUEUE_RENDER_COMMAND(GenerateFleshRingSDF)(
			[CapturedVertices = MoveTemp(CapturedVertices),
			 CapturedIndices = MoveTemp(CapturedIndices),
			 CapturedResolution,
			 CapturedBoundsMin,
			 CapturedBoundsMax,
			 RingIndex,
			 CachePtr](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				// Create SDF texture (for intermediate results)
				FRDGTextureDesc SDFTextureDesc = FRDGTextureDesc::Create3D(
					FIntVector(CapturedResolution.X, CapturedResolution.Y, CapturedResolution.Z),
					PF_R32_FLOAT,
					FClearValueBinding::Black,
					TexCreate_ShaderResource | TexCreate_UAV);

				FRDGTextureRef RawSDFTexture = GraphBuilder.CreateTexture(SDFTextureDesc, TEXT("FleshRing_RawSDF"));
				FRDGTextureRef CorrectedSDFTexture = GraphBuilder.CreateTexture(SDFTextureDesc, TEXT("FleshRing_CorrectedSDF"));

				// Generate SDF (Point-to-Triangle distance calculation)
				GenerateMeshSDF(
					GraphBuilder,
					RawSDFTexture,
					CapturedVertices,
					CapturedIndices,
					CapturedBoundsMin,
					CapturedBoundsMax,
					CapturedResolution);

				// Donut hole correction (2D Slice Flood Fill)
				Apply2DSliceFloodFill(
					GraphBuilder,
					RawSDFTexture,
					CorrectedSDFTexture,
					CapturedResolution);

				// Key: Convert RDG texture -> Pooled texture (before Execute!)
				// ConvertToExternalTexture must be called before Execute
				// Texture is preserved after Execute, available for next frame
				CachePtr->PooledTexture = GraphBuilder.ConvertToExternalTexture(CorrectedSDFTexture);
				CachePtr->bCached = true;

				// Execute RDG
				GraphBuilder.Execute();

				UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SDF cached for Ring[%d], Resolution=%d"),
					RingIndex, CapturedResolution.X);
			});
	}

	// Wait until SDF generation render commands complete
	// This ensures SDFCache->IsValid() is true after GenerateSDF() returns
	// (Resolves issue where SDF is not yet available in first frame after mode switch during async generation)
	FlushRenderingCommands();

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: GenerateSDF completed for %d rings"), FleshRingAsset->Rings.Num());
}

void UFleshRingComponent::UpdateSDF()
{
	GenerateSDF();
}

void UFleshRingComponent::InitializeForEditorPreview()
{
	// Skip if disabled
	if (!bEnableFleshRing)
	{
		return;
	}

	// Skip if already initialized
	if (bEditorPreviewInitialized)
	{
		return;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("InitializeForEditorPreview: Starting..."));

	// Resolve target mesh
	ResolveTargetMesh();

	if (!ResolvedTargetMesh.IsValid())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("InitializeForEditorPreview: No target mesh"));
		return;
	}

	// Generate SDF and wait for completion
	GenerateSDF();
	FlushRenderingCommands();

	// Setup Deformer only if valid SDF cache exists or VirtualRing mode Ring exists
	// (Auto mode SDF failures are still skipped individually, VirtualRing mode works without SDF)
	if (!HasAnyValidSDFCaches() && !HasAnyNonSDFRings())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("InitializeForEditorPreview: No valid SDF caches and no VirtualRing mode rings, skipping Deformer setup"));
		bEditorPreviewInitialized = true;
		return;
	}

	// Setup Deformer
	SetupDeformer();

	// Setup Ring meshes (may have already been called in OnRegister)
	if (RingMeshComponents.Num() == 0)
	{
		SetupRingMeshes();
	}

	bEditorPreviewInitialized = true;

	UE_LOG(LogFleshRingComponent, Log, TEXT("InitializeForEditorPreview: Completed"));
}

void UFleshRingComponent::ForceInitializeForEditorPreview()
{
	UE_LOG(LogFleshRingComponent, Log, TEXT("ForceInitializeForEditorPreview: Resetting and reinitializing..."));

	// Reset initialization flag
	bEditorPreviewInitialized = false;

	// Cleanup existing Deformer (prevents vertex count mismatch on mesh change)
	if (InternalDeformer)
	{
		CleanupDeformer();
	}

	// Reinitialize
	InitializeForEditorPreview();
}

void UFleshRingComponent::UpdateRingTransforms(int32 DirtyRingIndex)
{
	if (!FleshRingAsset || !ResolvedTargetMesh.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// Determine Ring range to update
	int32 StartIndex = (DirtyRingIndex != INDEX_NONE) ? DirtyRingIndex : 0;
	int32 EndIndex = (DirtyRingIndex != INDEX_NONE) ? DirtyRingIndex + 1 : FleshRingAsset->Rings.Num();

	// Validate range
	StartIndex = FMath::Clamp(StartIndex, 0, FleshRingAsset->Rings.Num());
	EndIndex = FMath::Clamp(EndIndex, 0, FleshRingAsset->Rings.Num());

	for (int32 RingIndex = StartIndex; RingIndex < EndIndex; ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

		// Calculate Bone Transform
		FTransform BoneTransform = GetBoneBindPoseTransform(SkelMesh, Ring.BoneName);
		FQuat BoneRotation = BoneTransform.GetRotation();

		// Mesh Transform (Ring Local -> Bone Local)
		FTransform MeshTransform;
		MeshTransform.SetLocation(Ring.MeshOffset);
		MeshTransform.SetRotation(FQuat(Ring.MeshRotation));
		MeshTransform.SetScale3D(Ring.MeshScale);

		// Full Transform: Ring Local -> Component Space
		FTransform LocalToComponentTransform = MeshTransform * BoneTransform;

		// 1. Update SDF cache's LocalToComponent
		if (RingSDFCaches.IsValidIndex(RingIndex))
		{
			RingSDFCaches[RingIndex].LocalToComponent = LocalToComponentTransform;

			//// [DEBUG] MeshRotation update confirmation
			//FQuat FinalRot = LocalToComponentTransform.GetRotation();
			//UE_LOG(LogFleshRingComponent, Log, TEXT("[DEBUG] UpdateRingTransforms Ring[%d]: MeshRot=(%f,%f,%f,%f), FinalRot=(%f,%f,%f,%f) [Component=%p]"),
			//	RingIndex,
			//	Ring.MeshRotation.X, Ring.MeshRotation.Y, Ring.MeshRotation.Z, Ring.MeshRotation.W,
			//	FinalRot.X, FinalRot.Y, FinalRot.Z, FinalRot.W,
			//	this);
		}

		// 2. Update Ring mesh component's transform
		if (RingMeshComponents.IsValidIndex(RingIndex) && RingMeshComponents[RingIndex])
		{
			FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(Ring.MeshOffset);
			FQuat WorldRotation = BoneRotation * Ring.MeshRotation;
			RingMeshComponents[RingIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
			RingMeshComponents[RingIndex]->SetWorldScale3D(Ring.MeshScale);
		}
	}

	// 3. Invalidate DeformerInstance's TightenedBindPose cache (trigger recalculation)
	// Pass DirtyRingIndex to reprocess only that Ring
	if (USkeletalMeshComponent* SkelMeshComp = ResolvedTargetMesh.Get())
	{
		if (UMeshDeformerInstance* DeformerInstance = SkelMeshComp->GetMeshDeformerInstance())
		{
			if (UFleshRingDeformerInstance* FleshRingInstance = Cast<UFleshRingDeformerInstance>(DeformerInstance))
			{
				FleshRingInstance->InvalidateTightnessCache(DirtyRingIndex);
			}
		}

		// 4. Notify render system of dynamic data change (reflect real-time deformation)
		SkelMeshComp->MarkRenderDynamicDataDirty();
	}

#if WITH_EDITORONLY_DATA
	// 5. Invalidate debug visualization cache (recalculate AffectedVertices when Ring moves)
	// Pass DirtyRingIndex to invalidate only that Ring
	InvalidateDebugCaches(DirtyRingIndex);
#endif
}

void UFleshRingComponent::RefreshRingMeshes()
{
#if WITH_EDITOR
	// Cleanup debug resources (SDF slice actors, etc.) when Ring is deleted
	CleanupDebugResources();
#endif
	CleanupRingMeshes();
	SetupRingMeshes();
}

bool UFleshRingComponent::RefreshWithDeformerReuse()
{
	// Check if Deformer can be reused
	if (!InternalDeformer || !ResolvedTargetMesh.IsValid() || !bEnableFleshRing)
	{
		return false;
	}

	// * Check if Deformer is actually set on SkeletalMeshComponent
	// This check is needed because PreviewScene releases Deformer first when mesh changes
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (TargetMesh && !TargetMesh->GetMeshDeformerInstance())
	{
		// * Cleanup SDF cache first (won't be cleaned in CleanupDeformer if InternalDeformer is set to null)
		FlushRenderingCommands();
		for (FRingSDFCache& Cache : RingSDFCaches)
		{
			Cache.Reset();
		}
		RingSDFCaches.Empty();

		// Consider Deformer released if DeformerInstance doesn't exist
		InternalDeformer = nullptr;
		return false;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: RefreshWithDeformerReuse - Reusing existing Deformer (avoiding GPU resource leak)"));

	// Wait for render commands to complete (SDF generation commands must complete before cache can be released)
	FlushRenderingCommands();

	// Cleanup only SDF cache (keep Deformer)
	for (FRingSDFCache& Cache : RingSDFCaches)
	{
		Cache.Reset();
	}
	RingSDFCaches.Empty();

	// Regenerate SDF
	GenerateSDF();

	// Refresh Ring meshes
	CleanupRingMeshes();
	SetupRingMeshes();

	// Invalidate DeformerInstance's Tightness cache (reflect Ring changes)
	if (USkeletalMeshComponent* SkelMeshComp = ResolvedTargetMesh.Get())
	{
		if (UFleshRingDeformerInstance* DeformerInstance = Cast<UFleshRingDeformerInstance>(SkelMeshComp->GetMeshDeformerInstance()))
		{
			DeformerInstance->InvalidateTightnessCache();
		}
	}

#if WITH_EDITORONLY_DATA
	// Invalidate debug cache (AffectedVertices recalculation needed when Thickness etc. changes)
	// Crash occurs from buffer size mismatch if GetDebugPointCount() returns old value
	bDebugAffectedVerticesCached = false;
	bDebugBulgeVerticesCached = false;

	// Resize debug arrays (array size change needed when Ring is added/removed)
	DebugAffectedData.Reset();
	DebugBulgeData.Reset();
#endif

	return true;
}

void UFleshRingComponent::ApplyAsset()
{
	if (!FleshRingAsset)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyAsset called but FleshRingAsset is null"));
		return;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applying asset '%s'"), *FleshRingAsset->GetName());

	// Reuse existing Deformer if available (prevents GPU memory leak)
	if (RefreshWithDeformerReuse())
	{
		return;
	}

	// Cleanup existing settings and reconfigure (for initial setup or when Deformer doesn't exist)
	CleanupRingMeshes();
	CleanupDeformer();
#if WITH_EDITOR
	CleanupDebugResources();
#endif

	// Reset editor preview state
	bEditorPreviewInitialized = false;

	if (bEnableFleshRing)
	{
		ResolveTargetMesh();

		// SkeletalMesh matching verification (ensures editor preview = game result)
		USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
		if (TargetMesh && !FleshRingAsset->TargetSkeletalMesh.IsNull())
		{
			USkeletalMesh* ExpectedMesh = FleshRingAsset->TargetSkeletalMesh.LoadSynchronous();
			USkeletalMesh* ActualMesh = TargetMesh->GetSkeletalMeshAsset();

			// Pass verification if SubdividedMesh is applied (this is normal)
			bool bIsSubdividedMesh = FleshRingAsset->HasSubdividedMesh() && ActualMesh == FleshRingAsset->SubdivisionSettings.SubdividedMesh;

			if (ExpectedMesh && ActualMesh && ExpectedMesh != ActualMesh && !bIsSubdividedMesh)
			{
				UE_LOG(LogFleshRingComponent, Warning,
					TEXT("FleshRingComponent: SkeletalMesh mismatch! Asset expects '%s' but target has '%s'. Effect may differ from editor preview."),
					*ExpectedMesh->GetName(), *ActualMesh->GetName());
			}
		}

		// Generate SDF (Deformer is setup in BeginPlay() or InitializeForEditorPreview())
		// In editor preview, Deformer is initialized via timer after SkeletalMesh render state is ready
		GenerateSDF();

		SetupRingMeshes();
	}
}

void UFleshRingComponent::SwapFleshRingAsset(UFleshRingAsset* NewAsset)
{
	// Restore original mesh + release asset when nullptr is passed
	if (!NewAsset)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SwapFleshRingAsset(nullptr) - restoring original mesh"));

		// Cleanup existing asset
		CleanupRingMeshes();

		// Restore original mesh
		// SetSkeletalMeshAsset automatically preserves animation state
		USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
		if (TargetMesh && CachedOriginalMesh.IsValid())
		{
			TargetMesh->SetSkeletalMeshAsset(CachedOriginalMesh.Get());
		}

		FleshRingAsset = nullptr;
		bUsingBakedMesh = false;

		return;
	}

	// Use regular ApplyAsset if no baked mesh
	if (!NewAsset->HasBakedMesh())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: NewAsset has no baked mesh, using regular ApplyAsset"));
		FleshRingAsset = NewAsset;
		ApplyAsset();
		return;
	}

	// Cleanup existing asset
	CleanupRingMeshes();
	if (InternalDeformer)
	{
		CleanupDeformer();
	}

	// Set new asset
	FleshRingAsset = NewAsset;

	// Apply baked mesh
	// No need to call ResolveTargetMesh if ResolvedTargetMesh is already valid
	// (ResolveTargetMesh tries to apply SubdividedMesh, which resets animation)
	if (!ResolvedTargetMesh.IsValid())
	{
		ResolveTargetMesh();
	}
	ApplyBakedMesh();

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Swapped to baked asset '%s'"), *NewAsset->GetName());
}

bool UFleshRingComponent::Internal_SwapModularRingAsset(UFleshRingAsset* NewAsset, bool bPreserveLeaderPose)
{
	// 1. BakedMesh check (validate before state change)
	if (NewAsset && !NewAsset->HasBakedMesh())
	{
		UE_LOG(LogFleshRingComponent, Warning,
			TEXT("[%s] Internal_SwapModularRingAsset: NewAsset '%s' has no BakedMesh, cannot apply at runtime"),
			*GetName(), *NewAsset->GetName());
		return false;
	}

	// 2. Edge case: Started without FleshRingAsset
	// Need to re-find target based on new asset's TargetSkeletalMesh
	const bool bNeedRetarget = !FleshRingAsset && NewAsset;
	if (bNeedRetarget)
	{
		FleshRingAsset = NewAsset;
		FindTargetMeshOnly();
	}

	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning,
			TEXT("[%s] Internal_SwapModularRingAsset: No target mesh resolved"), *GetName());
		return false;
	}

	// Skeleton compatibility verification (modular system prerequisite)
	if (NewAsset)
	{
		USkeletalMesh* CurrentMesh = TargetMesh->GetSkeletalMeshAsset();
		USkeletalMesh* NewBakedMesh = NewAsset->SubdivisionSettings.BakedMesh.Get();

		if (CurrentMesh && NewBakedMesh)
		{
			USkeleton* CurrentSkeleton = CurrentMesh->GetSkeleton();
			USkeleton* NewSkeleton = NewBakedMesh->GetSkeleton();

			if (CurrentSkeleton != NewSkeleton)
			{
				UE_LOG(LogFleshRingComponent, Warning,
					TEXT("[%s] Internal_SwapModularRingAsset: Skeleton mismatch - Current: '%s', NewAsset BakedMesh: '%s'"),
					*GetName(),
					CurrentSkeleton ? *CurrentSkeleton->GetName() : TEXT("null"),
					NewSkeleton ? *NewSkeleton->GetName() : TEXT("null"));
				return false;
			}
		}
	}

	// 3. Backup Leader Pose (if needed)
	TWeakObjectPtr<USkinnedMeshComponent> CachedLeaderPose;
	if (bPreserveLeaderPose)
	{
		CachedLeaderPose = TargetMesh->LeaderPoseComponent;
	}

	// 4. Cleanup existing Ring meshes and Deformer
	CleanupRingMeshes();
	if (InternalDeformer)
	{
		CleanupDeformer();
	}

	// 5. Remove ring effect (when nullptr is passed)
	if (!NewAsset)
	{
		// Restore to current asset's original mesh (keep part swap, only release ring effect)
		// Example: Thigh_A -> Thigh_B_BAKED swap then nullptr -> restore to Thigh_B (original)
		if (FleshRingAsset && FleshRingAsset->TargetSkeletalMesh.IsValid())
		{
			USkeletalMesh* CurrentAssetOriginalMesh = FleshRingAsset->TargetSkeletalMesh.LoadSynchronous();
			if (CurrentAssetOriginalMesh)
			{
				TargetMesh->SetSkeletalMeshAsset(CurrentAssetOriginalMesh);
			}
		}
		else if (CachedOriginalMesh.IsValid())
		{
			// Fallback: Use original if no current asset
			TargetMesh->SetSkeletalMeshAsset(CachedOriginalMesh.Get());
		}
		FleshRingAsset = nullptr;
		bUsingBakedMesh = false;

		// Restore Leader Pose
		if (bPreserveLeaderPose && CachedLeaderPose.IsValid())
		{
			TargetMesh->SetLeaderPoseComponent(CachedLeaderPose.Get());
		}

		return true;
	}

	// 6. Apply new asset (already assigned in bNeedRetarget case)
	if (!bNeedRetarget)
	{
		FleshRingAsset = NewAsset;
	}

	// 7. Cache original mesh (if not cached yet)
	if (!CachedOriginalMesh.IsValid())
	{
		CachedOriginalMesh = TargetMesh->GetSkeletalMeshAsset();
	}

	// 8. Apply BakedMesh
	TargetMesh->SetSkeletalMeshAsset(FleshRingAsset->SubdivisionSettings.BakedMesh.Get());
	bUsingBakedMesh = true;

	// 9. Restore Leader Pose
	if (bPreserveLeaderPose && CachedLeaderPose.IsValid())
	{
		TargetMesh->SetLeaderPoseComponent(CachedLeaderPose.Get());
	}

	// 10. Reconfigure Ring meshes and apply baked transforms
	SetupRingMeshes();
	ApplyBakedRingTransforms();

	return true;
}

void UFleshRingComponent::Internal_DetachModularRingAsset(bool bPreserveLeaderPose)
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		return;
	}

	// Backup Leader Pose
	TWeakObjectPtr<USkinnedMeshComponent> CachedLeaderPose;
	if (bPreserveLeaderPose)
	{
		CachedLeaderPose = TargetMesh->LeaderPoseComponent;
	}

	// Remove ring meshes
	CleanupRingMeshes();

	// Reset state (SkeletalMesh remains unchanged)
	FleshRingAsset = nullptr;
	bUsingBakedMesh = false;

	// Restore Leader Pose
	if (bPreserveLeaderPose && CachedLeaderPose.IsValid())
	{
		TargetMesh->SetLeaderPoseComponent(CachedLeaderPose.Get());
	}

	UE_LOG(LogFleshRingComponent, Log,
		TEXT("[%s] Internal_DetachModularRingAsset: Ring asset detached, SkeletalMesh unchanged"),
		*GetName());
}

void UFleshRingComponent::ApplyBakedMesh()
{
	if (!FleshRingAsset || !FleshRingAsset->HasBakedMesh())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyBakedMesh called but no baked mesh available"));
		return;
	}

	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyBakedMesh - no target mesh"));
		return;
	}

	// Save original mesh (for later restoration)
	if (!CachedOriginalMesh.IsValid())
	{
		CachedOriginalMesh = TargetMesh->GetSkeletalMeshAsset();
	}

	// Apply baked mesh
	// SetSkeletalMeshAsset automatically preserves animation state
	USkeletalMesh* BakedMesh = FleshRingAsset->SubdivisionSettings.BakedMesh.Get();
	TargetMesh->SetSkeletalMeshAsset(BakedMesh);

	// Extend bounds (deformation is already applied but for safety)
	TargetMesh->SetBoundsScale(BoundsScale);

	// Update render state
	TargetMesh->MarkRenderStateDirty();

	// Setup Ring meshes and apply baked transforms
	SetupRingMeshes();
	ApplyBakedRingTransforms();

	// Set bake mode flag
	bUsingBakedMesh = true;

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applied baked mesh '%s'"),
		BakedMesh ? *BakedMesh->GetName() : TEXT("null"));
}

void UFleshRingComponent::ApplyBakedRingTransforms()
{
	if (!FleshRingAsset)
	{
		return;
	}

	const TArray<FTransform>& BakedTransforms = FleshRingAsset->SubdivisionSettings.BakedRingTransforms;

	// Skip if no baked transforms (use default bone position)
	if (BakedTransforms.Num() == 0)
	{
		return;
	}

	// Apply baked transforms to each Ring mesh
	for (int32 RingIndex = 0; RingIndex < RingMeshComponents.Num(); ++RingIndex)
	{
		UFleshRingMeshComponent* MeshComp = RingMeshComponents[RingIndex];
		if (!MeshComp)
		{
			continue;
		}

		if (BakedTransforms.IsValidIndex(RingIndex))
		{
			// Baked transforms are in component space
			// Set as relative transform since attached to bone
			const FTransform& BakedTransform = BakedTransforms[RingIndex];
			MeshComp->SetRelativeTransform(BakedTransform);
		}
	}
}

void UFleshRingComponent::SetupRingMeshes()
{
	// Cleanup existing Ring meshes
	CleanupRingMeshes();

	if (!FleshRingAsset || !ResolvedTargetMesh.IsValid())
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// Create StaticMeshComponent for each Ring
	for (int32 RingIndex = 0; RingIndex < FleshRingAsset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

		// VirtualBand mode: Pick via gizmo (same approach as VirtualRing mode)
		// SDF generation is handled directly in GenerateSDF(), so no mesh component created here
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
		{
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// Skip if no RingMesh
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// BoneName validity check
		if (Ring.BoneName.IsNone())
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Ring[%d] has no BoneName"), RingIndex);
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// Check bone index
		const int32 BoneIndex = SkelMesh->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Ring[%d] bone '%s' not found"),
				RingIndex, *Ring.BoneName.ToString());
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// Create FleshRingMeshComponent (higher picking priority than bones in editor)
		// Prevent name collision in multi-FleshRingComponent environment: include component name
		// RF_Transient: Prevent serialization into Blueprint (recreated dynamically each time)
		// Outer is 'this' (not Owner) to avoid affecting Actor's component structure and prevent Reconstruction
		FName ComponentName = FName(*FString::Printf(TEXT("%s_RingMesh_%d"), *GetName(), RingIndex));
		UFleshRingMeshComponent* MeshComp = NewObject<UFleshRingMeshComponent>(this, ComponentName, RF_Transient);
		if (!MeshComp)
		{
			UE_LOG(LogFleshRingComponent, Error, TEXT("FleshRingComponent: Failed to create FleshRingMeshComponent for Ring[%d]"), RingIndex);
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// Set Ring index (used in HitProxy)
		MeshComp->SetRingIndex(RingIndex);

		// Set StaticMesh
		MeshComp->SetStaticMesh(RingMesh);

		// Treat as created by Construction Script (recreated even if deletion attempted in editor)
		MeshComp->CreationMethod = EComponentCreationMethod::Native;
		MeshComp->bIsEditorOnly = false;  // Also visible in game
		MeshComp->SetCastShadow(true);    // Shadow casting

		// Set visibility (must set before RegisterComponent to reflect in SceneProxy creation)
		MeshComp->SetVisibility(bShowRingMesh);

		// Register component
		MeshComp->RegisterComponent();

		// Attach to bone first (snap to bone position)
		MeshComp->AttachToComponent(SkelMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Ring.BoneName);

		// Set relative transform (in bone local space)
		// MeshRotation default FRotator(-90, 0, 0) aligns mesh Z-axis with bone X-axis
		MeshComp->SetRelativeLocation(Ring.MeshOffset);
		MeshComp->SetRelativeRotation(Ring.MeshRotation);
		MeshComp->SetRelativeScale3D(Ring.MeshScale);

		RingMeshComponents.Add(MeshComp);
	}

	// Apply Visibility based on bShowRingMesh state (sync with editor Show Flag)
	UpdateRingMeshVisibility();
}

void UFleshRingComponent::CleanupRingMeshes()
{
	if (RingMeshComponents.Num() > 0)
	{
		// Wait for render thread to finish using component resources
		FlushRenderingCommands();

		for (UStaticMeshComponent* MeshComp : RingMeshComponents)
		{
			if (MeshComp)
			{
				MeshComp->DestroyComponent();
			}
		}
		RingMeshComponents.Empty();
	}
}

void UFleshRingComponent::UpdateRingMeshVisibility()
{
	for (int32 i = 0; i < RingMeshComponents.Num(); ++i)
	{
		UStaticMeshComponent* MeshComp = RingMeshComponents[i];
		if (MeshComp)
		{
			bool bShouldShow = bShowRingMesh;

#if WITH_EDITOR
			// Check per-Ring visibility in editor
			if (FleshRingAsset && FleshRingAsset->Rings.IsValidIndex(i))
			{
				bShouldShow &= FleshRingAsset->Rings[i].bEditorVisible;
			}
#endif

			MeshComp->SetVisibility(bShouldShow);
		}
	}
}

// =====================================
// Debug Drawing (Editor only)
// =====================================

void UFleshRingComponent::SetDebugSlicePlanesVisible(bool bVisible)
{
#if WITH_EDITORONLY_DATA
	for (AActor* PlaneActor : DebugSlicePlaneActors)
	{
		if (PlaneActor)
		{
			// Use SetIsTemporarilyHiddenInEditor in editor (SetActorHiddenInGame doesn't work in editor)
			PlaneActor->SetIsTemporarilyHiddenInEditor(!bVisible);
		}
	}
#endif
}

#if WITH_EDITOR

void UFleshRingComponent::DrawDebugVisualization()
{
	// Skip debug visualization if TargetMesh is missing
	if (!ResolvedTargetMesh.IsValid() || !ResolvedTargetMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	// Hide slice planes if master switch is off
	if (!bShowDebugVisualization || !bShowSDFSlice)
	{
		for (AActor* PlaneActor : DebugSlicePlaneActors)
		{
			if (PlaneActor)
			{
				PlaneActor->SetActorHiddenInGame(true);
			}
		}
	}

	if (!bShowDebugVisualization)
	{
		// Clear Scene Proxy buffers when debug visualization is disabled
		if (DebugPointComponent)
		{
			DebugPointComponent->ClearTightnessBuffer();
			DebugPointComponent->ClearBulgeBuffer();
		}
		return;
	}

	// Ring count is based on Asset
	const int32 NumRings = FleshRingAsset ? FleshRingAsset->Rings.Num() : 0;

	// Calculate valid SDF Ring count (DebugSlicePlaneActors are created only for Rings with SDF)
	int32 NumValidSDFRings = 0;
	for (const FRingSDFCache& Cache : RingSDFCaches)
	{
		if (Cache.IsValid())
		{
			NumValidSDFRings++;
		}
	}

	// Clean up and recreate debug resources when Ring count changes
	// (Prevents index mismatch when Ring is deleted from middle)
	// NOTE: Compare with NumRings since DebugSlicePlaneActors is a Ring index-based array
	if (DebugSlicePlaneActors.Num() != NumRings)
	{
		// [DEBUG] SlicePlane recreation log (uncomment if needed)
		// UE_LOG(LogFleshRingComponent, Warning, TEXT("[DEBUG] SlicePlane RECREATE: DebugSlicePlaneActors=%d, NumRings=%d"),
		// 	DebugSlicePlaneActors.Num(), NumRings);

		for (AActor* PlaneActor : DebugSlicePlaneActors)
		{
			if (PlaneActor)
			{
				PlaneActor->Destroy();
			}
		}
		DebugSlicePlaneActors.Empty();
		DebugSliceRenderTargets.Empty();
	}

	// Pre-allocate array size to NumRings (VirtualRing mode Ring slots are also kept as nullptr)
	if (DebugSlicePlaneActors.Num() < NumRings)
	{
		DebugSlicePlaneActors.SetNum(NumRings);
	}
	if (DebugSliceRenderTargets.Num() < NumRings)
	{
		DebugSliceRenderTargets.SetNum(NumRings);
	}

	if (DebugAffectedData.Num() != NumRings)
	{
		bDebugAffectedVerticesCached = false;
	}
	if (DebugBulgeData.Num() != NumRings)
	{
		bDebugBulgeVerticesCached = false;
	}

	// GPU debug rendering mode: render circular points via shader
	// Scene Proxy approach: renders below editor gizmos
	// PointCount is read directly from buffer's NumElements on render thread
	if (bUseGPUDebugRendering)
	{
		UpdateTightnessDebugPointComponent();
		UpdateBulgeDebugPointComponent();
	}

	for (int32 RingIndex = 0; RingIndex < NumRings; ++RingIndex)
	{
		// Skip hidden Ring (debug visualization)
		if (FleshRingAsset && FleshRingAsset->Rings.IsValidIndex(RingIndex) &&
			!FleshRingAsset->Rings[RingIndex].bEditorVisible)
		{
			continue;
		}

		if (bShowSdfVolume)
		{
			DrawSdfVolume(RingIndex);
		}

		// Use CPU DrawDebugPoint only when not in GPU rendering mode
		if (bShowAffectedVertices && !bUseGPUDebugRendering)
		{
			DrawAffectedVertices(RingIndex);
		}

		if (bShowSDFSlice)
		{
			DrawSDFSlice(RingIndex);
		}

		if (bShowBulgeHeatmap)
		{
			// Use CPU DrawDebugPoint only when not in GPU rendering mode
			if (!bUseGPUDebugRendering)
			{
				DrawBulgeHeatmap(RingIndex);
			}
			// Always show direction arrow
			DrawBulgeDirectionArrow(RingIndex);
		}

		if (bShowBulgeRange)
		{
			DrawBulgeRange(RingIndex);
		}
	}
}

void UFleshRingComponent::DrawSdfVolume(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get SDF cache
	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		// Display warning on screen if no cache
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Red,
				FString::Printf(TEXT("Ring[%d]: SDF not cached!"), RingIndex));
		}
		return;
	}

	// OBB approach: local bounds + transform
	FVector LocalBoundsMin = FVector(SDFCache->BoundsMin);
	FVector LocalBoundsMax = FVector(SDFCache->BoundsMax);

	// Calculate Center and Extent in local space
	FVector LocalCenter = (LocalBoundsMin + LocalBoundsMax) * 0.5f;
	FVector LocalExtent = (LocalBoundsMax - LocalBoundsMin) * 0.5f;

	// Full transform: Local → Component → World
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	FTransform LocalToWorld = SDFCache->LocalToComponent;
	if (SkelMesh)
	{
		LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
	}

	// Center in world space
	FVector WorldCenter = LocalToWorld.TransformPosition(LocalCenter);

	// OBB rotation
	FQuat WorldRotation = LocalToWorld.GetRotation();

	// Scale-applied Extent
	FVector ScaledExtent = LocalExtent * LocalToWorld.GetScale3D();

	// [Conditional log] Output on first frame only - DrawSdfVolume Debug
	static bool bLoggedOBBDebug = false;
	if (!bLoggedOBBDebug)
	{
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("======== DrawSdfVolume OBB Debug ========"));
		UE_LOG(LogTemp, Log, TEXT("  [Local Space]"));
		UE_LOG(LogTemp, Log, TEXT("    LocalBoundsMin: %s"), *LocalBoundsMin.ToString());
		UE_LOG(LogTemp, Log, TEXT("    LocalBoundsMax: %s"), *LocalBoundsMax.ToString());
		UE_LOG(LogTemp, Log, TEXT("    LocalSize: %s"), *(LocalBoundsMax - LocalBoundsMin).ToString());
		UE_LOG(LogTemp, Log, TEXT("  [LocalToComponent Transform]"));
		UE_LOG(LogTemp, Log, TEXT("    Location: %s"), *SDFCache->LocalToComponent.GetLocation().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Rotation: %s"), *SDFCache->LocalToComponent.GetRotation().Rotator().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Scale: %s"), *SDFCache->LocalToComponent.GetScale3D().ToString());
		// Component Space OBB for comparison with SubdivideRegion
		{
			FVector CompCenter = SDFCache->LocalToComponent.TransformPosition(LocalCenter);
			FQuat CompRotation = SDFCache->LocalToComponent.GetRotation();
			FVector CompAxisX = CompRotation.RotateVector(FVector(1, 0, 0));
			FVector CompAxisY = CompRotation.RotateVector(FVector(0, 1, 0));
			FVector CompAxisZ = CompRotation.RotateVector(FVector(0, 0, 1));
			FVector CompHalfExtents = LocalExtent * SDFCache->LocalToComponent.GetScale3D();
			UE_LOG(LogTemp, Log, TEXT("  [Component Space OBB (compare with SubdivideRegion)]"));
			UE_LOG(LogTemp, Log, TEXT("    Center: %s"), *CompCenter.ToString());
			UE_LOG(LogTemp, Log, TEXT("    HalfExtents: %s"), *CompHalfExtents.ToString());
			UE_LOG(LogTemp, Log, TEXT("    AxisX: %s"), *CompAxisX.ToString());
			UE_LOG(LogTemp, Log, TEXT("    AxisY: %s"), *CompAxisY.ToString());
			UE_LOG(LogTemp, Log, TEXT("    AxisZ: %s"), *CompAxisZ.ToString());
		}
		UE_LOG(LogTemp, Log, TEXT("  [LocalToWorld (includes ComponentToWorld)]"));
		UE_LOG(LogTemp, Log, TEXT("    Location: %s"), *LocalToWorld.GetLocation().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Rotation: %s"), *LocalToWorld.GetRotation().Rotator().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Scale: %s"), *LocalToWorld.GetScale3D().ToString());
		UE_LOG(LogTemp, Log, TEXT("  [Visualization]"));
		UE_LOG(LogTemp, Log, TEXT("    WorldCenter: %s"), *WorldCenter.ToString());
		UE_LOG(LogTemp, Log, TEXT("    ScaledExtent: %s"), *ScaledExtent.ToString());
		UE_LOG(LogTemp, Log, TEXT("    WorldRotation: %s"), *WorldRotation.Rotator().ToString());
		UE_LOG(LogTemp, Log, TEXT("=========================================="));
		UE_LOG(LogTemp, Log, TEXT(""));
		bLoggedOBBDebug = true;
	}

	FColor BracketColor = FColor(130, 200, 255, 160);  // Blue (SDF texture bounds)
	FColor ExpandedBracketColor = FColor(80, 220, 80, 160);  // Green (expanded bounds)
	float LineThickness = 0.20f;
	float BracketRatio = 0.25f;

	// OBB local axis directions (in world space)
	FVector AxisX = WorldRotation.RotateVector(FVector::ForwardVector);  // X axis
	FVector AxisY = WorldRotation.RotateVector(FVector::RightVector);    // Y axis
	FVector AxisZ = WorldRotation.RotateVector(FVector::UpVector);       // Z axis

	// Bracket length per axis
	float BracketLenX = ScaledExtent.X * 2.0f * BracketRatio;
	float BracketLenY = ScaledExtent.Y * 2.0f * BracketRatio;
	float BracketLenZ = ScaledExtent.Z * 2.0f * BracketRatio;

	// Calculate 8 corners and draw brackets (SDF texture bounds - blue)
	// Corner = Center + (±ExtentX * AxisX) + (±ExtentY * AxisY) + (±ExtentZ * AxisZ)
	for (int32 i = 0; i < 8; ++i)
	{
		// Determine corner position via bitmask (0=Min, 1=Max)
		float SignX = (i & 1) ? 1.0f : -1.0f;
		float SignY = (i & 2) ? 1.0f : -1.0f;
		float SignZ = (i & 4) ? 1.0f : -1.0f;

		// Corner position (world space)
		FVector Corner = WorldCenter
			+ AxisX * ScaledExtent.X * SignX
			+ AxisY * ScaledExtent.Y * SignY
			+ AxisZ * ScaledExtent.Z * SignZ;

		// Draw bracket lines along each axis (inward from corner)
		// Draw with SDPG_Foreground to display above heatmap
		// X axis bracket
		FVector EndX = Corner - AxisX * BracketLenX * SignX;
		DrawDebugLine(World, Corner, EndX, BracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

		// Y axis bracket
		FVector EndY = Corner - AxisY * BracketLenY * SignY;
		DrawDebugLine(World, Corner, EndY, BracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

		// Z axis bracket
		FVector EndZ = Corner - AxisZ * BracketLenZ * SignZ;
		DrawDebugLine(World, Corner, EndZ, BracketColor, false, -1.0f, SDPG_Foreground, LineThickness);
	}

	// ===== Draw expanded bounds (green) - SDFBoundsExpandX/Y applied =====
	if (FleshRingAsset && FleshRingAsset->Rings.IsValidIndex(RingIndex))
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];
		const float ExpandX = Ring.SDFBoundsExpandX;
		const float ExpandY = Ring.SDFBoundsExpandY;

		// Only draw when expansion exists
		if (ExpandX > 0.01f || ExpandY > 0.01f)
		{
			// Calculate expanded local bounds
			FVector ExpandedLocalMin = LocalBoundsMin - FVector(ExpandX, ExpandY, 0.0f);
			FVector ExpandedLocalMax = LocalBoundsMax + FVector(ExpandX, ExpandY, 0.0f);

			// Expanded Center and Extent
			FVector ExpandedLocalCenter = (ExpandedLocalMin + ExpandedLocalMax) * 0.5f;
			FVector ExpandedLocalExtent = (ExpandedLocalMax - ExpandedLocalMin) * 0.5f;

			// Transform to world space
			FVector ExpandedWorldCenter = LocalToWorld.TransformPosition(ExpandedLocalCenter);
			FVector ExpandedScaledExtent = ExpandedLocalExtent * LocalToWorld.GetScale3D();

			// Expanded bracket lengths
			float ExpandedBracketLenX = ExpandedScaledExtent.X * 2.0f * BracketRatio;
			float ExpandedBracketLenY = ExpandedScaledExtent.Y * 2.0f * BracketRatio;
			float ExpandedBracketLenZ = ExpandedScaledExtent.Z * 2.0f * BracketRatio;

			// Draw 8 corners of expanded bounds (green)
			for (int32 i = 0; i < 8; ++i)
			{
				float SignX = (i & 1) ? 1.0f : -1.0f;
				float SignY = (i & 2) ? 1.0f : -1.0f;
				float SignZ = (i & 4) ? 1.0f : -1.0f;

				FVector ExpandedCorner = ExpandedWorldCenter
					+ AxisX * ExpandedScaledExtent.X * SignX
					+ AxisY * ExpandedScaledExtent.Y * SignY
					+ AxisZ * ExpandedScaledExtent.Z * SignZ;

				// X axis bracket (green)
				FVector EndX = ExpandedCorner - AxisX * ExpandedBracketLenX * SignX;
				DrawDebugLine(World, ExpandedCorner, EndX, ExpandedBracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

				// Y axis bracket (green)
				FVector EndY = ExpandedCorner - AxisY * ExpandedBracketLenY * SignY;
				DrawDebugLine(World, ExpandedCorner, EndY, ExpandedBracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

				// Z axis bracket (green)
				FVector EndZ = ExpandedCorner - AxisZ * ExpandedBracketLenZ * SignZ;
				DrawDebugLine(World, ExpandedCorner, EndZ, ExpandedBracketColor, false, -1.0f, SDPG_Foreground, LineThickness);
			}
		}
	}
}

void UFleshRingComponent::DrawAffectedVertices(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Cache first if not already cached
	if (!bDebugAffectedVerticesCached)
	{
		CacheAffectedVerticesForDebug();
	}

	// Validate data
	if (!DebugAffectedData.IsValidIndex(RingIndex) ||
		DebugBindPoseVertices.Num() == 0)
	{
		return;
	}

	const FRingAffectedData& RingData = DebugAffectedData[RingIndex];
	if (RingData.Vertices.Num() == 0)
	{
		return;
	}

	// Current skeletal mesh component
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh)
	{
		return;
	}

	// Component → World transform
	FTransform CompTransform = SkelMesh->GetComponentTransform();

	// ===== Get GPU Influence Readback result from DeformerInstance =====
	// NOTE: Currently only single Ring (RingIndex == 0) supported, multi-Ring for future extension
	if (RingIndex == 0 && InternalDeformer)
	{
		UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance();
		if (DeformerInstance)
		{
			if (DeformerInstance->IsDebugInfluenceReadbackComplete(0))
			{
				const TArray<float>* ReadbackResult = DeformerInstance->GetDebugInfluenceReadbackResult(0);
				if (ReadbackResult && ReadbackResult->Num() > 0)
				{
					// Initialize GPU Influence cache array (if needed)
					if (!CachedGPUInfluences.IsValidIndex(RingIndex))
					{
						CachedGPUInfluences.SetNum(RingIndex + 1);
						bGPUInfluenceReady.SetNum(RingIndex + 1);
					}

					// Copy Readback result
					CachedGPUInfluences[RingIndex] = *ReadbackResult;
					bGPUInfluenceReady[RingIndex] = true;

					// Reset Readback completion flag (prepare for next Readback)
					DeformerInstance->ResetDebugInfluenceReadback(0);
				}
			}
			else
			{
				// Invalidate existing cache when Readback incomplete (switch to CPU fallback)
				// Prevents stale data display when cache invalidated during drag
				if (bGPUInfluenceReady.IsValidIndex(RingIndex))
				{
					bGPUInfluenceReady[RingIndex] = false;
				}
			}
		}
	}

	// Check if GPU Influence is available
	bool bUseGPUInfluence = false;
	if (bGPUInfluenceReady.IsValidIndex(RingIndex) && bGPUInfluenceReady[RingIndex] &&
		CachedGPUInfluences.IsValidIndex(RingIndex) && CachedGPUInfluences[RingIndex].Num() > 0)
	{
		bUseGPUInfluence = true;
	}

	// For each affected vertex
	for (int32 i = 0; i < RingData.Vertices.Num(); ++i)
	{
		const FAffectedVertex& AffectedVert = RingData.Vertices[i];
		if (!DebugBindPoseVertices.IsValidIndex(AffectedVert.VertexIndex))
		{
			continue;
		}

		// Bind pose position (component space)
		const FVector3f& BindPosePos = DebugBindPoseVertices[AffectedVert.VertexIndex];

		// Transform to world space (based on bind pose - animation not applied)
		FVector WorldPos = CompTransform.TransformPosition(FVector(BindPosePos));

		// Determine Influence value: GPU value takes priority, use CPU value if unavailable
		float Influence;
		if (bUseGPUInfluence && CachedGPUInfluences[RingIndex].IsValidIndex(i))
		{
			// Use GPU-computed Influence
			Influence = CachedGPUInfluences[RingIndex][i];
		}
		else
		{
			// Use CPU-computed Influence (fallback)
			Influence = AffectedVert.Influence;
		}

		// Color based on Influence (0=blue, 0.5=green, 1=red)
		FColor PointColor;
		if (Influence < 0.5f)
		{
			// Blue → Green
			float T = Influence * 2.0f;
			PointColor = FColor(
				0,
				FMath::RoundToInt(255 * T),
				FMath::RoundToInt(255 * (1.0f - T))
			);
		}
		else
		{
			// Green → Red
			float T = (Influence - 0.5f) * 2.0f;
			PointColor = FColor(
				FMath::RoundToInt(255 * T),
				FMath::RoundToInt(255 * (1.0f - T)),
				0
			);
		}

		// Draw point (size proportional to Influence)
		float PointSize = 2.0f + Influence * 6.0f; // Range 2~8
		DrawDebugPoint(World, WorldPos, PointSize, PointColor, false, -1.0f, SDPG_Foreground);
	}

	// Display info on screen
	if (GEngine)
	{
		FString SourceStr = bUseGPUInfluence ? TEXT("GPU") : TEXT("CPU");
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Green,
			FString::Printf(TEXT("Ring[%d] Affected: %d vertices (Source: %s)"),
				RingIndex, RingData.Vertices.Num(), *SourceStr));
	}
}

void UFleshRingComponent::DrawSDFSlice(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		// Clean up Actor for Ring with invalidated SDF (when Ring Mesh deleted)
		if (DebugSlicePlaneActors.IsValidIndex(RingIndex) && DebugSlicePlaneActors[RingIndex])
		{
			DebugSlicePlaneActors[RingIndex]->Destroy();
			DebugSlicePlaneActors[RingIndex] = nullptr;
		}
		return;
	}

	// Ensure array size
	if (DebugSlicePlaneActors.Num() <= RingIndex)
	{
		DebugSlicePlaneActors.SetNum(RingIndex + 1);
	}
	if (DebugSliceRenderTargets.Num() <= RingIndex)
	{
		DebugSliceRenderTargets.SetNum(RingIndex + 1);
	}

	// Create plane actor if not exists
	if (!DebugSlicePlaneActors[RingIndex])
	{
		DebugSlicePlaneActors[RingIndex] = CreateDebugSlicePlane(RingIndex);
	}

	AActor* PlaneActor = DebugSlicePlaneActors[RingIndex];
	if (!PlaneActor)
	{
		return;
	}

	// Make plane visible
	PlaneActor->SetActorHiddenInGame(false);

	// OBB approach: calculate local bounds
	FVector LocalBoundsMin = FVector(SDFCache->BoundsMin);
	FVector LocalBoundsMax = FVector(SDFCache->BoundsMax);
	FVector LocalBoundsSize = LocalBoundsMax - LocalBoundsMin;

	// Calculate Z slice position (local space)
	float ZRatio = (SDFCache->Resolution.Z > 1)
		? (float)DebugSliceZ / (float)(SDFCache->Resolution.Z - 1)
		: 0.5f;
	ZRatio = FMath::Clamp(ZRatio, 0.0f, 1.0f);

	FVector LocalSliceCenter = LocalBoundsMin + FVector(
		LocalBoundsSize.X * 0.5f,
		LocalBoundsSize.Y * 0.5f,
		LocalBoundsSize.Z * ZRatio
	);

	// OBB transform: Local → Component → World
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	FTransform LocalToWorld = SDFCache->LocalToComponent;
	if (SkelMesh)
	{
		LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
	}

	// Slice position/rotation in world space
	FVector WorldSliceCenter = LocalToWorld.TransformPosition(LocalSliceCenter);
	FQuat WorldRotation = LocalToWorld.GetRotation();

	// Set plane position/rotation
	PlaneActor->SetActorLocation(WorldSliceCenter);
	PlaneActor->SetActorRotation(WorldRotation.Rotator());

	// Plane scale (local bounds size + OBB scale applied, default Plane is 100x100 units)
	FVector OBBScale = LocalToWorld.GetScale3D();
	float ScaleX = (LocalBoundsSize.X * OBBScale.X) / 100.0f;
	float ScaleY = (LocalBoundsSize.Y * OBBScale.Y) / 100.0f;
	PlaneActor->SetActorScale3D(FVector(ScaleX, ScaleY, 1.0f));

	// Update slice texture
	UpdateSliceTexture(RingIndex, DebugSliceZ);

	// Display slice info on screen
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("Ring[%d] Slice Z: %d/%d"),
				RingIndex, DebugSliceZ, SDFCache->Resolution.Z));
	}
}

AActor* UFleshRingComponent::CreateDebugSlicePlane(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// Spawn plane actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* PlaneActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (!PlaneActor)
	{
		return nullptr;
	}

	// Create root component
	USceneComponent* RootComp = NewObject<USceneComponent>(PlaneActor, TEXT("RootComponent"));
	PlaneActor->SetRootComponent(RootComp);
	RootComp->RegisterComponent();

	// Create StaticMeshComponent (using default Plane mesh) - front face
	UStaticMeshComponent* PlaneMeshFront = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("PlaneMeshFront"));

	// Load engine default Plane mesh
	UStaticMesh* DefaultPlane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (DefaultPlane)
	{
		PlaneMeshFront->SetStaticMesh(DefaultPlane);
	}

	// Disable collision
	PlaneMeshFront->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlaneMeshFront->SetCollisionResponseToAllChannels(ECR_Ignore);
	PlaneMeshFront->SetGenerateOverlapEvents(false);

	// Disable shadows
	PlaneMeshFront->SetCastShadow(false);

	// Register and attach component
	PlaneMeshFront->AttachToComponent(RootComp, FAttachmentTransformRules::KeepRelativeTransform);
	PlaneMeshFront->RegisterComponent();

	// Add back face plane (180 degree rotation)
	UStaticMeshComponent* PlaneMeshBack = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("PlaneMeshBack"));
	if (DefaultPlane)
	{
		PlaneMeshBack->SetStaticMesh(DefaultPlane);
	}
	PlaneMeshBack->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlaneMeshBack->SetCollisionResponseToAllChannels(ECR_Ignore);
	PlaneMeshBack->SetGenerateOverlapEvents(false);
	PlaneMeshBack->SetCastShadow(false);
	PlaneMeshBack->AttachToComponent(RootComp, FAttachmentTransformRules::KeepRelativeTransform);
	PlaneMeshBack->SetRelativeRotation(FRotator(180.0f, 0.0f, 0.0f));  // Rotate 180 degrees around X axis
	PlaneMeshBack->RegisterComponent();

	// Create render target
	if (DebugSliceRenderTargets.Num() <= RingIndex)
	{
		DebugSliceRenderTargets.SetNum(RingIndex + 1);
	}

	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	int32 Resolution = SDFCache ? SDFCache->Resolution.X : 64;

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(this);
	RenderTarget->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
	RenderTarget->UpdateResourceImmediate(true);
	DebugSliceRenderTargets[RingIndex] = RenderTarget;

	// Use Widget3DPassThrough material (displays texture as-is)
	UMaterial* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
	if (!BaseMaterial)
	{
		BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}

	UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, PlaneActor);
	if (DynMaterial && RenderTarget)
	{
		DynMaterial->SetTextureParameterValue(TEXT("SlateUI"), RenderTarget);
		PlaneMeshFront->SetMaterial(0, DynMaterial);
		PlaneMeshBack->SetMaterial(0, DynMaterial);  // Same material for back face
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("Created debug slice plane for Ring[%d]"), RingIndex);

	return PlaneActor;
}

void UFleshRingComponent::UpdateSliceTexture(int32 RingIndex, int32 SliceZ)
{
	if (!DebugSliceRenderTargets.IsValidIndex(RingIndex))
	{
		return;
	}

	UTextureRenderTarget2D* RenderTarget = DebugSliceRenderTargets[RingIndex];
	if (!RenderTarget)
	{
		return;
	}

	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		return;
	}

	// GPU work: extract slice from cached SDF
	TRefCountPtr<IPooledRenderTarget> SDFTexture = SDFCache->PooledTexture;
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	FIntVector Resolution = SDFCache->Resolution;
	int32 CapturedSliceZ = FMath::Clamp(SliceZ, 0, Resolution.Z - 1);

	ENQUEUE_RENDER_COMMAND(ExtractSDFSlice)(
		[SDFTexture, RTResource, Resolution, CapturedSliceZ](FRHICommandListImmediate& RHICmdList)
		{
			if (!SDFTexture.IsValid() || !RTResource)
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);

			// Register cached SDF to RDG
			FRDGTextureRef SDFTextureRDG = GraphBuilder.RegisterExternalTexture(SDFTexture);

			// Set up output texture
			FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
				FIntPoint(Resolution.X, Resolution.Y),
				PF_B8G8R8A8,
				FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
			);
			FRDGTextureRef OutputSlice = GraphBuilder.CreateTexture(OutputDesc, TEXT("DebugSDFSlice"));

			// Execute slice visualization shader
			GenerateSDFSlice(
				GraphBuilder,
				SDFTextureRDG,
				OutputSlice,
				Resolution,
				CapturedSliceZ,
				10.0f  // MaxDisplayDist
			);

			// Copy to render target
			FRHITexture* DestTexture = RTResource->GetRenderTargetTexture();
			if (DestTexture)
			{
				AddCopyTexturePass(GraphBuilder, OutputSlice,
					GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DestTexture, TEXT("DebugSliceRT"))));
			}

			GraphBuilder.Execute();
		}
	);
}

void UFleshRingComponent::CleanupDebugResources()
{
	// Remove slice plane actors
	for (AActor* PlaneActor : DebugSlicePlaneActors)
	{
		if (PlaneActor)
		{
			PlaneActor->Destroy();
		}
	}
	DebugSlicePlaneActors.Empty();

	// Clean up render targets
	DebugSliceRenderTargets.Empty();

	// Clean up debug affected vertex data
	DebugAffectedData.Empty();
	DebugBindPoseVertices.Empty();
	DebugSpatialHash.Clear();
	bDebugAffectedVerticesCached = false;

	// Clean up debug Bulge vertex data
	DebugBulgeData.Empty();
	bDebugBulgeVerticesCached = false;
}

void UFleshRingComponent::CacheAffectedVerticesForDebug()
{
	// Skip if already cached
	if (bDebugAffectedVerticesCached)
	{
		return;
	}

	// Validate
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh || !FleshRingAsset)
	{
		return;
	}

	USkeletalMesh* Mesh = SkelMesh->GetSkeletalMeshAsset();
	if (!Mesh)
	{
		return;
	}

	// ===== 1. Extract bind pose vertices (only when empty - same pattern as Bulge) =====
	// Bind pose is the same unless mesh changes, so reuse cache
	if (DebugBindPoseVertices.Num() == 0)
	{
		const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			return;
		}

		// Use LOD 0
		const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
		const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

		if (NumVertices == 0)
		{
			return;
		}

		DebugBindPoseVertices.Reset(NumVertices);
		for (uint32 VertexIdx = 0; VertexIdx < NumVertices; ++VertexIdx)
		{
			const FVector3f& Position = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIdx);
			DebugBindPoseVertices.Add(Position);
		}

		// Build Spatial Hash (for O(1) queries)
		DebugSpatialHash.Build(DebugBindPoseVertices);
	}

	// ===== 2. Try to reuse actual deformation data =====
	// Reuse already computed data if Deformer is active
	if (UFleshRingDeformerInstance* DeformerInstance =
		Cast<UFleshRingDeformerInstance>(SkelMesh->GetMeshDeformerInstance()))
	{
		const TArray<FRingAffectedData>* ActualData =
			DeformerInstance->GetAffectedRingDataForDebug(0);  // LOD0

		if (ActualData && ActualData->Num() == FleshRingAsset->Rings.Num())
		{
			// Copy actual data (avoid duplicate computation)
			DebugAffectedData = *ActualData;
			bDebugAffectedVerticesCached = true;
			return;
		}
	}

	// ===== 3. Fallback: Calculate affected vertices per Ring directly =====
	// Check array size (only initialize when Ring count changed)
	if (DebugAffectedData.Num() != FleshRingAsset->Rings.Num())
	{
		DebugAffectedData.Reset();
		DebugAffectedData.SetNum(FleshRingAsset->Rings.Num());
	}

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

	for (int32 RingIdx = 0; RingIdx < FleshRingAsset->Rings.Num(); ++RingIdx)
	{
		// ★ Skip already cached Rings (supports per-Ring invalidation)
		if (DebugAffectedData[RingIdx].Vertices.Num() > 0)
		{
			continue;
		}

		const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIdx];

		// Find bone index
		const int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		// Calculate bind pose bone transform (accumulate parent chain)
		FTransform BoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BoneTransform = BoneTransform * RefBonePose[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}

		// Get SDF cache
		const FRingSDFCache* SDFCache = GetRingSDFCache(RingIdx);

		// Select affected vertices
		FRingAffectedData& RingData = DebugAffectedData[RingIdx];
		RingData.BoneName = RingSettings.BoneName;
		RingData.RingCenter = BoneTransform.GetLocation();

		// Branch by per-Ring InfluenceMode
		// - Auto: SDF-based only when SDF is valid
		// - VirtualBand: Always distance-based (variable radius)
		// - VirtualRing: Always distance-based (fixed radius)
		const bool bUseSDFForThisRing =
			(RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto) &&
			(SDFCache && SDFCache->IsValid());

		// Falloff calculation lambda (inline version of CalculateFalloff)
		auto CalcFalloff = [](float Distance, float MaxDistance, EFalloffType Type) -> float
		{
			const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);
			const float T = 1.0f - NormalizedDist;
			switch (Type)
			{
			case EFalloffType::Quadratic:
				return T * T;
			case EFalloffType::Hermite:
				return T * T * (3.0f - 2.0f * T);
			case EFalloffType::Linear:
			default:
				return T;
			}
		};

		if (bUseSDFForThisRing)
		{
			// ===== SDF mode: OBB-based Spatial Hash query =====
			// In SDF mode Influence = 1.0 (max value) - GPU shader refines with SDF
			// In debug visualization all selected vertices show as red
			const FTransform& LocalToComponent = SDFCache->LocalToComponent;
			const FVector BoundsMin = FVector(SDFCache->BoundsMin);
			const FVector BoundsMax = FVector(SDFCache->BoundsMax);

			// Extract candidates within OBB via Spatial Hash - O(1)
			TArray<int32> CandidateIndices;
			if (DebugSpatialHash.IsBuilt())
			{
				DebugSpatialHash.QueryOBB(LocalToComponent, BoundsMin, BoundsMax, CandidateIndices);
			}
			else
			{
				// Fallback: iterate all
				CandidateIndices.Reserve(DebugBindPoseVertices.Num());
				for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
				{
					CandidateIndices.Add(i);
				}
			}

			for (int32 VertexIdx : CandidateIndices)
			{
				// SDF mode: Influence = 1.0 (red) if inside OBB
				FAffectedVertex AffectedVert;
				AffectedVert.VertexIndex = static_cast<uint32>(VertexIdx);
				AffectedVert.Influence = 1.0f;
				RingData.Vertices.Add(AffectedVert);
			}
		}
		else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
		{
			// Virtual Band debug visualization disabled (code preserved)
			/*
			// ===== Virtual Band mode (SDF invalid): variable radius distance-based =====
			// Use dedicated BandOffset/BandRotation
			const FVirtualBandSettings& BandSettings = RingSettings.VirtualBand;
			const FQuat BoneRotation = BoneTransform.GetRotation();
			const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
			const FVector BandCenter = BoneTransform.GetLocation() + WorldBandOffset;
			const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
			const FVector BandAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

			// Height parameters
			const float LowerHeight = BandSettings.Lower.Height;
			const float BandHeight = BandSettings.BandHeight;
			const float UpperHeight = BandSettings.Upper.Height;
			const float TotalHeight = LowerHeight + BandHeight + UpperHeight;

			// Tightness region: Band Section only (-BandHeight/2 ~ +BandHeight/2)
			// New coordinate system: Z=0 is Mid Band center
			const float TightnessZMin = -BandHeight * 0.5f;
			const float TightnessZMax = BandHeight * 0.5f;

			// Tightness Falloff range: distance pushed as band tightens
			// Upper/Lower radius difference = bulge amount = distance to tighten
			const float UpperBulge = BandSettings.Upper.Radius - BandSettings.MidUpperRadius;
			const float LowerBulge = BandSettings.Lower.Radius - BandSettings.MidLowerRadius;
			const float TightnessFalloffRange = FMath::Max(FMath::Max(UpperBulge, LowerBulge), 1.0f);

			// Calculate max radius (for AABB query)
			const float MaxRadius = FMath::Max(
				FMath::Max(BandSettings.Lower.Radius, BandSettings.Upper.Radius),
				FMath::Max(BandSettings.MidLowerRadius, BandSettings.MidUpperRadius)
			) + TightnessFalloffRange;

			auto GetRadiusAtHeight = [&BandSettings](float LocalZ) -> float
			{
				return BandSettings.GetRadiusAtHeight(LocalZ);
			};

			// Extract candidates using Spatial Hash
			TArray<int32> CandidateIndices;
			if (DebugSpatialHash.IsBuilt())
			{
				FTransform BandLocalToComponent;
				BandLocalToComponent.SetLocation(BandCenter);
				BandLocalToComponent.SetRotation(WorldBandRotation);
				BandLocalToComponent.SetScale3D(FVector::OneVector);

				// New coordinate system: Z=0 is Mid Band center
				const float MidOffset = LowerHeight + BandHeight * 0.5f;
				const FVector LocalMin(-MaxRadius, -MaxRadius, -MidOffset);
				const FVector LocalMax(MaxRadius, MaxRadius, TotalHeight - MidOffset);
				DebugSpatialHash.QueryOBB(BandLocalToComponent, LocalMin, LocalMax, CandidateIndices);
			}
			else
			{
				CandidateIndices.Reserve(DebugBindPoseVertices.Num());
				for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
				{
					CandidateIndices.Add(i);
				}
			}

			for (int32 VertexIdx : CandidateIndices)
			{
				const FVector VertexPos = FVector(DebugBindPoseVertices[VertexIdx]);
				const FVector ToVertex = VertexPos - BandCenter;
				const float AxisDistance = FVector::DotProduct(ToVertex, BandAxis);
				const float LocalZ = AxisDistance;

				// Band Section range check (Tightness region)
				if (LocalZ < TightnessZMin || LocalZ > TightnessZMax)
				{
					continue;
				}

				const FVector RadialVec = ToVertex - BandAxis * AxisDistance;
				const float RadialDistance = RadialVec.Size();
				const float BandRadius = GetRadiusAtHeight(LocalZ);

				// Must be outside band surface for Tightness influence
				if (RadialDistance <= BandRadius)
				{
					continue;
				}

				const float DistanceOutside = RadialDistance - BandRadius;
				if (DistanceOutside > TightnessFalloffRange)
				{
					continue;
				}

				const float RadialInfluence = CalcFalloff(DistanceOutside, TightnessFalloffRange, RingSettings.FalloffType);

				// Axial Influence (falloff based on distance from Band boundary)
				float AxialInfluence = 1.0f;
				const float AxialFalloffRange = BandHeight * 0.2f;
				if (LocalZ < TightnessZMin + AxialFalloffRange)
				{
					const float Dist = TightnessZMin + AxialFalloffRange - LocalZ;
					AxialInfluence = CalcFalloff(Dist, AxialFalloffRange, RingSettings.FalloffType);
				}
				else if (LocalZ > TightnessZMax - AxialFalloffRange)
				{
					const float Dist = LocalZ - (TightnessZMax - AxialFalloffRange);
					AxialInfluence = CalcFalloff(Dist, AxialFalloffRange, RingSettings.FalloffType);
				}

				const float CombinedInfluence = RadialInfluence * AxialInfluence;

				if (CombinedInfluence > KINDA_SMALL_NUMBER)
				{
					FAffectedVertex AffectedVert;
					AffectedVert.VertexIndex = static_cast<uint32>(VertexIdx);
					AffectedVert.Influence = CombinedInfluence;
					RingData.Vertices.Add(AffectedVert);
				}
			}
			*/
		}
		else
		{
			// ===== VirtualRing mode: Cylindrical distance-based Spatial Hash query =====
			const FQuat BoneRotation = BoneTransform.GetRotation();
			const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
			const FVector RingCenter = BoneTransform.GetLocation() + WorldRingOffset;
			const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
			const FVector RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

			const float MaxDistance = RingSettings.RingRadius + RingSettings.RingThickness;
			const float HalfWidth = RingSettings.RingHeight / 2.0f;

			// Extract candidates within OBB containing cylinder via Spatial Hash - O(1)
			TArray<int32> CandidateIndices;
			if (DebugSpatialHash.IsBuilt())
			{
				// OBB query reflecting Ring rotation
				FTransform RingLocalToComponent;
				RingLocalToComponent.SetLocation(RingCenter);
				RingLocalToComponent.SetRotation(WorldRingRotation);
				RingLocalToComponent.SetScale3D(FVector::OneVector);

				const FVector LocalMin(-MaxDistance, -MaxDistance, -HalfWidth);
				const FVector LocalMax(MaxDistance, MaxDistance, HalfWidth);
				DebugSpatialHash.QueryOBB(RingLocalToComponent, LocalMin, LocalMax, CandidateIndices);
			}
			else
			{
				// Fallback: iterate all
				CandidateIndices.Reserve(DebugBindPoseVertices.Num());
				for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
				{
					CandidateIndices.Add(i);
				}
			}

			for (int32 VertexIdx : CandidateIndices)
			{
				const FVector VertexPos = FVector(DebugBindPoseVertices[VertexIdx]);
				const FVector ToVertex = VertexPos - RingCenter;
				const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);
				const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
				const float RadialDistance = RadialVec.Size();

				if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
				{
					const float DistFromRingSurface = FMath::Abs(RadialDistance - RingSettings.RingRadius);
					const float RadialInfluence = CalcFalloff(DistFromRingSurface, RingSettings.RingThickness, RingSettings.FalloffType);
					const float AxialInfluence = CalcFalloff(FMath::Abs(AxisDistance), HalfWidth, RingSettings.FalloffType);
					const float CombinedInfluence = RadialInfluence * AxialInfluence;

					if (CombinedInfluence > KINDA_SMALL_NUMBER)
					{
						FAffectedVertex AffectedVert;
						AffectedVert.VertexIndex = static_cast<uint32>(VertexIdx);
						AffectedVert.Influence = CombinedInfluence;
						RingData.Vertices.Add(AffectedVert);
					}
				}
			}
		}

		UE_LOG(LogFleshRingComponent, Verbose, TEXT("CacheAffectedVerticesForDebug: Ring[%d] '%s' - %d affected vertices, Mode=%s"),
			RingIdx, *RingSettings.BoneName.ToString(), RingData.Vertices.Num(),
			bUseSDFForThisRing ? TEXT("SDF") : TEXT("VirtualRing"));
	}

	bDebugAffectedVerticesCached = true;

	UE_LOG(LogFleshRingComponent, Verbose, TEXT("CacheAffectedVerticesForDebug: Cached %d rings, %d total vertices"),
		DebugAffectedData.Num(), DebugBindPoseVertices.Num());
}

void UFleshRingComponent::DrawBulgeHeatmap(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Cache first if not already cached
	if (!bDebugBulgeVerticesCached)
	{
		CacheBulgeVerticesForDebug();
	}

	// Validate data
	if (!DebugBulgeData.IsValidIndex(RingIndex) ||
		DebugBindPoseVertices.Num() == 0)
	{
		return;
	}

	const FRingAffectedData& RingData = DebugBulgeData[RingIndex];
	if (RingData.Vertices.Num() == 0)
	{
		return;
	}

	// Current skeletal mesh component
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh)
	{
		return;
	}

	// Component → World transform
	FTransform CompTransform = SkelMesh->GetComponentTransform();

	// For each Bulge affected vertex
	for (const FAffectedVertex& AffectedVert : RingData.Vertices)
	{
		if (!DebugBindPoseVertices.IsValidIndex(AffectedVert.VertexIndex))
		{
			continue;
		}

		// Bind pose position (component space)
		const FVector3f& BindPosePos = DebugBindPoseVertices[AffectedVert.VertexIndex];

		// Transform to world space
		FVector WorldPos = CompTransform.TransformPosition(FVector(BindPosePos));

		// Color based on influence (cyan → magenta gradient, high contrast)
		float Influence = AffectedVert.Influence;
		float T = FMath::Clamp(Influence, 0.0f, 1.0f);

		// Cyan(weak) → Magenta(strong) gradient (high contrast against skin tone)
		FColor PointColor(
			FMath::RoundToInt(255 * T),          // R: 0 → 255
			FMath::RoundToInt(255 * (1.0f - T)), // G: 255 → 0
			255                                  // B: Always 255 (keep bright)
		);

		// Point size (proportional to influence, larger)
		float PointSize = 5.0f + T * 7.0f;  // Range 5~12

		// Outline effect: draw black larger point first, then colored point on top
		DrawDebugPoint(World, WorldPos, PointSize + 2.0f, FColor::Black, false, -1.0f, SDPG_Foreground);
		DrawDebugPoint(World, WorldPos, PointSize, PointColor, false, -1.0f, SDPG_Foreground);
	}

	// Display info on screen
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Orange,
			FString::Printf(TEXT("Ring[%d] Bulge: %d vertices (Smoothstep filtered)"),
				RingIndex, RingData.Vertices.Num()));
	}
}

void UFleshRingComponent::CacheBulgeVerticesForDebug()
{
	// Skip if already cached
	if (bDebugBulgeVerticesCached)
	{
		// ★ Check bEnableBulge state change (works correctly even with single Ring)
		if (FleshRingAsset)
		{
			bool bNeedsRecache = false;
			const int32 NumRings = FleshRingAsset->Rings.Num();

			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				if (!DebugBulgeData.IsValidIndex(RingIdx)) continue;

				const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIdx];
				const bool bHasCachedData = DebugBulgeData[RingIdx].Vertices.Num() > 0;

				// Cache exists but Bulge disabled → need to clear
				if (bHasCachedData && !RingSettings.bEnableBulge)
				{
					DebugBulgeData[RingIdx].Vertices.Reset();
				}
				// No cache but Bulge enabled → need to recache
				else if (!bHasCachedData && RingSettings.bEnableBulge)
				{
					bNeedsRecache = true;
				}
			}

			if (!bNeedsRecache)
			{
				return;
			}
			// If bNeedsRecache = true, proceed below to recache
		}
		else
		{
			return;
		}
	}

	// Validate
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh || !FleshRingAsset)
	{
		return;
	}

	// Cache bind pose vertices if empty
	if (DebugBindPoseVertices.Num() == 0)
	{
		CacheAffectedVerticesForDebug();
	}

	if (DebugBindPoseVertices.Num() == 0)
	{
		return;
	}

	// Check DebugBulgeData array size (only initialize when Ring count changed)
	if (DebugBulgeData.Num() != FleshRingAsset->Rings.Num())
	{
		DebugBulgeData.Reset();
		DebugBulgeData.SetNum(FleshRingAsset->Rings.Num());
	}

	for (int32 RingIdx = 0; RingIdx < FleshRingAsset->Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIdx];
		FRingAffectedData& BulgeData = DebugBulgeData[RingIdx];

		// ★ Check bEnableBulge first (clear cache if disabled)
		if (!RingSettings.bEnableBulge)
		{
			if (BulgeData.Vertices.Num() > 0)
			{
				BulgeData.Vertices.Reset();
			}
			continue;
		}

		// ★ Only check cache when bEnableBulge == true (supports per-Ring invalidation)
		if (BulgeData.Vertices.Num() > 0)
		{
			continue;
		}

		BulgeData.BoneName = RingSettings.BoneName;

		// ===== Calculate Ring info: SDF mode vs VirtualRing mode branching =====
		FTransform LocalToComponent = FTransform::Identity;
		FVector3f RingCenter;
		FVector3f RingAxis;
		float RingHeight;
		float RingRadius;
		int32 DetectedDirection = 0;
		bool bUseLocalSpace = false;  // VirtualRing mode uses Component Space directly
		FQuat VirtualRingRotation = FQuat::Identity;  // For VirtualRing mode OBB query

		// ★ Branch by InfluenceMode: Access SDFCache only in Auto mode
		const FRingSDFCache* SDFCache = nullptr;
		bool bHasValidSDF = false;
		if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto)
		{
			SDFCache = GetRingSDFCache(RingIdx);
			bHasValidSDF = (SDFCache && SDFCache->IsValid());
		}

		if (bHasValidSDF)
		{
			// ===== Auto mode: Get Ring info from SDF cache =====
			bUseLocalSpace = true;
			LocalToComponent = SDFCache->LocalToComponent;
			FVector3f BoundsMin = SDFCache->BoundsMin;
			FVector3f BoundsMax = SDFCache->BoundsMax;
			FVector3f BoundsSize = BoundsMax - BoundsMin;
			RingCenter = (BoundsMin + BoundsMax) * 0.5f;

			// Detect Ring axis (shortest axis)
			if (BoundsSize.X <= BoundsSize.Y && BoundsSize.X <= BoundsSize.Z)
				RingAxis = FVector3f(1, 0, 0);
			else if (BoundsSize.Y <= BoundsSize.X && BoundsSize.Y <= BoundsSize.Z)
				RingAxis = FVector3f(0, 1, 0);
			else
				RingAxis = FVector3f(0, 0, 1);

			// Calculate Ring size (same as FleshRingBulgeProviders.cpp)
			RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
			RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;
			DetectedDirection = SDFCache->DetectedBulgeDirection;
		}
		else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
		{
			// ===== VirtualRing mode: Get directly from Ring parameters (Component Space) =====
			bUseLocalSpace = false;

			// Get Bone Transform
			FTransform BoneTransform = FTransform::Identity;
			if (SkelMesh)
			{
				int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
				if (BoneIndex != INDEX_NONE)
				{
					BoneTransform = SkelMesh->GetBoneTransform(BoneIndex, FTransform::Identity);
				}
			}

			// RingCenter = Bone Position + RingOffset (Bone rotation applied)
			const FQuat BoneRotation = BoneTransform.GetRotation();
			const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
			RingCenter = FVector3f(BoneTransform.GetLocation() + WorldRingOffset);

			// RingAxis = Z axis of Bone Rotation * RingRotation
			const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
			RingAxis = FVector3f(WorldRingRotation.RotateVector(FVector::ZAxisVector));
			VirtualRingRotation = WorldRingRotation;  // Store for OBB query

			// Use Ring size directly
			RingHeight = RingSettings.RingHeight;
			RingRadius = RingSettings.RingRadius;
			DetectedDirection = 0;  // VirtualRing mode cannot auto-detect, bidirectional
		}
		else
		{
			// Skip if no SDF and not VirtualRing
			continue;
		}

		// Bulge start distance (Ring boundary)
		const float BulgeStartDist = RingHeight * 0.5f;

		// Orthogonal range limits (each axis controlled independently)
		// AxialLimit = start point + extension (extends by RingHeight*0.5 when AxialRange=1)
		const float AxialLimit = BulgeStartDist + RingHeight * 0.5f * RingSettings.BulgeAxialRange;
		const float RadialLimit = RingRadius * RingSettings.BulgeRadialRange;

		// Determine direction (0 = bidirectional) - DetectedDirection already calculated above
		int32 FinalDirection = 0;
		switch (RingSettings.BulgeDirection)
		{
		case EBulgeDirectionMode::Auto:
			FinalDirection = DetectedDirection;  // 0 means bidirectional (closed mesh)
			break;
		case EBulgeDirectionMode::Bidirectional:
			FinalDirection = 0;  // Bidirectional
			break;
		case EBulgeDirectionMode::Positive:
			FinalDirection = 1;
			break;
		case EBulgeDirectionMode::Negative:
			FinalDirection = -1;
			break;
		}

		BulgeData.RingCenter = FVector(RingCenter);

		// Extract candidate vertices only via Spatial Hash - O(1)
		TArray<int32> CandidateIndices;
		if (DebugSpatialHash.IsBuilt())
		{
			if (bUseLocalSpace)
			{
				// SDF mode: OBB query (considering Bulge region expansion)
				const FVector3f& BoundsMin = SDFCache->BoundsMin;
				const FVector3f& BoundsMax = SDFCache->BoundsMax;
				// Bulge region expansion: consider both Axial(Z) + Radial(X/Y)
				const float AxialExtend = AxialLimit - BulgeStartDist;
				const float RadialExtend = FMath::Max(0.0f, RadialLimit - RingRadius);
				FVector ExpandedMin = FVector(BoundsMin) - FVector(RadialExtend, RadialExtend, AxialExtend);
				FVector ExpandedMax = FVector(BoundsMax) + FVector(RadialExtend, RadialExtend, AxialExtend);
				DebugSpatialHash.QueryOBB(LocalToComponent, ExpandedMin, ExpandedMax, CandidateIndices);
			}
			else
			{
				// VirtualRing mode: OBB query (reflecting Ring rotation, including Bulge region)
				FTransform RingLocalToComponent;
				RingLocalToComponent.SetLocation(FVector(RingCenter));
				RingLocalToComponent.SetRotation(VirtualRingRotation);
				RingLocalToComponent.SetScale3D(FVector::OneVector);

				const float MaxTaperFactor = 1.0f + FMath::Max(RingSettings.BulgeRadialTaper, 0.0f);
				const float MaxExtent = FMath::Max(RadialLimit * MaxTaperFactor, AxialLimit);
				const FVector LocalMin(-MaxExtent, -MaxExtent, -AxialLimit);
				const FVector LocalMax(MaxExtent, MaxExtent, AxialLimit);
				DebugSpatialHash.QueryOBB(RingLocalToComponent, LocalMin, LocalMax, CandidateIndices);
			}
		}
		else
		{
			// Fallback: iterate all
			CandidateIndices.Reserve(DebugBindPoseVertices.Num());
			for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
			{
				CandidateIndices.Add(i);
			}
		}

		// Iterate candidate vertices only
		for (int32 VertIdx : CandidateIndices)
		{
			FVector CompSpacePos = FVector(DebugBindPoseVertices[VertIdx]);
			FVector3f VertexPos;

			if (bUseLocalSpace)
			{
				// SDF mode: Component Space → Ring Local Space transform
				// InverseTransformPosition: (V - Trans) * Rot^-1 / Scale (correct order)
				FVector LocalSpacePos = LocalToComponent.InverseTransformPosition(CompSpacePos);
				VertexPos = FVector3f(LocalSpacePos);
			}
			else
			{
				// VirtualRing mode: Use Component Space directly (RingCenter, RingAxis already in Component Space)
				VertexPos = FVector3f(CompSpacePos);
			}

			// Vector from Ring center
			FVector3f ToVertex = VertexPos - RingCenter;

			// 1. Axial distance (up/down)
			float AxialComponent = FVector3f::DotProduct(ToVertex, RingAxis);
			float AxialDist = FMath::Abs(AxialComponent);

			// Exclude before Bulge start point (Ring boundary) - Tightness region
			if (AxialDist < BulgeStartDist)
			{
				continue;
			}

			// Check axial range exceeded
			if (AxialDist > AxialLimit)
			{
				continue;
			}

			// 2. Radial distance (side)
			FVector3f RadialVec = ToVertex - RingAxis * AxialComponent;
			float RadialDist = RadialVec.Size();

			// Dynamic RadialLimit adjustment based on Axial distance (RadialTaper: negative=shrink, 0=cylinder, positive=expand)
			const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
			const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * RingSettings.BulgeRadialTaper);

			// Check radial range exceeded (prevents affecting other thigh)
			if (RadialDist > DynamicRadialLimit)
			{
				continue;
			}

			// 3. Direction filtering (only one side if FinalDirection != 0)
			if (FinalDirection != 0)
			{
				int32 VertexSide = (AxialComponent > 0.0f) ? 1 : -1;
				if (VertexSide != FinalDirection)
				{
					continue;
				}
			}

			// 4. Axial distance-based Falloff attenuation
			// Smooth attenuation from 1.0 at Ring boundary to 0 at AxialLimit
			const float AxialFalloffRange = AxialLimit - BulgeStartDist;
			float NormalizedDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
			float ClampedDist = FMath::Clamp(NormalizedDist, 0.0f, 1.0f);

			// Use same function for actual calculation and visualization
			float BulgeInfluence = FFleshRingFalloff::Evaluate(ClampedDist, RingSettings.BulgeFalloff);

			if (BulgeInfluence > KINDA_SMALL_NUMBER)
			{
				FAffectedVertex BulgeVert;
				BulgeVert.VertexIndex = VertIdx;
				BulgeVert.Influence = BulgeInfluence;
				BulgeData.Vertices.Add(BulgeVert);
			}
		}

		const TCHAR* ModeStr = TEXT("Unknown");
		switch (RingSettings.BulgeDirection)
		{
		case EBulgeDirectionMode::Auto: ModeStr = TEXT("Auto"); break;
		case EBulgeDirectionMode::Bidirectional: ModeStr = TEXT("Both"); break;
		case EBulgeDirectionMode::Positive: ModeStr = TEXT("Positive"); break;
		case EBulgeDirectionMode::Negative: ModeStr = TEXT("Negative"); break;
		}
		UE_LOG(LogFleshRingComponent, Log, TEXT("CacheBulgeVerticesForDebug: Ring[%d] - %d Bulge vertices (Direction: %d, Detected: %d, Mode: %s, RingAxis: %s)"),
			RingIdx, BulgeData.Vertices.Num(), FinalDirection, DetectedDirection, ModeStr, *RingAxis.ToString());
	}

	bDebugBulgeVerticesCached = true;
}

void UFleshRingComponent::DrawBulgeDirectionArrow(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!FleshRingAsset || !FleshRingAsset->Rings.IsValidIndex(RingIndex))
	{
		return;
	}

	const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIndex];

	// Skip if Bulge disabled
	if (!RingSettings.bEnableBulge)
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// ★ Branch by InfluenceMode: Access SDFCache only in Auto mode
	const FRingSDFCache* SDFCache = nullptr;
	bool bHasValidSDF = false;
	if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto)
	{
		SDFCache = GetRingSDFCache(RingIndex);
		bHasValidSDF = (SDFCache && SDFCache->IsValid());
	}

	// ===== Calculate Ring info: SDF mode vs VirtualRing mode branching =====
	FVector WorldCenter;
	FVector WorldZAxis;
	float ArrowLength;
	int32 DetectedDirection = 0;

	if (bHasValidSDF)
	{
		// ===== Auto mode: Get info from SDF cache =====
		DetectedDirection = SDFCache->DetectedBulgeDirection;

		// OBB center position (local space)
		FVector LocalCenter = FVector(SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;

		// Component → World transform
		FTransform LocalToWorld = SDFCache->LocalToComponent;
		if (SkelMesh)
		{
			LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
		}

		WorldCenter = LocalToWorld.TransformPosition(LocalCenter);
		FQuat WorldRotation = LocalToWorld.GetRotation();

		// Transform local Z axis to world space
		FVector LocalZAxis = FVector(0.0f, 0.0f, 1.0f);
		WorldZAxis = WorldRotation.RotateVector(LocalZAxis);

		// Arrow size (proportional to SDF volume size)
		ArrowLength = FVector(SDFCache->BoundsMax - SDFCache->BoundsMin).Size() * 0.05f;
	}
	else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
	{
		// ===== VirtualRing mode: Get directly from Ring parameters =====
		DetectedDirection = 0;  // VirtualRing mode cannot auto-detect

		// Get Bone Transform
		FTransform BoneTransform = FTransform::Identity;
		if (SkelMesh)
		{
			int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);
			}
		}

		// RingCenter (World Space)
		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
		WorldCenter = BoneTransform.GetLocation() + WorldRingOffset;

		// RingAxis (World Space)
		const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
		WorldZAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

		// Arrow size (proportional to Ring radius)
		ArrowLength = RingSettings.RingRadius * 0.1f;
	}
	else
	{
		// Skip if no SDF and not VirtualRing
		return;
	}

	// Determine final direction to use (0 = bidirectional)
	int32 FinalDirection = 0;
	switch (RingSettings.BulgeDirection)
	{
	case EBulgeDirectionMode::Auto:
		FinalDirection = DetectedDirection;  // 0 means bidirectional
		break;
	case EBulgeDirectionMode::Bidirectional:
		FinalDirection = 0;  // Bidirectional
		break;
	case EBulgeDirectionMode::Positive:
		FinalDirection = 1;
		break;
	case EBulgeDirectionMode::Negative:
		FinalDirection = -1;
		break;
	}

	// Arrow color: unified to white
	FColor ArrowColor = FColor::White;

	// Draw arrow (SDPG_Foreground to display in front of mesh)
	if (bShowBulgeArrows)
	{
		const float ArrowHeadSize = 0.5f;  // Arrow head size
		const float ArrowThickness = 0.5f; // Arrow thickness

		if (FinalDirection == 0)
		{
			// Bidirectional: draw arrows both up and down
			FVector ArrowEndUp = WorldCenter + WorldZAxis * ArrowLength;
			FVector ArrowEndDown = WorldCenter - WorldZAxis * ArrowLength;
			DrawDebugDirectionalArrow(World, WorldCenter, ArrowEndUp, ArrowHeadSize, ArrowColor, false, -1.0f, SDPG_Foreground, ArrowThickness);
			DrawDebugDirectionalArrow(World, WorldCenter, ArrowEndDown, ArrowHeadSize, ArrowColor, false, -1.0f, SDPG_Foreground, ArrowThickness);
		}
		else
		{
			// Unidirectional
			FVector ArrowDirection = WorldZAxis * static_cast<float>(FinalDirection);
			FVector ArrowEnd = WorldCenter + ArrowDirection * ArrowLength;
			DrawDebugDirectionalArrow(World, WorldCenter, ArrowEnd, ArrowHeadSize, ArrowColor, false, -1.0f, SDPG_Foreground, ArrowThickness);
		}
	}

	// Display info on screen
	if (GEngine)
	{
		FString ModeStr;
		switch (RingSettings.BulgeDirection)
		{
		case EBulgeDirectionMode::Auto: ModeStr = TEXT("Auto"); break;
		case EBulgeDirectionMode::Bidirectional: ModeStr = TEXT("Both"); break;
		case EBulgeDirectionMode::Positive: ModeStr = TEXT("+Z"); break;
		case EBulgeDirectionMode::Negative: ModeStr = TEXT("-Z"); break;
		}
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, ArrowColor,
			FString::Printf(TEXT("Ring[%d] Bulge Dir: %s (Detected: %d, Final: %d)"),
				RingIndex, *ModeStr, DetectedDirection, FinalDirection));
	}
}

void UFleshRingComponent::DrawBulgeRange(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!FleshRingAsset || !FleshRingAsset->Rings.IsValidIndex(RingIndex))
	{
		return;
	}

	const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIndex];

	// Skip if Bulge disabled
	if (!RingSettings.bEnableBulge)
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// Color (orange)
	const FColor CylinderColor(255, 180, 50, 200);
	const float LineThickness = 0.15f;
	const int32 CircleSegments = 32;

	// ★ Correction coefficient per Falloff type (based on Evaluate(q) = KINDA_SMALL_NUMBER)
	// Actual Bulge selection: included if BulgeInfluence > 0.0001
	auto GetFalloffCorrection = [](EFleshRingFalloffType FalloffType) -> float
	{
		switch (FalloffType)
		{
		case EFleshRingFalloffType::Linear:
			return 1.0f;    // 1-q = 0.0001 → q ≈ 1.0
		case EFleshRingFalloffType::Quadratic:
			return 0.99f;   // (1-q)² = 0.0001 → q = 0.99
		case EFleshRingFalloffType::Hermite:
			return 0.99f;   // t²(3-2t) = 0.0001 → q ≈ 0.99
		case EFleshRingFalloffType::WendlandC2:
			return 0.93f;   // (1-q)⁴(4q+1) = 0.0001 → q ≈ 0.93
		case EFleshRingFalloffType::Smootherstep:
			return 0.98f;   // t³(t(6t-15)+10) = 0.0001 → q ≈ 0.98
		default:
			return 1.0f;
		}
	};
	const float FalloffCorrection = GetFalloffCorrection(RingSettings.BulgeFalloff);

	// ===== VirtualBand mode: variable radius shape =====
	if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
	{
		const FVirtualBandSettings& Band = RingSettings.VirtualBand;

		// Get Bone Transform
		FTransform BoneTransform = FTransform::Identity;
		if (SkelMesh)
		{
			int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);
			}
		}

		// Band Center/Axis (World Space)
		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldBandOffset = BoneRotation.RotateVector(Band.BandOffset);
		const FVector WorldCenter = BoneTransform.GetLocation() + WorldBandOffset;
		const FQuat WorldBandRotation = BoneRotation * Band.BandRotation;
		const FVector WorldZAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

		// Calculate two vectors perpendicular to axis
		FVector Tangent, Binormal;
		WorldZAxis.FindBestAxisVectors(Tangent, Binormal);

		const float BandHalfHeight = Band.BandHeight * 0.5f;
		const float RadialRange = RingSettings.BulgeRadialRange;
		// Apply per-falloff-type correction
		const float AxialRange = RingSettings.BulgeAxialRange * FalloffCorrection;

		// Upper Bulge region (UpperBulgeStrength > 0)
		// From Band top (+BandHalfHeight) to Upper Section end (+BandHalfHeight + Upper.Height)
		// + AxialRange extension
		if (RingSettings.UpperBulgeStrength > 0.01f && Band.Upper.Height > 0.01f)
		{
			const float UpperStart = BandHalfHeight;
			const float UpperEnd = BandHalfHeight + Band.Upper.Height * AxialRange;
			const int32 NumSlices = 4;

			// Draw circles at multiple heights (to represent variable radius)
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = FMath::Lerp(UpperStart, UpperEnd, T);
				float BaseRadius = Band.GetRadiusAtHeight(LocalZ);
				float BulgeRadius = BaseRadius * RadialRange;

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(BulgeRadius);

				// Draw circle
				DrawDebugCircle(World, SlicePos, BulgeRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			// 4 vertical lines (connecting slices)
			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		// Lower Bulge region (LowerBulgeStrength > 0)
		// From Band bottom (-BandHalfHeight) to Lower Section end (-BandHalfHeight - Lower.Height)
		// + AxialRange extension
		if (RingSettings.LowerBulgeStrength > 0.01f && Band.Lower.Height > 0.01f)
		{
			const float LowerStart = -BandHalfHeight;
			const float LowerEnd = -BandHalfHeight - Band.Lower.Height * AxialRange;
			const int32 NumSlices = 4;

			// Draw circles at multiple heights (to represent variable radius)
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = FMath::Lerp(LowerStart, LowerEnd, T);
				float BaseRadius = Band.GetRadiusAtHeight(LocalZ);
				float BulgeRadius = BaseRadius * RadialRange;

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(BulgeRadius);

				// Draw circle
				DrawDebugCircle(World, SlicePos, BulgeRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			// 4 vertical lines (connecting slices)
			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		return;
	}

	// ===== Auto/VirtualRing mode: cone shape =====
	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	const bool bHasValidSDF = SDFCache && SDFCache->IsValid();

	// ★ SDF mode: calculate all points in local space then transform to world
	// LocalToComponent may include scale, so each point must be transformed individually
	if (bHasValidSDF)
	{
		// Calculate geometry info from SDF bounds (local space)
		const FVector3f BoundsSize = SDFCache->BoundsMax - SDFCache->BoundsMin;
		const FVector LocalCenter = FVector(SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;
		const float RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
		const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;

		// Ring axis = shortest SDF bounds axis (same as actual Bulge calculation)
		FVector LocalRingAxis;
		if (BoundsSize.X <= BoundsSize.Y && BoundsSize.X <= BoundsSize.Z)
		{
			LocalRingAxis = FVector(1.0f, 0.0f, 0.0f);
		}
		else if (BoundsSize.Y <= BoundsSize.X && BoundsSize.Y <= BoundsSize.Z)
		{
			LocalRingAxis = FVector(0.0f, 1.0f, 0.0f);
		}
		else
		{
			LocalRingAxis = FVector(0.0f, 0.0f, 1.0f);
		}

		// Two vectors perpendicular to axis (local space)
		FVector LocalTangent, LocalBinormal;
		LocalRingAxis.FindBestAxisVectors(LocalTangent, LocalBinormal);

		// Local → World transform
		FTransform LocalToWorld = SDFCache->LocalToComponent;
		if (SkelMesh)
		{
			LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
		}

		// Bulge range (local space) - apply per-falloff-type correction
		const float BulgeRadialExtent = RingRadius * RingSettings.BulgeRadialRange;
		const float AxialExtent = RingHeight * 0.5f * RingSettings.BulgeAxialRange * FalloffCorrection;
		const float RingHalfHeight = RingHeight * 0.5f;

		const int32 NumSlices = 4;

		// Lambda: transform local space point to world
		auto TransformToWorld = [&LocalToWorld](const FVector& LocalPos) -> FVector
		{
			return LocalToWorld.TransformPosition(LocalPos);
		};

		// Upper cone (UpperBulgeStrength > 0)
		if (RingSettings.UpperBulgeStrength > 0.01f)
		{
			// Store circle points for each slice (world space)
			TArray<TArray<FVector>> SliceCirclePoints;
			SliceCirclePoints.SetNum(NumSlices + 1);

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = RingHalfHeight + AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * RingSettings.BulgeRadialTaper);

				// Circle center in local space
				FVector LocalSliceCenter = LocalCenter + LocalRingAxis * LocalZ;

				// Calculate circle points in local space then transform to world
				TArray<FVector>& CirclePoints = SliceCirclePoints[i];
				CirclePoints.SetNum(CircleSegments + 1);

				for (int32 j = 0; j <= CircleSegments; ++j)
				{
					float Angle = static_cast<float>(j) / static_cast<float>(CircleSegments) * 2.0f * PI;
					FVector LocalPoint = LocalSliceCenter + LocalTangent * (FMath::Cos(Angle) * DynamicRadius) + LocalBinormal * (FMath::Sin(Angle) * DynamicRadius);
					CirclePoints[j] = TransformToWorld(LocalPoint);
				}

				// Draw circle
				for (int32 j = 0; j < CircleSegments; ++j)
				{
					DrawDebugLine(World, CirclePoints[j], CirclePoints[j + 1], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}

			// 4 vertical lines (connecting slices)
			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				int32 PointIdx = (CircleSegments * LineIdx) / 4;
				for (int32 i = 0; i < NumSlices; ++i)
				{
					DrawDebugLine(World, SliceCirclePoints[i][PointIdx], SliceCirclePoints[i + 1][PointIdx], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		// Lower cone (LowerBulgeStrength > 0)
		if (RingSettings.LowerBulgeStrength > 0.01f)
		{
			TArray<TArray<FVector>> SliceCirclePoints;
			SliceCirclePoints.SetNum(NumSlices + 1);

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = -RingHalfHeight - AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * RingSettings.BulgeRadialTaper);

				FVector LocalSliceCenter = LocalCenter + LocalRingAxis * LocalZ;

				TArray<FVector>& CirclePoints = SliceCirclePoints[i];
				CirclePoints.SetNum(CircleSegments + 1);

				for (int32 j = 0; j <= CircleSegments; ++j)
				{
					float Angle = static_cast<float>(j) / static_cast<float>(CircleSegments) * 2.0f * PI;
					FVector LocalPoint = LocalSliceCenter + LocalTangent * (FMath::Cos(Angle) * DynamicRadius) + LocalBinormal * (FMath::Sin(Angle) * DynamicRadius);
					CirclePoints[j] = TransformToWorld(LocalPoint);
				}

				for (int32 j = 0; j < CircleSegments; ++j)
				{
					DrawDebugLine(World, CirclePoints[j], CirclePoints[j + 1], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}

			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				int32 PointIdx = (CircleSegments * LineIdx) / 4;
				for (int32 i = 0; i < NumSlices; ++i)
				{
					DrawDebugLine(World, SliceCirclePoints[i][PointIdx], SliceCirclePoints[i + 1][PointIdx], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		return;
	}

	// ===== VirtualRing mode: legacy approach =====
	if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
	{
		FTransform BoneTransform = FTransform::Identity;
		if (SkelMesh)
		{
			int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);
			}
		}

		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
		const FVector WorldCenter = BoneTransform.GetLocation() + WorldRingOffset;

		const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
		const FVector WorldZAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

		// Calculate Bulge range - apply per-falloff-type correction
		const float RingRadius = RingSettings.RingRadius;
		const float RingHeight = RingSettings.RingHeight;
		const float BulgeRadialExtent = RingRadius * RingSettings.BulgeRadialRange;
		const float AxialExtent = RingHeight * 0.5f * RingSettings.BulgeAxialRange * FalloffCorrection;
		const float RingHalfHeight = RingHeight * 0.5f;

		// Two vectors perpendicular to axis
		FVector Tangent, Binormal;
		WorldZAxis.FindBestAxisVectors(Tangent, Binormal);

		const int32 NumSlices = 4;

		// Upper cone (UpperBulgeStrength > 0)
		if (RingSettings.UpperBulgeStrength > 0.01f)
		{
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = RingHalfHeight + AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * RingSettings.BulgeRadialTaper);

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(DynamicRadius);

				DrawDebugCircle(World, SlicePos, DynamicRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		// Lower cone (LowerBulgeStrength > 0)
		if (RingSettings.LowerBulgeStrength > 0.01f)
		{
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = -RingHalfHeight - AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * RingSettings.BulgeRadialTaper);

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(DynamicRadius);

				DrawDebugCircle(World, SlicePos, DynamicRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}
	}
}

// ============================================================================
// GPU Debug Rendering Functions
// ============================================================================

TArray<uint32> UFleshRingComponent::GetVisibilityMaskArray() const
{
	TArray<uint32> MaskArray;

	// Return empty array if no FleshRingAsset (all treated as visible)
	if (!FleshRingAsset)
	{
		return MaskArray;
	}

	const int32 NumRings = FleshRingAsset->Rings.Num();
	if (NumRings == 0)
	{
		return MaskArray;
	}

	// Calculate required uint32 element count: ceil(NumRings / 32)
	const int32 NumElements = (NumRings + 31) / 32;
	MaskArray.SetNumZeroed(NumElements);

	// Set each Ring's visibility as bitmask
	for (int32 i = 0; i < NumRings; ++i)
	{
		if (FleshRingAsset->Rings[i].bEditorVisible)
		{
			const int32 ElementIndex = i / 32;
			const int32 BitIndex = i % 32;
			MaskArray[ElementIndex] |= (1u << BitIndex);
		}
	}

	return MaskArray;
}

void UFleshRingComponent::InitializeDebugPointComponents()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	USceneComponent* AttachParent = Owner->GetRootComponent();

	// Create unified debug point component
	if (!DebugPointComponent)
	{
		DebugPointComponent = NewObject<UFleshRingDebugPointComponent>(
			Owner, UFleshRingDebugPointComponent::StaticClass(),
			FName(*FString::Printf(TEXT("%s_DebugPoints"), *GetName())));

		if (DebugPointComponent)
		{
			if (AttachParent)
			{
				DebugPointComponent->SetupAttachment(AttachParent);
			}
			DebugPointComponent->RegisterComponent();
		}
	}
}

void UFleshRingComponent::UpdateTightnessDebugPointComponent()
{
	// Initialize if component doesn't exist
	if (!DebugPointComponent)
	{
		InitializeDebugPointComponents();
	}

	if (!DebugPointComponent)
	{
		return;
	}

	// Disable Tightness rendering if bShowAffectedVertices is disabled
	if (!bShowAffectedVertices || !bShowDebugVisualization)
	{
		DebugPointComponent->ClearTightnessBuffer();
		return;
	}

	// Get cached DebugPointBuffer from DeformerInstance
	if (!InternalDeformer)
	{
		DebugPointComponent->ClearTightnessBuffer();
		return;
	}

	UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance();
	if (!DeformerInstance)
	{
		DebugPointComponent->ClearTightnessBuffer();
		return;
	}

	// Get CachedDebugPointBufferSharedPtr
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugPointBufferSharedPtr = DeformerInstance->GetCachedDebugPointBufferSharedPtr();
	if (!DebugPointBufferSharedPtr.IsValid())
	{
		DebugPointComponent->ClearTightnessBuffer();
		return;
	}

	// Pass Tightness buffer to DebugPointComponent (unlimited ring support)
	DebugPointComponent->SetTightnessBuffer(DebugPointBufferSharedPtr, GetVisibilityMaskArray());
}

void UFleshRingComponent::UpdateBulgeDebugPointComponent()
{
	// Initialize if component doesn't exist
	if (!DebugPointComponent)
	{
		InitializeDebugPointComponents();
	}

	if (!DebugPointComponent)
	{
		return;
	}

	// Disable Bulge rendering if bShowBulgeHeatmap is disabled
	if (!bShowBulgeHeatmap || !bShowDebugVisualization)
	{
		DebugPointComponent->ClearBulgeBuffer();
		return;
	}

	// Get cached DebugBulgePointBuffer from DeformerInstance
	if (!InternalDeformer)
	{
		DebugPointComponent->ClearBulgeBuffer();
		return;
	}

	UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance();
	if (!DeformerInstance)
	{
		DebugPointComponent->ClearBulgeBuffer();
		return;
	}

	// Get CachedDebugBulgePointBufferSharedPtr
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugBulgePointBufferSharedPtr = DeformerInstance->GetCachedDebugBulgePointBufferSharedPtr();
	if (!DebugBulgePointBufferSharedPtr.IsValid())
	{
		DebugPointComponent->ClearBulgeBuffer();
		return;
	}

	// Pass Bulge buffer to DebugPointComponent (unlimited ring support)
	DebugPointComponent->SetBulgeBuffer(DebugBulgePointBufferSharedPtr, GetVisibilityMaskArray());
}

#endif // WITH_EDITOR

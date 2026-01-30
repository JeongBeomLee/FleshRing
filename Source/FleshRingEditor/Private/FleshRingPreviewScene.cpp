// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingSubdivisionProcessor.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/MeshDeformerInstance.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Editor.h"
#include "RenderingThread.h"         // For FlushRenderingCommands
#include "UObject/UObjectGlobals.h"  // For CollectGarbage
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"

FFleshRingPreviewScene::FFleshRingPreviewScene(const ConstructionValues& CVS)
	: FAdvancedPreviewScene(CVS)
{
	// Create preview actor
	CreatePreviewActor();
}

FFleshRingPreviewScene::~FFleshRingPreviewScene()
{
	// Unsubscribe from delegate
	UnbindFromAssetDelegate();

	// Restore original mesh (if PreviewSubdividedMesh was applied)
	if (SkeletalMeshComponent && CachedOriginalMesh.IsValid())
	{
		USkeletalMesh* CurrentMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
		USkeletalMesh* OriginalMesh = CachedOriginalMesh.Get();
		if (CurrentMesh != OriginalMesh)
		{
			// ★ Disable Undo
			ITransaction* OldGUndo = GUndo;
			GUndo = nullptr;

			SkeletalMeshComponent->SetSkeletalMesh(OriginalMesh);

			GUndo = OldGUndo;

			UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Restored original mesh '%s' on destruction"),
				OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
		}
	}
	CachedOriginalMesh.Reset();

	// Clean up PreviewSubdividedMesh
	ClearPreviewMesh();

	// Clean up Ring mesh components
	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RemoveComponent(RingComp);
		}
	}
	RingMeshComponents.Empty();

	// Clean up preview actor
	if (PreviewActor)
	{
		PreviewActor->Destroy();
		PreviewActor = nullptr;
	}

	SkeletalMeshComponent = nullptr;
	FleshRingComponent = nullptr;
}

void FFleshRingPreviewScene::CreatePreviewActor()
{
	// Create actor in preview world
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(TEXT("FleshRingPreviewActor"));
	SpawnParams.ObjectFlags = RF_Transient;

	PreviewActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!PreviewActor)
	{
		return;
	}

	// Create skeletal mesh component (using DebugSkelMesh - Persona style fixed bone colors)
	SkeletalMeshComponent = NewObject<UDebugSkelMeshComponent>(PreviewActor, TEXT("SkeletalMeshComponent"));
	SkeletalMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SkeletalMeshComponent->bCastDynamicShadow = true;
	SkeletalMeshComponent->CastShadow = true;
	SkeletalMeshComponent->SetVisibility(true);
	SkeletalMeshComponent->SkeletonDrawMode = ESkeletonDrawMode::Default;  // Bone display and selection enabled
	SkeletalMeshComponent->RegisterComponent();
	PreviewActor->AddInstanceComponent(SkeletalMeshComponent);

	// Create FleshRing component (enable Deformer in editor as well)
	FleshRingComponent = NewObject<UFleshRingComponent>(PreviewActor, TEXT("FleshRingComponent"));
	FleshRingComponent->SetTargetMesh(SkeletalMeshComponent);
	FleshRingComponent->bEnableFleshRing = true;  // Enable Deformer in editor preview
	FleshRingComponent->RegisterComponent();
	PreviewActor->AddInstanceComponent(FleshRingComponent);
}

void FFleshRingPreviewScene::SetFleshRingAsset(UFleshRingAsset* InAsset)
{
	// Unbind delegate from existing asset
	UnbindFromAssetDelegate();

	CurrentAsset = InAsset;

	// ★ Check for nullptr and GC'd objects (may be invalid when called from Timer callback)
	if (!InAsset || !IsValid(InAsset))
	{
		return;
	}

	// Bind delegate to new asset
	BindToAssetDelegate();

	// ============================================
	// Step 1: First set to original mesh (for FleshRingComponent initialization)
	// ============================================
	// ★ Soft Reference validity check (prevent stale reference from old assets)
	USkeletalMesh* OriginalMesh = nullptr;
	if (!InAsset->TargetSkeletalMesh.IsNull())
	{
		OriginalMesh = InAsset->TargetSkeletalMesh.LoadSynchronous();
		// Additional validation after LoadSynchronous (prevent corrupt objects)
		if (OriginalMesh && !IsValid(OriginalMesh))
		{
			UE_LOG(LogTemp, Warning, TEXT("FleshRingPreviewScene: TargetSkeletalMesh reference is invalid (stale asset?)"));
			OriginalMesh = nullptr;
		}
	}

	// Check if mesh changed (based on TargetSkeletalMesh)
	const bool bOriginalMeshChanged = (CachedOriginalMesh.Get() != OriginalMesh);

	// Currently displayed mesh
	USkeletalMesh* CurrentDisplayedMesh = SkeletalMeshComponent ? SkeletalMeshComponent->GetSkeletalMeshAsset() : nullptr;

	// Determine mesh to display + whether Subdivision regeneration is needed
	USkeletalMesh* TargetDisplayMesh = OriginalMesh;
	bool bNeedsPreviewMeshGeneration = false;

#if WITH_EDITOR
	if (InAsset->SubdivisionSettings.bEnableSubdivision)
	{
		if (HasValidPreviewMesh() && !NeedsPreviewMeshRegeneration())
		{
			// Valid PreviewMesh exists - use it for display
			TargetDisplayMesh = PreviewSubdividedMesh;
		}
		else
		{
			// PreviewMesh regeneration needed - full refresh path required
			bNeedsPreviewMeshGeneration = true;
		}
	}
#endif

	// Whether display mesh needs to change
	const bool bDisplayMeshChanged = (CurrentDisplayedMesh != TargetDisplayMesh);

	// Condition: original same + display mesh same + no regeneration needed + DeformerInstance exists
	// Early return only when all conditions are met (only update Ring parameters)
	if (!bOriginalMeshChanged && !bDisplayMeshChanged && !bNeedsPreviewMeshGeneration &&
		OriginalMesh && SkeletalMeshComponent && SkeletalMeshComponent->GetMeshDeformerInstance())
	{
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Mesh unchanged, skipping full refresh (preserving DeformerInstance caches)"));

		// FleshRingComponent handles its own update via OnAssetChanged delegate
		// (Already processed in FleshRingComponent::OnFleshRingAssetChanged -> ApplyAsset -> RefreshWithDeformerReuse)

		// Update Ring meshes (only when FleshRingComponent is disabled)
		// If bEnableFleshRing=true, FleshRingComponent manages Ring Mesh, PreviewScene only cleans up
		if (!FleshRingComponent || !FleshRingComponent->bEnableFleshRing)
		{
			RefreshRings(InAsset->Rings);
		}
		else
		{
			// FleshRingComponent manages Ring Mesh, so clean up PreviewScene's Ring Mesh
			RefreshRings(TArray<FFleshRingSettings>());
		}
		return;
	}

	// ★ CL 320 restore: Destroy DeformerInstance only when original mesh changed
	// (Keep Deformer when toggling Subdivision - ApplyAsset runs first so Deformer is set before mesh swap)
	if (bOriginalMeshChanged && SkeletalMeshComponent)
	{
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Mesh changed, destroying DeformerInstance"));
		if (UMeshDeformerInstance* OldInstance = SkeletalMeshComponent->GetMeshDeformerInstance())
		{
			FlushRenderingCommands();
			OldInstance->MarkAsGarbage();
			OldInstance->ConditionalBeginDestroy();
		}
		// Also release Deformer so SetSkeletalMesh() doesn't create a new Instance
		SkeletalMeshComponent->SetMeshDeformer(nullptr);
	}

	// If TargetSkeletalMesh is null, clean up scene and return
	if (!OriginalMesh)
	{
		SetSkeletalMesh(nullptr);
		CachedOriginalMesh.Reset();  // Clear cache (prevent restoration)
		if (FleshRingComponent)
		{
			FleshRingComponent->FleshRingAsset = InAsset;
			FleshRingComponent->ApplyAsset();
		}
		RefreshRings(TArray<FFleshRingSettings>());  // Clean up Rings too
		return;
	}

	SetSkeletalMesh(OriginalMesh);

	// Cache original mesh (for restoration) - also update on mesh change
	if (CachedOriginalMesh.IsValid() && CachedOriginalMesh.Get() != OriginalMesh)
	{
		// Update cache if mesh changed
		CachedOriginalMesh = OriginalMesh;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Updated cached mesh to '%s' (mesh changed)"),
			OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
	}
	else if (!CachedOriginalMesh.IsValid() && OriginalMesh)
	{
		// Initial setup
		CachedOriginalMesh = OriginalMesh;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Cached original mesh '%s' for restoration"),
			*OriginalMesh->GetName());
	}

	// ============================================
	// Step 2: Initialize FleshRing component (before Subdivision processing!)
	// ★ CL 320 order restore: Call ApplyAsset() first so Deformer is set before mesh swap
	// ============================================
	if (FleshRingComponent)
	{
		FleshRingComponent->FleshRingAsset = InAsset;
		FleshRingComponent->ApplyAsset();

		// Apply Ring mesh visibility immediately after ApplyAsset() (prevent flickering)
		const auto& ComponentRingMeshes = FleshRingComponent->GetRingMeshComponents();
		for (UStaticMeshComponent* RingComp : ComponentRingMeshes)
		{
			if (RingComp)
			{
				RingComp->SetVisibility(bRingMeshesVisible);
			}
		}
	}

	// ============================================
	// Step 3: Subdivision processing (after ApplyAsset!)
	// ★ If mesh is swapped after Deformer is already set, Deformer is preserved
	// ============================================
#if WITH_EDITOR
	if (InAsset->SubdivisionSettings.bEnableSubdivision)
	{
		// Generate if preview mesh doesn't exist or needs regeneration
		if (!HasValidPreviewMesh() || NeedsPreviewMeshRegeneration())
		{
			GeneratePreviewMesh();
		}

		// Use preview mesh (if available)
		if (HasValidPreviewMesh())
		{
			SetSkeletalMesh(PreviewSubdividedMesh);

			// ★ Synchronize render resources (wait for IndexBuffer initialization)
			if (SkeletalMeshComponent)
			{
				SkeletalMeshComponent->MarkRenderStateDirty();
				FlushRenderingCommands();
			}

			// ★ Prevent GC: Check validity before logging (objects may be destroyed when called from Timer callback)
			if (IsValid(InAsset) && IsValid(PreviewSubdividedMesh))
			{
				FSkeletalMeshRenderData* RenderData = PreviewSubdividedMesh->GetResourceForRendering();
				UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Using PreviewSubdividedMesh (Level %d, %d vertices)"),
					InAsset->SubdivisionSettings.PreviewSubdivisionLevel,
					RenderData ? RenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() : 0);
			}
		}
	}
	else
	{
		// Remove preview mesh and restore original when Subdivision is disabled
		ClearPreviewMesh();

		// Restore to original mesh
		if (CachedOriginalMesh.IsValid() && SkeletalMeshComponent)
		{
			USkeletalMesh* CurrentMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
			USkeletalMesh* OrigMesh = CachedOriginalMesh.Get();
			if (CurrentMesh != OrigMesh)
			{
				SetSkeletalMesh(OrigMesh);
				UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Restored original mesh '%s' (subdivision disabled)"),
					OrigMesh ? *OrigMesh->GetName() : TEXT("null"));
			}
		}
	}
#endif

	// ============================================
	// Step 4: Schedule Deformer initialization
	// ★ CL 320 restore: Only set bPendingDeformerInit
	// ============================================
	if (FleshRingComponent && FleshRingComponent->bEnableFleshRing)
	{
		bPendingDeformerInit = true;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Deformer init pending (waiting for mesh to be rendered)"));
	}

	// Visualize Rings only when Deformer is disabled (FleshRingComponent manages when enabled)
	if (!FleshRingComponent || !FleshRingComponent->bEnableFleshRing)
	{
		RefreshRings(InAsset->Rings);
	}
	else
	{
		// FleshRingComponent manages Ring Mesh, so clean up PreviewScene's Ring Mesh
		RefreshRings(TArray<FFleshRingSettings>());
	}

	// ============================================
	// Step 5: Clean up unused PreviewMesh (CL 325 mesh cleanup code)
	// ★ Prevent memory leak: GC previous PreviewMesh when toggling Subdivision or Refresh
	// ============================================
	if (bDisplayMeshChanged || bNeedsPreviewMeshGeneration)
	{
		FlushRenderingCommands();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: GC executed to clean up unused PreviewMesh"));
	}
}

void FFleshRingPreviewScene::SetSkeletalMesh(USkeletalMesh* InMesh)
{
	if (SkeletalMeshComponent)
	{
		// Validate mesh (prevent Undo/Redo crash + verify render resource initialization)
		if (InMesh && !FleshRingUtils::IsSkeletalMeshValid(InMesh, /*bLogWarnings=*/ true))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("FleshRingPreviewScene::SetSkeletalMesh: Mesh '%s' is invalid, skipping"),
				*InMesh->GetName());
			return;
		}

		// ★ Disable Undo to prevent mesh swap from being captured in transaction
		// (If previous mesh is captured in TransBuffer, it cannot be GC'd)
		ITransaction* OldGUndo = GUndo;
		GUndo = nullptr;

		SkeletalMeshComponent->SetSkeletalMesh(InMesh);

		GUndo = OldGUndo;

		if (InMesh)
		{
			SkeletalMeshComponent->InitAnim(true);
			SkeletalMeshComponent->SetVisibility(true);
			SkeletalMeshComponent->UpdateBounds();
			SkeletalMeshComponent->MarkRenderStateDirty();
			FlushRenderingCommands();  // Sync render thread to prevent GC crash
		}
		else
		{
			// Hide component if mesh is nullptr
			SkeletalMeshComponent->SetVisibility(false);
		}
	}
}

void FFleshRingPreviewScene::RefreshPreview()
{
	if (CurrentAsset)
	{
		SetFleshRingAsset(CurrentAsset);
	}
}

void FFleshRingPreviewScene::RefreshRings(const TArray<FFleshRingSettings>& Rings)
{
	// Remove existing Ring components
	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RemoveComponent(RingComp);
		}
	}
	RingMeshComponents.Empty();

	// Create new Ring components
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		const FFleshRingSettings& RingSetting = Rings[i];

		UFleshRingMeshComponent* RingComp = NewObject<UFleshRingMeshComponent>(PreviewActor);
		RingComp->SetRingIndex(i);  // Set Ring index for use in HitProxy
		RingComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		RingComp->SetCollisionResponseToAllChannels(ECR_Ignore);
		RingComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		RingComp->bSelectable = true;

		// Set Ring mesh
		UStaticMesh* RingMesh = RingSetting.RingMesh.LoadSynchronous();
		if (RingMesh)
		{
			RingComp->SetStaticMesh(RingMesh);
		}

		// Place at bone position (apply MeshOffset, MeshRotation)
		if (SkeletalMeshComponent && SkeletalMeshComponent->GetSkeletalMeshAsset())
		{
			int32 BoneIndex = SkeletalMeshComponent->GetBoneIndex(RingSetting.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
				FQuat BoneRotation = BoneTransform.GetRotation();

				// Apply MeshOffset (in bone local coordinate system)
				FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(RingSetting.MeshOffset);

				// Bone rotation * Mesh rotation = World rotation (by default bone's X-axis aligns with mesh's Z-axis)
				FQuat MeshWorldRotation = BoneRotation * RingSetting.MeshRotation;

				RingComp->SetWorldLocationAndRotation(MeshLocation, MeshWorldRotation);
				RingComp->SetWorldScale3D(RingSetting.MeshScale);
			}
		}

		// Set visibility according to current Show Flag (set before AddComponent)
		RingComp->SetVisibility(bRingMeshesVisible);

		AddComponent(RingComp, RingComp->GetComponentTransform());
		RingMeshComponents.Add(RingComp);
	}
}

void FFleshRingPreviewScene::UpdateRingTransform(int32 Index, const FTransform& Transform)
{
	if (RingMeshComponents.IsValidIndex(Index) && RingMeshComponents[Index])
	{
		RingMeshComponents[Index]->SetWorldTransform(Transform);
	}
}

void FFleshRingPreviewScene::UpdateAllRingTransforms()
{
	if (!CurrentAsset || !SkeletalMeshComponent || !SkeletalMeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	const TArray<FFleshRingSettings>& Rings = CurrentAsset->Rings;

	for (int32 i = 0; i < Rings.Num() && i < RingMeshComponents.Num(); ++i)
	{
		UStaticMeshComponent* RingComp = RingMeshComponents[i];
		if (!RingComp)
		{
			continue;
		}

		const FFleshRingSettings& RingSetting = Rings[i];
		int32 BoneIndex = SkeletalMeshComponent->GetBoneIndex(RingSetting.BoneName);
		if (BoneIndex != INDEX_NONE)
		{
			FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
			FQuat BoneRotation = BoneTransform.GetRotation();

			// Apply MeshOffset (in bone local coordinate system)
			FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(RingSetting.MeshOffset);

			// Bone rotation * Mesh rotation = World rotation
			FQuat MeshWorldRotation = BoneRotation * RingSetting.MeshRotation;

			RingComp->SetWorldLocationAndRotation(MeshLocation, MeshWorldRotation);
			RingComp->SetWorldScale3D(RingSetting.MeshScale);
		}
	}
}

void FFleshRingPreviewScene::SetSelectedRingIndex(int32 Index)
{
	SelectedRingIndex = Index;
}

void FFleshRingPreviewScene::SetRingMeshesVisible(bool bVisible)
{
	bRingMeshesVisible = bVisible;

	// Also sync FleshRingComponent's bShowRingMesh (to apply during SetupRingMeshes)
	if (FleshRingComponent)
	{
		FleshRingComponent->bShowRingMesh = bVisible;
	}

	// 1. PreviewScene's RingMeshComponents (when Deformer is disabled)
	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RingComp->SetVisibility(bVisible);
		}
	}

	// 2. FleshRingComponent's RingMeshComponents (when Deformer is enabled)
	if (FleshRingComponent)
	{
		const auto& ComponentRingMeshes = FleshRingComponent->GetRingMeshComponents();
		for (UStaticMeshComponent* RingComp : ComponentRingMeshes)
		{
			if (RingComp)
			{
				RingComp->SetVisibility(bVisible);
			}
		}
	}
}

void FFleshRingPreviewScene::BindToAssetDelegate()
{
	if (CurrentAsset)
	{
		if (!AssetChangedDelegateHandle.IsValid())
		{
			AssetChangedDelegateHandle = CurrentAsset->OnAssetChanged.AddRaw(
				this, &FFleshRingPreviewScene::OnAssetChanged);
		}
	}
}

void FFleshRingPreviewScene::UnbindFromAssetDelegate()
{
	if (CurrentAsset)
	{
		if (AssetChangedDelegateHandle.IsValid())
		{
			CurrentAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
			AssetChangedDelegateHandle.Reset();
		}
	}
}

void FFleshRingPreviewScene::OnAssetChanged(UFleshRingAsset* ChangedAsset)
{
	// Verify it's the same asset
	if (ChangedAsset == CurrentAsset)
	{
		// Safely update on next tick after transaction completes
		// (May be inside transaction when called from PostEditChangeProperty - prevent Undo crash during mesh creation)
		if (GEditor)
		{
			TWeakObjectPtr<UFleshRingAsset> WeakAsset = ChangedAsset;
			FFleshRingPreviewScene* Scene = this;

			GEditor->GetTimerManager()->SetTimerForNextTick(
				[Scene, WeakAsset]()
				{
					if (WeakAsset.IsValid() && Scene && Scene->CurrentAsset == WeakAsset.Get())
					{
						UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Asset changed, refreshing preview (deferred)..."));
						Scene->RefreshPreview();
					}
				});
		}
	}
}

bool FFleshRingPreviewScene::IsPendingDeformerInit() const
{
	if (!bPendingDeformerInit)
	{
		return false;
	}

	// Check if skeletal mesh has been rendered
	// WasRecentlyRendered() checks last render time to return whether recently rendered
	if (SkeletalMeshComponent && SkeletalMeshComponent->WasRecentlyRendered(0.1f))
	{
		return true;
	}

	return false;
}

void FFleshRingPreviewScene::ExecutePendingDeformerInit()
{
	if (!bPendingDeformerInit)
	{
		return;
	}

	bPendingDeformerInit = false;

	if (!FleshRingComponent || !FleshRingComponent->bEnableFleshRing)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Mesh rendered, executing deferred Deformer init"));

	// Initialize Deformer
	FleshRingComponent->InitializeForEditorPreview();

	// Apply Show Flag to Ring meshes created by FleshRingComponent
	const auto& RingMeshes = FleshRingComponent->GetRingMeshComponents();
	for (UStaticMeshComponent* RingComp : RingMeshes)
	{
		if (RingComp)
		{
			RingComp->SetVisibility(bRingMeshesVisible);
		}
	}

	// Reapply PreviewMesh (InitializeForEditorPreview may have overwritten the mesh)
	if (CurrentAsset)
	{
		bool bUsePreviewMesh = CurrentAsset->SubdivisionSettings.bEnableSubdivision
			&& HasValidPreviewMesh();
		if (bUsePreviewMesh && SkeletalMeshComponent)
		{
			// ★ Disable Undo to prevent mesh swap from being captured in transaction
			ITransaction* OldGUndo = GUndo;
			GUndo = nullptr;

			SkeletalMeshComponent->SetSkeletalMesh(PreviewSubdividedMesh);

			GUndo = OldGUndo;

			SkeletalMeshComponent->MarkRenderStateDirty();
			FlushRenderingCommands();
		}
	}
}

// =====================================
// Preview Mesh Management (separated from asset to exclude from transaction)
// =====================================

void FFleshRingPreviewScene::ClearPreviewMesh()
{
	if (PreviewSubdividedMesh)
	{
		USkeletalMesh* OldMesh = PreviewSubdividedMesh;

		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene::ClearPreviewMesh: Destroying '%s'"),
			*OldMesh->GetName());

		// 1. Release pointer
		PreviewSubdividedMesh = nullptr;

		// 2. Fully release render resources
		OldMesh->ReleaseResources();
		OldMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// 3. Change Outer to TransientPackage
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

		// 4. Set flags
		OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		OldMesh->SetFlags(RF_Transient);

		// 5. Mark for GC
		OldMesh->MarkAsGarbage();

		// Invalidate cache
		bPreviewMeshCacheValid = false;
		LastPreviewBoneConfigHash = 0;
	}
}

void FFleshRingPreviewScene::InvalidatePreviewMeshCache()
{
	bPreviewMeshCacheValid = false;
	LastPreviewBoneConfigHash = MAX_uint32;
}

bool FFleshRingPreviewScene::IsPreviewMeshCacheValid() const
{
	if (!HasValidPreviewMesh())
	{
		return false;
	}

	// Compare hash
	return LastPreviewBoneConfigHash == CalculatePreviewBoneConfigHash();
}

bool FFleshRingPreviewScene::NeedsPreviewMeshRegeneration() const
{
	if (!CurrentAsset || !CurrentAsset->SubdivisionSettings.bEnableSubdivision)
	{
		return false;
	}

	// Need regeneration if mesh doesn't exist
	if (PreviewSubdividedMesh == nullptr)
	{
		return true;
	}

	// Need regeneration if cache is invalidated
	if (!IsPreviewMeshCacheValid())
	{
		return true;
	}

	return false;
}

uint32 FFleshRingPreviewScene::CalculatePreviewBoneConfigHash() const
{
	if (!CurrentAsset)
	{
		return 0;
	}

	uint32 Hash = 0;

	// TargetSkeletalMesh pointer hash (invalidate cache when mesh changes)
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->TargetSkeletalMesh.Get()));

	// Ring attachment bone list hash
	for (const FFleshRingSettings& Ring : CurrentAsset->Rings)
	{
		Hash = HashCombine(Hash, GetTypeHash(Ring.BoneName));
	}

	// Subdivision parameters hash
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->SubdivisionSettings.PreviewSubdivisionLevel));
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->SubdivisionSettings.PreviewBoneHopCount));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(CurrentAsset->SubdivisionSettings.PreviewBoneWeightThreshold * 255)));
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->SubdivisionSettings.MinEdgeLength));

	return Hash;
}

void FFleshRingPreviewScene::GeneratePreviewMesh()
{
	if (!CurrentAsset)
	{
		return;
	}

	// Cache check - no regeneration needed if already valid
	if (IsPreviewMeshCacheValid())
	{
		return;
	}

	// ★ Exclude entire mesh creation/removal process from Undo system
	// If previous mesh cleanup and new mesh creation are captured in transaction, GC is impossible
	ITransaction* OldGUndo = GUndo;
	GUndo = nullptr;

	// Remove existing PreviewMesh first if present
	if (PreviewSubdividedMesh)
	{
		ClearPreviewMesh();
	}

	if (!CurrentAsset->SubdivisionSettings.bEnableSubdivision)
	{
		GUndo = OldGUndo;
		return;
	}

	if (CurrentAsset->TargetSkeletalMesh.IsNull())
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: TargetSkeletalMesh is not set"));
		GUndo = OldGUndo;
		return;
	}

	USkeletalMesh* SourceMesh = CurrentAsset->TargetSkeletalMesh.LoadSynchronous();
	if (!SourceMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: Failed to load SourceMesh"));
		GUndo = OldGUndo;
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// 1. Get source mesh render data
	FSkeletalMeshRenderData* RenderData = SourceMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: No RenderData"));
		GUndo = OldGUndo;
		return;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = RenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// 2. Extract source vertex data
	TArray<FVector> SourcePositions;
	TArray<FVector> SourceNormals;
	TArray<FVector4> SourceTangents;
	TArray<FVector2D> SourceUVs;

	SourcePositions.SetNum(SourceVertexCount);
	SourceNormals.SetNum(SourceVertexCount);
	SourceTangents.SetNum(SourceVertexCount);
	SourceUVs.SetNum(SourceVertexCount);

	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		SourcePositions[i] = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
		SourceNormals[i] = FVector(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		FVector4f TangentX = SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
		SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		SourceUVs[i] = FVector2D(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
	}

	// Extract indices
	TArray<uint32> SourceIndices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = SourceLODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		SourceIndices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			SourceIndices[i] = IndexBuffer->Get(i);
		}
	}

	// Extract material index per section
	TArray<int32> SourceTriangleMaterialIndices;
	{
		const int32 NumTriangles = SourceIndices.Num() / 3;
		SourceTriangleMaterialIndices.SetNum(NumTriangles);
		for (const FSkelMeshRenderSection& Section : SourceLODData.RenderSections)
		{
			const int32 StartTriangle = Section.BaseIndex / 3;
			const int32 EndTriangle = StartTriangle + Section.NumTriangles;
			for (int32 TriIdx = StartTriangle; TriIdx < EndTriangle && TriIdx < NumTriangles; ++TriIdx)
			{
				SourceTriangleMaterialIndices[TriIdx] = Section.MaterialIndex;
			}
		}
	}

	// Extract bone weights
	const int32 MaxBoneInfluences = SourceLODData.GetVertexBufferMaxBoneInfluences();
	TArray<TArray<uint16>> SourceBoneIndices;
	TArray<TArray<uint8>> SourceBoneWeights;
	SourceBoneIndices.SetNum(SourceVertexCount);
	SourceBoneWeights.SetNum(SourceVertexCount);

	TArray<FVertexBoneInfluence> VertexBoneInfluences;
	VertexBoneInfluences.SetNum(SourceVertexCount);

	// Create vertex-to-section index map
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(SourceVertexCount);
	for (int32& SectionIdx : VertexToSectionIndex) { SectionIdx = INDEX_NONE; }
	for (int32 SectionIdx = 0; SectionIdx < SourceLODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = SourceLODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;
		for (int32 IdxPos = StartIndex; IdxPos < EndIndex; ++IdxPos)
		{
			uint32 VertexIdx = SourceIndices[IdxPos];
			if (VertexIdx < SourceVertexCount && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
			{
				VertexToSectionIndex[VertexIdx] = SectionIdx;
			}
		}
	}

	const FSkinWeightVertexBuffer* SkinWeightBuffer = SourceLODData.GetSkinWeightVertexBuffer();
	if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
	{
		const int32 ClampedInfluences = FMath::Min(MaxBoneInfluences, FVertexBoneInfluence::MAX_INFLUENCES);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			SourceBoneIndices[i].SetNum(MaxBoneInfluences);
			SourceBoneWeights[i].SetNum(MaxBoneInfluences);

			FVertexBoneInfluence& Influence = VertexBoneInfluences[i];
			FMemory::Memzero(Influence.BoneIndices, sizeof(Influence.BoneIndices));
			FMemory::Memzero(Influence.BoneWeights, sizeof(Influence.BoneWeights));

			int32 SectionIdx = VertexToSectionIndex[i];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < SourceLODData.RenderSections.Num())
			{
				BoneMap = &SourceLODData.RenderSections[SectionIdx].BoneMap;
			}
			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);
				uint16 GlobalBoneIdx = LocalBoneIdx;
				if (BoneMap && LocalBoneIdx < BoneMap->Num())
				{
					GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
				}
				SourceBoneIndices[i][j] = GlobalBoneIdx;
				SourceBoneWeights[i][j] = Weight;

				if (j < ClampedInfluences)
				{
					Influence.BoneIndices[j] = GlobalBoneIdx;
					Influence.BoneWeights[j] = Weight;
				}
			}
		}
	}

	// 3. Execute bone-based Subdivision processor
	FFleshRingSubdivisionProcessor Processor;

	if (!Processor.SetSourceMesh(SourcePositions, SourceIndices, SourceUVs, SourceTriangleMaterialIndices))
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: SetSourceMesh failed"));
		GUndo = OldGUndo;
		return;
	}
	Processor.SetVertexBoneInfluences(VertexBoneInfluences);

	FSubdivisionProcessorSettings Settings;
	Settings.MinEdgeLength = CurrentAsset->SubdivisionSettings.MinEdgeLength;
	Processor.SetSettings(Settings);

	FSubdivisionTopologyResult TopologyResult;

	// ★ Skip subdivision if no Rings (matches runtime behavior)
	if (CurrentAsset->Rings.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: Skipping Subdivision because there are no Rings"));
		GUndo = OldGUndo;
		return;
	}

	if (!Processor.HasBoneInfo())
	{
		// Ring exists + No BoneInfo -> Skip (abnormal situation)
		UE_LOG(LogTemp, Error,
			TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: Skipping Subdivision because there is no BoneInfo. ")
			TEXT("SkeletalMesh '%s' has no SkinWeightBuffer or bone weight extraction failed."),
			SourceMesh ? *SourceMesh->GetName() : TEXT("null"));
		GUndo = OldGUndo;
		return;
	}

	// Collect Ring attachment bone indices
	const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
	TArray<int32> RingBoneIndices;
	for (const FFleshRingSettings& Ring : CurrentAsset->Rings)
	{
		int32 BoneIdx = RefSkeleton.FindBoneIndex(Ring.BoneName);
		if (BoneIdx != INDEX_NONE)
		{
			RingBoneIndices.Add(BoneIdx);
		}
	}

	// ★ Skip if no Rings have valid BoneName
	if (RingBoneIndices.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: Skipping Subdivision because no Rings have valid BoneName. ")
			TEXT("Please set BoneName on the Ring."));
		GUndo = OldGUndo;
		return;
	}

	TSet<int32> TargetBones = FFleshRingSubdivisionProcessor::GatherNeighborBones(
		RefSkeleton, RingBoneIndices, CurrentAsset->SubdivisionSettings.PreviewBoneHopCount);

	FBoneRegionSubdivisionParams BoneParams;
	BoneParams.TargetBoneIndices = TargetBones;
	BoneParams.BoneWeightThreshold = static_cast<uint8>(CurrentAsset->SubdivisionSettings.PreviewBoneWeightThreshold * 255);
	BoneParams.NeighborHopCount = CurrentAsset->SubdivisionSettings.PreviewBoneHopCount;
	BoneParams.MaxSubdivisionLevel = CurrentAsset->SubdivisionSettings.PreviewSubdivisionLevel;

	if (!Processor.ProcessBoneRegion(TopologyResult, BoneParams))
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: ProcessBoneRegion failed"));
		GUndo = OldGUndo;
		return;
	}

	// 4. Interpolate new vertex data
	const int32 NewVertexCount = TopologyResult.VertexData.Num();
	TArray<FVector> NewPositions;
	TArray<FVector> NewNormals;
	TArray<FVector4> NewTangents;
	TArray<FVector2D> NewUVs;
	TArray<TArray<uint16>> NewBoneIndices;
	TArray<TArray<uint8>> NewBoneWeights;

	NewPositions.SetNum(NewVertexCount);
	NewNormals.SetNum(NewVertexCount);
	NewTangents.SetNum(NewVertexCount);
	NewUVs.SetNum(NewVertexCount);
	NewBoneIndices.SetNum(NewVertexCount);
	NewBoneWeights.SetNum(NewVertexCount);

	TMap<uint16, float> BoneWeightMap;
	TArray<TPair<uint16, float>> SortedWeights;

	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FSubdivisionVertexData& VD = TopologyResult.VertexData[i];
		const float U = VD.BarycentricCoords.X;
		const float V = VD.BarycentricCoords.Y;
		const float W = VD.BarycentricCoords.Z;

		const uint32 P0 = FMath::Min(VD.ParentV0, (uint32)(SourceVertexCount - 1));
		const uint32 P1 = FMath::Min(VD.ParentV1, (uint32)(SourceVertexCount - 1));
		const uint32 P2 = FMath::Min(VD.ParentV2, (uint32)(SourceVertexCount - 1));

		NewPositions[i] = SourcePositions[P0] * U + SourcePositions[P1] * V + SourcePositions[P2] * W;
		// Normal interpolation
		FVector InterpolatedNormal = SourceNormals[P0] * U + SourceNormals[P1] * V + SourceNormals[P2] * W;
		NewNormals[i] = InterpolatedNormal.GetSafeNormal();
		FVector4 InterpTangent = SourceTangents[P0] * U + SourceTangents[P1] * V + SourceTangents[P2] * W;
		FVector TangentDir = FVector(InterpTangent.X, InterpTangent.Y, InterpTangent.Z).GetSafeNormal();
		NewTangents[i] = FVector4(TangentDir.X, TangentDir.Y, TangentDir.Z, SourceTangents[P0].W);
		NewUVs[i] = SourceUVs[P0] * U + SourceUVs[P1] * V + SourceUVs[P2] * W;

		NewBoneIndices[i].SetNum(MaxBoneInfluences);
		NewBoneWeights[i].SetNum(MaxBoneInfluences);

		BoneWeightMap.Reset();
		SortedWeights.Reset();

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (SourceBoneWeights[P0][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P0][j]) += (SourceBoneWeights[P0][j] / 255.0f) * U;
			if (SourceBoneWeights[P1][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P1][j]) += (SourceBoneWeights[P1][j] / 255.0f) * V;
			if (SourceBoneWeights[P2][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P2][j]) += (SourceBoneWeights[P2][j] / 255.0f) * W;
		}
		for (const auto& Pair : BoneWeightMap) { SortedWeights.Add(TPair<uint16, float>(Pair.Key, Pair.Value)); }
		SortedWeights.Sort([](const TPair<uint16, float>& A, const TPair<uint16, float>& B) { return A.Value > B.Value; });
		float TotalWeight = 0.0f;
		for (int32 j = 0; j < FMath::Min(SortedWeights.Num(), MaxBoneInfluences); ++j) { TotalWeight += SortedWeights[j].Value; }
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (j < SortedWeights.Num() && TotalWeight > 0.0f)
			{
				NewBoneIndices[i][j] = SortedWeights[j].Key;
				NewBoneWeights[i][j] = FMath::Clamp<uint8>(FMath::RoundToInt((SortedWeights[j].Value / TotalWeight) * 255.0f), 0, 255);
			}
			else
			{
				NewBoneIndices[i][j] = 0;
				NewBoneWeights[i][j] = 0;
			}
		}
	}

	// 5. Create USkeletalMesh for preview
	// ★ Set Outer to GetTransientPackage() - eligible for GC when PreviewScene is destroyed
	FString MeshName = FString::Printf(TEXT("%s_Preview_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	PreviewSubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, GetTransientPackage(), FName(*MeshName));

	if (!PreviewSubdividedMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: Mesh duplication failed"));
		GUndo = OldGUndo;
		return;
	}

	// Set flags - completely exclude from transaction
	PreviewSubdividedMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
	PreviewSubdividedMesh->SetFlags(RF_Transient);

	FlushRenderingCommands();
	PreviewSubdividedMesh->ReleaseResources();
	PreviewSubdividedMesh->ReleaseResourcesFence.Wait();

	if (PreviewSubdividedMesh->HasMeshDescription(0))
	{
		PreviewSubdividedMesh->ClearMeshDescription(0);
	}

	// 6. Create MeshDescription
	FMeshDescription MeshDescription;
	FSkeletalMeshAttributes MeshAttributes(MeshDescription);
	MeshAttributes.Register();

	MeshDescription.ReserveNewVertices(NewVertexCount);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FVertexID VertexID = MeshDescription.CreateVertex();
		MeshDescription.GetVertexPositions()[VertexID] = FVector3f(NewPositions[i]);
	}

	MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
	const int32 NumMaterials = SourceMesh ? SourceMesh->GetMaterials().Num() : 1;
	const int32 NumFaces = TopologyResult.Indices.Num() / 3;

	TSet<int32> UsedMaterialIndices;
	for (int32 TriIdx = 0; TriIdx < NumFaces; ++TriIdx)
	{
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(TriIdx) ? TopologyResult.TriangleMaterialIndices[TriIdx] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		UsedMaterialIndices.Add(MatIdx);
	}

	TMap<int32, FPolygonGroupID> MaterialIndexToPolygonGroup;
	TArray<int32> SortedMaterialIndices = UsedMaterialIndices.Array();
	SortedMaterialIndices.Sort();
	for (int32 MatIdx : SortedMaterialIndices)
	{
		FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		MaterialIndexToPolygonGroup.Add(MatIdx, GroupID);
		FName MaterialSlotName = NAME_None;
		if (SourceMesh && SourceMesh->GetMaterials().IsValidIndex(MatIdx))
		{
			MaterialSlotName = SourceMesh->GetMaterials()[MatIdx].ImportedMaterialSlotName;
		}
		if (MaterialSlotName.IsNone()) { MaterialSlotName = *FString::Printf(TEXT("Material_%d"), MatIdx); }
		MeshDescription.PolygonGroupAttributes().SetAttribute(GroupID, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 0, MaterialSlotName);
	}

	TArray<FVertexInstanceID> VertexInstanceIDs;
	VertexInstanceIDs.Reserve(TopologyResult.Indices.Num());
	for (int32 i = 0; i < TopologyResult.Indices.Num(); ++i)
	{
		const uint32 VertexIndex = TopologyResult.Indices[i];
		const FVertexID VertexID(VertexIndex);
		const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
		VertexInstanceIDs.Add(VertexInstanceID);
		MeshAttributes.GetVertexInstanceUVs().Set(VertexInstanceID, 0, FVector2f(NewUVs[VertexIndex]));
		MeshAttributes.GetVertexInstanceNormals().Set(VertexInstanceID, FVector3f(NewNormals[VertexIndex]));
		MeshAttributes.GetVertexInstanceTangents().Set(VertexInstanceID, FVector3f(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z));
		MeshAttributes.GetVertexInstanceBinormalSigns().Set(VertexInstanceID, NewTangents[VertexIndex].W);
	}

	for (int32 i = 0; i < NumFaces; ++i)
	{
		TArray<FVertexInstanceID> TriangleVertexInstances;
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 0]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 1]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 2]);
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(i) ? TopologyResult.TriangleMaterialIndices[i] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		FPolygonGroupID* GroupID = MaterialIndexToPolygonGroup.Find(MatIdx);
		if (GroupID) { MeshDescription.CreatePolygon(*GroupID, TriangleVertexInstances); }
	}

	FSkinWeightsVertexAttributesRef SkinWeights = MeshAttributes.GetVertexSkinWeights();
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		FVertexID VertexID(i);
		TArray<UE::AnimationCore::FBoneWeight> BoneWeightArray;
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (NewBoneWeights[i][j] > 0)
			{
				UE::AnimationCore::FBoneWeight BW;
				BW.SetBoneIndex(NewBoneIndices[i][j]);
				BW.SetWeight(NewBoneWeights[i][j] / 255.0f);
				BoneWeightArray.Add(BW);
			}
		}
		SkinWeights.Set(VertexID, BoneWeightArray);
	}

	PreviewSubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	PreviewSubdividedMesh->CommitMeshDescription(0, CommitParams);

	// ★ Key: Disable Normal/Tangent recomputation before Build()
	// DuplicateObject copies the source mesh's BuildSettings,
	// if bRecomputeNormals=true is set on the source, our set Normals will be ignored
	if (FSkeletalMeshLODInfo* LODInfo = PreviewSubdividedMesh->GetLODInfo(0))
	{
		LODInfo->BuildSettings.bRecomputeNormals = false;
		LODInfo->BuildSettings.bRecomputeTangents = false;
	}

	PreviewSubdividedMesh->Build();
	PreviewSubdividedMesh->InitResources();

	FlushRenderingCommands();

	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i) { BoundingBox += NewPositions[i]; }
	PreviewSubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	PreviewSubdividedMesh->CalculateExtendedBounds();

	// Update cache hash
	LastPreviewBoneConfigHash = CalculatePreviewBoneConfigHash();
	bPreviewMeshCacheValid = true;

	const double EndTime = FPlatformTime::Seconds();
	const double ElapsedMs = (EndTime - StartTime) * 1000.0;

	UE_LOG(LogTemp, Log, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh completed: %d vertices, %d triangles (%.2fms, CacheHash=%u)"),
		NewVertexCount, TopologyResult.SubdividedTriangleCount, ElapsedMs, LastPreviewBoneConfigHash);

	// ★ Restore Undo system
	GUndo = OldGUndo;
}


// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.generated.h"

class UStaticMesh;
class UVolumeTexture;

// =====================================
// ´ê±°•ì˜
// =====================================

/** Ring í–¥ ë²”ìœ„ ê²°ì • ë°©ì‹ */
UENUM(BlueprintType)
enum class EFleshRingInfluenceMode : uint8
{
	/** SDF ê¸°ë°˜ ë™ ê³„ì‚° */
	Auto	UMETA(DisplayName = "Auto (SDF-based)"),

	/** ˜ë™ Radius ì§€*/
	Manual	UMETA(DisplayName = "Manual")
};

/** SDF …ë°´íŠ¸ ëª¨ë“œ */
UENUM(BlueprintType)
enum class EFleshRingSdfUpdateMode : uint8
{
	/** ë§±ë§ˆ…ë°´íŠ¸ */
	OnTick		UMETA(DisplayName = "On Tick"),

	/** ê°ë³€ê²œì—ë§…ë°´íŠ¸ */
	OnChange	UMETA(DisplayName = "On Change"),

	/** ˜ë™ …ë°´íŠ¸ */
	Manual		UMETA(DisplayName = "Manual")
};

// =====================================
// êµ¬ì¡°ì²•ì˜
// =====================================

/** ê°œë³„ Ring ¤ì • */
USTRUCT(BlueprintType)
struct FFleshRingSettings
{
	GENERATED_BODY()

	/** €ê²ë³´ë¦„ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	FName BoneName;

	/** Ring ë©”ì‰¬ (œê°œí˜„ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	TSoftObjectPtr<UStaticMesh> RingMesh;

	/** í–¥ ë²”ìœ„ ê²°ì • ë°©ì‹ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	EFleshRingInfluenceMode InfluenceMode = EFleshRingInfluenceMode::Auto;

	/** Ring ë°˜ìë¦(Manual ëª¨ë“œì„œë§¬ìš©) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual", ClampMin = "0.1", ClampMax = "100.0"))
	float RingRadius = 5.0f;

	/** Ring ê»˜ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.1", ClampMax = "50.0"))
	float RingWidth = 2.0f;

	/** ê°ì‡  ê³¡ì„  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float Falloff = 1.0f;

	/** ë³¼ë¡ ¨ê³¼ ê°•ë„ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float BulgeIntensity = 0.5f;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::Auto)
		, RingRadius(5.0f)
		, RingWidth(2.0f)
		, Falloff(1.0f)
		, BulgeIntensity(0.5f)
	{
	}
};

/** SDF ê´€¤ì • */
USTRUCT(BlueprintType)
struct FFleshRingSdfSettings
{
	GENERATED_BODY()

	/** SDF ë³¼ë¥¨ ´ìƒ*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF", meta = (ClampMin = "16", ClampMax = "128"))
	int32 Resolution = 64;

	/** JFA ë°˜ë³µ Ÿìˆ˜ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF", meta = (ClampMin = "1", ClampMax = "16"))
	int32 JfaIterations = 8;

	/** …ë°´íŠ¸ ëª¨ë“œ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF")
	EFleshRingSdfUpdateMode UpdateMode = EFleshRingSdfUpdateMode::OnTick;
};

// =====================================
// ì»´í¬ŒíŠ¸ ´ë˜// =====================================

/**
 * FleshRing ë©”ì‰¬ ë³€ì»´í¬ŒíŠ¸
 * SDF ê¸°ë°˜¼ë¡œ ¤ì¼ˆˆíƒˆ ë©”ì‰¬Flesh) œí˜„ì²˜ë¦¬
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent), DisplayName="Flesh Ring")
class FLESHRINGRUNTIME_API UFleshRingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFleshRingComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =====================================
	// SDF Source
	// =====================================

	/** SDF ì„±¬ìš©StaticMesh */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF Source")
	TObjectPtr<UStaticMesh> SDFSourceMesh;

	// =====================================
	// Target Settings
	// =====================================

	/** ˜ë™¼ë¡œ €ê²SkeletalMeshComponent ì§€(falseë©Ownerì„œ ë™ ìƒ‰) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target Settings", AdvancedDisplay)
	bool bUseCustomTarget = false;

	/** ˜ë™ ì§€€ê²(bUseCustomTargettrueŒë§Œ ¬ìš©) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target Settings", AdvancedDisplay, meta = (EditCondition = "bUseCustomTarget"))
	TObjectPtr<USkeletalMeshComponent> CustomTargetMesh;

	// =====================================
	// General
	// =====================================

	/** „ì²´ ê¸°ëŠ¥ œì„±*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bEnableFleshRing = true;

	/** Bounds •ì¥ ë°°ìœ¨ (VSM ìºì‹± œìŠ¤œì˜ •ìƒ ‘ë™„í•´ Deformer ë³€•ëŸ‰ë§ê²Œ ì¡°ì •) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float BoundsScale = 2.0f;

	// =====================================
	// Ring Settings
	// =====================================

	/** Ring ¤ì • ë°°ì—´ */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Settings")
	TArray<FFleshRingSettings> Rings;

	// =====================================
	// SDF Settings
	// =====================================

	/** SDF ¤ì • */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF Settings")
	FFleshRingSdfSettings SdfSettings;

	// =====================================
	// Debug / Visualization (ë””„ìš©)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** SDF ë³¼ë¥¨ œì‹œ */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowSdfVolume = false;

	/** í–¥ë°›ëŠ” ë²„í…œì‹œ */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowAffectedVertices = false;

	/** Ring ê¸°ì¦ˆëªœì‹œ */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowRingGizmos = true;

	/** Bulge ˆíŠ¸ë§œì‹œ */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowBulgeHeatmap = false;
#endif

	// =====================================
	// Blueprint Callable Functions
	// =====================================

	/** SDF ˜ë™ …ë°´íŠ¸ */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void UpdateSDF();

	/** ¤ì œ ìš©€ê²SkeletalMeshComponent ë°˜í™˜ */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	USkeletalMeshComponent* GetResolvedTargetMesh() const { return ResolvedTargetMesh.Get(); }

	/** ´ë Deformer ë°˜í™˜ */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	UFleshRingDeformer* GetDeformer() const { return InternalDeformer; }

private:
	/** ë™/˜ë™ ìƒ‰¤ì œ €ê²*/
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> ResolvedTargetMesh;

	/** °í„ì— ì„±´ë Deformer */
	UPROPERTY(Transient)
	TObjectPtr<UFleshRingDeformer> InternalDeformer;

	/** SDF 3D ë³¼ë¥¨ ìŠ¤ì²*/
	UPROPERTY(Transient)
	TObjectPtr<UVolumeTexture> SDFVolumeTexture;

	/** €ê²SkeletalMeshComponent ìƒ‰ ë°¤ì • */
	void ResolveTargetMesh();

	/** Deformer ì„± ë°±ë¡ */
	void SetupDeformer();

	/** Deformer ´ì œ */
	void CleanupDeformer();

	/** SDF ì„± (SDFSourceMesh ê¸°ë°˜) */
	void GenerateSDF();
};

// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformer.h"
#include "FleshRingDeformer.generated.h"

class UFleshRingDeformerInstance;
class UFleshRingComponent;

UCLASS(Blueprintable, BlueprintType, Meta = (DisplayName = "Flesh Ring Deformer"))
class FLESHRINGRUNTIME_API UFleshRingDeformer : public UMeshDeformer
{
	GENERATED_BODY()

public:
	UFleshRingDeformer();

	/** 현재 활성화된 DeformerInstance 반환 (없으면 nullptr) */
	UFleshRingDeformerInstance* GetActiveInstance() const { return ActiveInstance.Get(); }

	// UMeshDeformer interface
	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(UMeshComponent* InMeshComponent) override;
	virtual UMeshDeformerInstance* CreateInstance(UMeshComponent* InMeshComponent, UMeshDeformerInstanceSettings* InSettings) override;

private:
	/** 생성된 DeformerInstance 캐싱 (GetActiveInstance()로 접근) */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFleshRingDeformerInstance> ActiveInstance;

	/** 이 Deformer를 생성/관리하는 FleshRingComponent (다중 컴포넌트 환경 지원) */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFleshRingComponent> OwnerFleshRingComponent;

public:
	/** Owner FleshRingComponent 설정 (SetupDeformer에서 호출) */
	void SetOwnerFleshRingComponent(UFleshRingComponent* InComponent);

	/** Owner FleshRingComponent 반환 */
	UFleshRingComponent* GetOwnerFleshRingComponent() const;
};

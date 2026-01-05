// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingMeshComponent.h"

#if WITH_EDITOR
#include "FleshRingMeshHitProxy.h"
#include "StaticMeshSceneProxy.h"

// HitProxy 구현 매크로
IMPLEMENT_HIT_PROXY(HFleshRingMeshHitProxy, HHitProxy);

/**
 * Ring 메시용 커스텀 SceneProxy
 * CreateHitProxies를 오버라이드하여 본보다 높은 우선순위의 HitProxy 제공
 */
class FFleshRingMeshSceneProxy : public FStaticMeshSceneProxy
{
public:
	FFleshRingMeshSceneProxy(UStaticMeshComponent* Component, bool bForceLODsShareStaticLighting, int32 InRingIndex)
		: FStaticMeshSceneProxy(Component, bForceLODsShareStaticLighting)
		, RingIndex(InRingIndex)
	{
	}

	virtual HHitProxy* CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies) override
	{
		// 커스텀 HitProxy 생성 - HPP_Foreground로 본(HPP_World)보다 높은 우선순위
		if (RingIndex != INDEX_NONE)
		{
			HFleshRingMeshHitProxy* HitProxy = new HFleshRingMeshHitProxy(RingIndex);
			OutHitProxies.Add(HitProxy);
			return HitProxy;
		}

		// RingIndex가 없으면 기본 동작
		return FStaticMeshSceneProxy::CreateHitProxies(Component, OutHitProxies);
	}

private:
	int32 RingIndex;
};
#endif // WITH_EDITOR

UFleshRingMeshComponent::UFleshRingMeshComponent()
{
	// 기본 설정
	CastShadow = false;
	bCastDynamicShadow = false;
}

FPrimitiveSceneProxy* UFleshRingMeshComponent::CreateSceneProxy()
{
#if WITH_EDITOR
	// 에디터에서는 커스텀 프록시 생성 (높은 피킹 우선순위)
	if (GetStaticMesh() == nullptr || GetStaticMesh()->GetRenderData() == nullptr)
	{
		return nullptr;
	}

	if (GetStaticMesh()->GetRenderData()->LODResources.Num() == 0)
	{
		return nullptr;
	}

	return new FFleshRingMeshSceneProxy(this, false, RingIndex);
#else
	// 런타임에서는 기본 프록시 사용
	return Super::CreateSceneProxy();
#endif
}

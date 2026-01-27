// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingMeshComponent.h"

#if WITH_EDITOR
#include "FleshRingMeshHitProxy.h"
#include "StaticMeshSceneProxy.h"

// HitProxy implementation macro
IMPLEMENT_HIT_PROXY(HFleshRingMeshHitProxy, HHitProxy);

/**
 * Custom SceneProxy for Ring mesh
 * Overrides CreateHitProxies to provide HitProxy with higher priority than bones
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
		// Create custom HitProxy - HPP_Foreground has higher priority than bones (HPP_World)
		if (RingIndex != INDEX_NONE)
		{
			HFleshRingMeshHitProxy* HitProxy = new HFleshRingMeshHitProxy(RingIndex);
			OutHitProxies.Add(HitProxy);
			return HitProxy;
		}

		// Fall back to default behavior if RingIndex is not set
		return FStaticMeshSceneProxy::CreateHitProxies(Component, OutHitProxies);
	}

private:
	int32 RingIndex;
};
#endif // WITH_EDITOR

UFleshRingMeshComponent::UFleshRingMeshComponent()
{
	// Default settings
	CastShadow = false;
	bCastDynamicShadow = false;
}

FPrimitiveSceneProxy* UFleshRingMeshComponent::CreateSceneProxy()
{
#if WITH_EDITOR
	// Create custom proxy in editor (with higher picking priority)
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
	// Use default proxy at runtime
	return Super::CreateSceneProxy();
#endif
}

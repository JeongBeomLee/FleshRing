// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingComponent.h"

UFleshRingComponent::UFleshRingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UFleshRingComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UFleshRingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// TODO: SDF 업데이트 및 메쉬 변형 로직 (C팀 구현 예정)
}

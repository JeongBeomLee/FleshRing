// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"

class IDetailChildrenBuilder;

/**
 * FFleshRingSettings 구조체의 프로퍼티 타입 커스터마이저
 * Bone 이름을 드롭다운으로 선택할 수 있게 함
 */
class FFleshRingSettingsCustomization : public IPropertyTypeCustomization
{
public:
	/** 커스터마이저 인스턴스 생성 (팩토리 함수) */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization 인터페이스 구현 */

	// Header row customization (collapsed view)
	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	// Children customization (expanded view)
	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> PropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	/** Bone 이름 목록 가져오기 */
	void UpdateBoneNameList();

	/** 드롭다운 열릴 때 호출 - 본 목록 갱신 */
	void OnComboBoxOpening();

	/** Bone 드롭다운에서 선택 시 호출 */
	void OnBoneNameSelected(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo);

	/** 현재 선택된 Bone 이름 가져오기 */
	FText GetCurrentBoneName() const;

	/** 현재 본이 유효하지 않은지 확인 (경고 아이콘 표시용) */
	bool IsBoneInvalid() const;

	/** EulerRotation에서 FQuat으로 동기화 */
	void SyncQuatFromEuler(
		TSharedPtr<IPropertyHandle> EulerHandle,
		TSharedPtr<IPropertyHandle> QuatHandle);

	/** FQuat 핸들에서 Euler 각도 읽기 */
	FRotator GetQuatAsEuler(TSharedPtr<IPropertyHandle> QuatHandle) const;

	/** FVector용 선형 드래그 Row 추가 */
	void AddLinearVectorRow(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> VectorHandle,
		const FText& DisplayName,
		float Delta);

	/** FRotator용 선형 드래그 Row 추가 */
	void AddLinearRotatorRow(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FText& DisplayName,
		float Delta);

	/** Euler 각도를 FQuat 핸들에 쓰기 */
	void SetEulerToQuat(TSharedPtr<IPropertyHandle> QuatHandle, const FRotator& Euler);

	/** 프로퍼티 핸들 캐싱 */
	TSharedPtr<IPropertyHandle> BoneNameHandle;

	/** 메인 프로퍼티 핸들 (Asset 접근용) */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	/** FQuat 프로퍼티 핸들 캐싱 (Euler 표시용) */
	TSharedPtr<IPropertyHandle> RingRotationHandle;
	TSharedPtr<IPropertyHandle> MeshRotationHandle;

	/** 사용 가능한 Bone 이름 목록 */
	TArray<TSharedPtr<FName>> BoneNameList;

	/** Asset에서 TargetSkeletalMesh 가져오기 */
	class USkeletalMesh* GetTargetSkeletalMesh() const;
};

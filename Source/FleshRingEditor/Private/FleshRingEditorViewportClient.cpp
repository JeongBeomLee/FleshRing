// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditorViewportClient.h"
#include "FleshRingEdMode.h"
#include "FleshRingPreviewScene.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingHitProxy.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingTypes.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "EngineUtils.h"
#include "SkeletalDebugRendering.h"
#include "Preferences/PersonaOptions.h"
#include "UnrealWidget.h"
#include "Editor.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "Stats/Stats.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Engine/StaticMesh.h"

// Stat 그룹 및 카운터 선언
DECLARE_STATS_GROUP(TEXT("FleshRingEditor"), STATGROUP_FleshRingEditor, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Tick"), STAT_FleshRingEditor_Tick, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("Draw"), STAT_FleshRingEditor_Draw, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("DrawRingGizmos"), STAT_FleshRingEditor_DrawRingGizmos, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("InputWidgetDelta"), STAT_FleshRingEditor_InputWidgetDelta, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("UpdateRingTransforms"), STAT_FleshRingEditor_UpdateRingTransforms, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("MarkPackageDirty"), STAT_FleshRingEditor_MarkPackageDirty, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("Invalidate"), STAT_FleshRingEditor_Invalidate, STATGROUP_FleshRingEditor);

IMPLEMENT_HIT_PROXY(HFleshRingGizmoHitProxy, HHitProxy);
IMPLEMENT_HIT_PROXY(HFleshRingAxisHitProxy, HHitProxy);
// HFleshRingMeshHitProxy는 FleshRingMeshComponent.cpp (Runtime 모듈)에서 IMPLEMENT_HIT_PROXY됨
IMPLEMENT_HIT_PROXY(HFleshRingBoneHitProxy, HHitProxy);

// 에셋별 설정 저장용 Config 섹션 베이스
static const FString FleshRingViewportConfigSectionBase = TEXT("FleshRingEditorViewport");

FFleshRingEditorViewportClient::FFleshRingEditorViewportClient(
	FEditorModeTools* InModeTools,
	FFleshRingPreviewScene* InPreviewScene,
	const TWeakPtr<SFleshRingEditorViewport>& InViewportWidget)
	: FEditorViewportClient(InModeTools, InPreviewScene, StaticCastSharedPtr<SEditorViewport>(InViewportWidget.Pin()))
	, PreviewScene(InPreviewScene)
	, ViewportWidget(InViewportWidget)
{
	// Widget에 ModeTools 연결 (ShouldDrawWidget 호출을 위해 필요)
	if (Widget && ModeTools)
	{
		Widget->SetUsesEditorModeTools(ModeTools.Get());
	}

	// 기본 카메라 설정
	SetViewLocation(FVector(-300, 200, 150));
	SetViewRotation(FRotator(-15, -30, 0));

	// 뷰포트 설정
	SetRealtime(true);
	DrawHelper.bDrawGrid = true;
	DrawHelper.bDrawPivot = false;
	DrawHelper.AxesLineThickness = 2.0f;
	DrawHelper.PivotSize = 5.0f;

	// 근거리 평면 기본값 (작은 오브젝트 확대 시 클리핑 방지)
	OverrideNearClipPlane(0.001f);

	// 배경 설정
	EngineShowFlags.SetGrid(true);
	EngineShowFlags.SetBones(false);  // 직접 그리기 위해 기본 본 렌더링 비활성화

	// 조명 설정
	EngineShowFlags.SetLighting(true);
	EngineShowFlags.SetPostProcessing(true);

	// Stats 표시 활성화 (FPS 등)
	SetShowStats(true);

	// 정적 인스턴스 레지스트리에 등록 (타입 안전 확인용)
	GetAllInstances().Add(this);

	// Preview Scene Settings 변경 델리게이트 구독
	if (UAssetViewerSettings* Settings = UAssetViewerSettings::Get())
	{
		AssetViewerSettingsChangedHandle = Settings->OnAssetViewerSettingsChanged().AddRaw(
			this, &FFleshRingEditorViewportClient::OnAssetViewerSettingsChanged);
	}

	// 초기 ShowFlags 적용
	ApplyPreviewSceneShowFlags();
}

FFleshRingEditorViewportClient::~FFleshRingEditorViewportClient()
{
	// Preview Scene Settings 변경 델리게이트 해제
	if (UAssetViewerSettings* Settings = UAssetViewerSettings::Get())
	{
		Settings->OnAssetViewerSettingsChanged().Remove(AssetViewerSettingsChangedHandle);
	}

	// 설정 저장
	SaveSettings();

	// 정적 인스턴스 레지스트리에서 제거
	GetAllInstances().Remove(this);
}

void FFleshRingEditorViewportClient::ToggleLocalCoordSystem()
{
	// 스케일 모드일 때는 토글하지 않음 (항상 로컬 유지)
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return;
	}

	bUseLocalCoordSystem = !bUseLocalCoordSystem;
	Invalidate();
}

bool FFleshRingEditorViewportClient::IsUsingLocalCoordSystem() const
{
	// 스케일 모드일 때는 항상 로컬
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return true;
	}

	return bUseLocalCoordSystem;
}

void FFleshRingEditorViewportClient::SetLocalCoordSystem(bool bLocal)
{
	// 스케일 모드일 때는 변경하지 않음 (항상 로컬 유지)
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return;
	}

	bUseLocalCoordSystem = bLocal;
	Invalidate();
}

void FFleshRingEditorViewportClient::Tick(float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_Tick);
	FEditorViewportClient::Tick(DeltaSeconds);

	// 첫 Tick에서 저장된 설정 로드 (생성자에서는 뷰포트가 아직 준비되지 않음)
	if (!bSettingsLoaded)
	{
		LoadSettings();
		bSettingsLoaded = true;
	}

	// 프리뷰 씬 틱
	if (PreviewScene)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);

		// Deformer 초기화 대기 상태 확인 - 메시가 렌더링되면 초기화 실행
		if (PreviewScene->IsPendingDeformerInit())
		{
			PreviewScene->ExecutePendingDeformerInit();
		}
	}

	// 카메라 포커스 보간 처리
	if (bIsCameraInterpolating)
	{
		const FVector CurrentLocation = GetViewLocation();
		const FVector NewLocation = FMath::VInterpTo(CurrentLocation, CameraTargetLocation, DeltaSeconds, CameraInterpSpeed);

		SetViewLocation(NewLocation);

		// 목표 지점에 충분히 가까워지면 보간 종료 (강제 스냅 없이)
		const float DistanceToTarget = FVector::Dist(NewLocation, CameraTargetLocation);
		if (DistanceToTarget < 0.01f)
		{
			bIsCameraInterpolating = false;
		}

		Invalidate();
	}

	// 선택된 링이 삭제되었는지 확인하고 선택 해제
	// (Undo/Redo 중에는 스킵 - RefreshViewport에서 복원됨)
	if (!bSkipSelectionValidation && SelectionType != EFleshRingSelectionType::None && PreviewScene)
	{
		int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
		bool bSelectionValid = false;

		if (EditingAsset.IsValid() && SelectedIndex >= 0)
		{
			bSelectionValid = EditingAsset->Rings.IsValidIndex(SelectedIndex);
		}

		if (!bSelectionValid)
		{
			ClearSelection();
		}
	}
}

void FFleshRingEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_Draw);
	// 본 그리기 배열 업데이트 (Persona 스타일 - 매 Draw마다 호출)
	UpdateBonesToDraw();

	FEditorViewportClient::Draw(View, PDI);

	// 본 렌더링 (BoneDrawMode가 None이 아닐 때만)
	if (BoneDrawMode != EFleshRingBoneDrawMode::None)
	{
		DrawMeshBones(PDI);
	}

	// Ring 기즈모 그리기
	if (bShowRingGizmos)
	{
		DrawRingGizmos(PDI);
	}
}

void FFleshRingEditorViewportClient::DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
	FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);

	// 본 이름 표시 (Persona 스타일)
	if (BoneDrawMode != EFleshRingBoneDrawMode::None && bShowBoneNames && PreviewScene)
	{
		UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
		if (MeshComponent && MeshComponent->GetSkeletalMeshAsset() && MeshComponent->IsRegistered())
		{
			const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
			const int32 NumBones = RefSkeleton.GetNum();
			const TArray<FTransform>& ComponentSpaceTransforms = MeshComponent->GetComponentSpaceTransforms();

			if (ComponentSpaceTransforms.Num() >= NumBones && BonesToDraw.Num() >= NumBones)
			{
				const int32 HalfX = static_cast<int32>(Viewport->GetSizeXY().X / 2 / GetDPIScale());
				const int32 HalfY = static_cast<int32>(Viewport->GetSizeXY().Y / 2 / GetDPIScale());

				for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
				{
					// BonesToDraw 배열로 그릴 본 결정 (본 렌더링과 동일)
					if (!BonesToDraw[BoneIdx])
					{
						continue;
					}

					const FVector BonePos = MeshComponent->GetComponentTransform().TransformPosition(
						ComponentSpaceTransforms[BoneIdx].GetLocation());

					// View->Project로 스크린 좌표 변환 (Persona 방식)
					const FPlane Proj = View.Project(BonePos);

					// proj.W > 0.f 체크로 카메라 뒤에 있는 본 숨김
					if (Proj.W > 0.f)
					{
						const int32 XPos = static_cast<int32>(HalfX + (HalfX * Proj.X));
						const int32 YPos = static_cast<int32>(HalfY + (HalfY * (Proj.Y * -1)));

						const FName BoneName = RefSkeleton.GetBoneName(BoneIdx);
						const FString BoneString = FString::Printf(TEXT("%d: %s"), BoneIdx, *BoneName.ToString());

						// Persona 스타일: 흰색 텍스트 + 검은 그림자
						FCanvasTextItem TextItem(
							FVector2D(XPos, YPos),
							FText::FromString(BoneString),
							GEngine->GetSmallFont(),
							FColor::White);
						TextItem.EnableShadow(FLinearColor::Black);
						Canvas.DrawItem(TextItem);
					}
				}
			}
		}
	}
}

bool FFleshRingEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	// 카메라 조작 키 입력 시 포커스 보간 중단
	if (bIsCameraInterpolating && EventArgs.Event == IE_Pressed)
	{
		if (EventArgs.Key == EKeys::RightMouseButton ||
			EventArgs.Key == EKeys::MiddleMouseButton ||
			EventArgs.Key == EKeys::MouseScrollUp ||
			EventArgs.Key == EKeys::MouseScrollDown)
		{
			bIsCameraInterpolating = false;
		}
	}

	if (EventArgs.Event == IE_Pressed)
	{
		// F키로 메시에 포커스 (카메라 조작 중에는 무시)
		if (EventArgs.Key == EKeys::F && !IsTracking())
		{
			FocusOnMesh();
			return true;
		}

		// Delete 키로 선택된 Ring 삭제
		if (EventArgs.Key == EKeys::Delete)
		{
			if (CanDeleteSelectedRing())
			{
				DeleteSelectedRing();
				return true;
			}
		}

		// Ctrl+` 로 Local/World 좌표계 전환
		if (EventArgs.Key == EKeys::Tilde &&
			(EventArgs.Viewport->KeyState(EKeys::LeftControl) || EventArgs.Viewport->KeyState(EKeys::RightControl)))
		{
			ToggleLocalCoordSystem();
			return true;
		}

		// 카메라 조작 중에는 QWER 키 무시 (우클릭 드래그 등)
		if (!IsTracking())
		{
			// QWER 키로 위젯 모드 전환 (ModeTools 사용)
			if (ModeTools)
			{
				if (EventArgs.Key == EKeys::Q)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_None);
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::W)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_Translate);
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::E)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_Rotate);
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::R)
				{
					ModeTools->SetWidgetMode(UE::Widget::WM_Scale);
					Invalidate();
					return true;
				}
			}

			// Shift+숫자키로 Show 토글 (Skeletal Mesh, Ring Gizmos, Ring Meshes)
			bool bShift = EventArgs.Viewport->KeyState(EKeys::LeftShift) || EventArgs.Viewport->KeyState(EKeys::RightShift);
			bool bCtrl = EventArgs.Viewport->KeyState(EKeys::LeftControl) || EventArgs.Viewport->KeyState(EKeys::RightControl);

			if (bShift && !bCtrl)
			{
				if (EventArgs.Key == EKeys::One)
				{
					ToggleShowSkeletalMesh();
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::Two)
				{
					ToggleShowRingGizmos();
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::Three)
				{
					ToggleShowRingMeshes();
					Invalidate();
					return true;
				}
			}
			// Ctrl+숫자키로 Debug 옵션 토글 (SDF Slice, Bulge Direction)
			else if (bCtrl && !bShift)
			{
				if (EventArgs.Key == EKeys::Two)
				{
					ToggleShowSDFSlice();
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::Four)
				{
					ToggleShowBulgeArrows();
					Invalidate();
					return true;
				}
			}
			// 숫자키만으로 Debug Visualization 토글
			else if (!bShift && !bCtrl)
			{
				if (EventArgs.Key == EKeys::One)
				{
					ToggleShowDebugVisualization();
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::Two)
				{
					ToggleShowSdfVolume();
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::Three)
				{
					ToggleShowAffectedVertices();
					Invalidate();
					return true;
				}
				if (EventArgs.Key == EKeys::Four)
				{
					ToggleShowBulgeHeatmap();
					Invalidate();
					return true;
				}
			}
		}
	}

	return FEditorViewportClient::InputKey(EventArgs);
}

bool FFleshRingEditorViewportClient::InputAxis(const FInputKeyEventArgs& Args)
{
	bool bResult = false;

	if (!bDisableInput && PreviewScene)
	{
		// FAdvancedPreviewScene::HandleViewportInput 호출 (K키로 Sky Rotation 처리)
		bResult = PreviewScene->HandleViewportInput(
			Args.Viewport,
			Args.InputDevice,
			Args.Key,
			Args.AmountDepressed,
			Args.DeltaTime,
			Args.NumSamples,
			Args.IsGamepad());

		if (bResult)
		{
			Invalidate();
		}
	}

	// 처리되지 않았으면 기본 클래스로 전달 (L키로 Light Direction 처리 포함)
	if (!bResult)
	{
		bResult = FEditorViewportClient::InputAxis(Args);
	}

	return bResult;
}

void FFleshRingEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	if (Key == EKeys::LeftMouseButton && Event == IE_Released)
	{
		if (HitProxy)
		{
			// Ring 기즈모 클릭
			if (HitProxy->IsA(HFleshRingGizmoHitProxy::StaticGetType()))
			{
				HFleshRingGizmoHitProxy* GizmoProxy = static_cast<HFleshRingGizmoHitProxy*>(HitProxy);
				if (PreviewScene && EditingAsset.IsValid())
				{
					// 선택 트랜잭션 생성 (Undo 가능)
					FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectRingGizmo", "Select Ring Gizmo"));
					EditingAsset->Modify();
					EditingAsset->EditorSelectedRingIndex = GizmoProxy->RingIndex;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::Gizmo;

					PreviewScene->SetSelectedRingIndex(GizmoProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Gizmo;
					Invalidate();

					// 트리/디테일 패널 동기화를 위한 델리게이트 호출
					OnRingSelectedInViewport.ExecuteIfBound(GizmoProxy->RingIndex, EFleshRingSelectionType::Gizmo);
				}
				return;
			}
			// Ring 메시 클릭 (커스텀 HitProxy) - 본보다 높은 우선순위 (HPP_Foreground)
			else if (HitProxy->IsA(HFleshRingMeshHitProxy::StaticGetType()))
			{
				HFleshRingMeshHitProxy* MeshProxy = static_cast<HFleshRingMeshHitProxy*>(HitProxy);
				if (PreviewScene && EditingAsset.IsValid())
				{
					// 선택 트랜잭션 생성 (Undo 가능)
					FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectRingMesh", "Select Ring Mesh"));
					EditingAsset->Modify();
					EditingAsset->EditorSelectedRingIndex = MeshProxy->RingIndex;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::Mesh;

					PreviewScene->SetSelectedRingIndex(MeshProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Mesh;
					Invalidate();

					// 트리/디테일 패널 동기화를 위한 델리게이트 호출
					OnRingSelectedInViewport.ExecuteIfBound(MeshProxy->RingIndex, EFleshRingSelectionType::Mesh);
				}
				return;
			}

			// 본 클릭 처리 (Ring 피킹보다 우선순위 낮음 - HPP_World)
			if (HitProxy->IsA(HFleshRingBoneHitProxy::StaticGetType()))
			{
				HFleshRingBoneHitProxy* BoneProxy = static_cast<HFleshRingBoneHitProxy*>(HitProxy);
				FName ClickedBoneName = BoneProxy->BoneName;

				// 기존 Ring 선택 해제
				if (PreviewScene)
				{
					PreviewScene->SetSelectedRingIndex(INDEX_NONE);
				}
				SelectionType = EFleshRingSelectionType::None;

				if (EditingAsset.IsValid())
				{
					EditingAsset->EditorSelectedRingIndex = INDEX_NONE;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
				}

				// 본 선택
				SetSelectedBone(ClickedBoneName);

				// 스켈레톤 트리 동기화 델리게이트 호출
				OnBoneSelectedInViewport.ExecuteIfBound(ClickedBoneName);

				Invalidate();
				return;
			}
		}

		// 빈 공간 클릭 - 선택 해제 (Ring + 본)
		ClearSelection();
		ClearSelectedBone();
	}

	// 우클릭 처리 - 컨텍스트 메뉴 표시
	if (Key == EKeys::RightMouseButton && Event == IE_Released)
	{
		FName TargetBoneName = NAME_None;

		// 1. 본 위에서 우클릭 - 해당 본 사용
		if (HitProxy && HitProxy->IsA(HFleshRingBoneHitProxy::StaticGetType()))
		{
			HFleshRingBoneHitProxy* BoneProxy = static_cast<HFleshRingBoneHitProxy*>(HitProxy);
			TargetBoneName = BoneProxy->BoneName;
			SetSelectedBone(TargetBoneName);
		}
		// 2. 본이 선택된 상태에서 빈 공간 우클릭 - 선택된 본 사용
		else if (!SelectedBoneName.IsNone())
		{
			TargetBoneName = SelectedBoneName;
		}

		// 대상 본이 있으면 컨텍스트 메뉴 표시
		if (!TargetBoneName.IsNone())
		{
			PendingRingAddBoneName = TargetBoneName;
			PendingRingAddScreenPos = FVector2D(static_cast<float>(HitX), static_cast<float>(HitY));
			ShowBoneContextMenu(TargetBoneName, FVector2D(static_cast<float>(HitX), static_cast<float>(HitY)));
			return;
		}
	}

	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

void FFleshRingEditorViewportClient::ClearSelection()
{
	// Undo/Redo 중에는 선택 해제 스킵 (어디서 호출되든 보호)
	if (bSkipSelectionValidation)
	{
		return;
	}

	// 이미 선택 해제 상태면 스킵 (불필요한 트랜잭션 방지)
	if (SelectionType == EFleshRingSelectionType::None)
	{
		return;
	}

	// 선택 해제 트랜잭션 생성 (Undo 가능)
	if (EditingAsset.IsValid())
	{
		FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "ClearRingSelection", "Clear Ring Selection"));
		EditingAsset->Modify();
		EditingAsset->EditorSelectedRingIndex = -1;
		EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
	}

	if (PreviewScene)
	{
		PreviewScene->SetSelectedRingIndex(-1);
	}
	SelectionType = EFleshRingSelectionType::None;
	Invalidate();
}

bool FFleshRingEditorViewportClient::CanDeleteSelectedRing() const
{
	if (!EditingAsset.IsValid() || !PreviewScene)
	{
		return false;
	}

	// Ring이 선택되어 있어야 함
	if (SelectionType == EFleshRingSelectionType::None)
	{
		return false;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	return EditingAsset->Rings.IsValidIndex(SelectedIndex);
}

void FFleshRingEditorViewportClient::DeleteSelectedRing()
{
	if (!CanDeleteSelectedRing())
	{
		return;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();

	// Undo/Redo 지원
	// 트랜잭션 범위를 제한하여 RefreshPreview()가 트랜잭션 밖에서 호출되도록 함
	// (트랜잭션 중 PreviewSubdividedMesh 생성 시 Undo 크래시 방지)
	{
		FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "DeleteRing", "Delete Ring"));
		EditingAsset->Modify();

		// Ring 삭제
		EditingAsset->Rings.RemoveAt(SelectedIndex);

		// 선택 해제 (Transient 제거했으므로 Undo 시 정상 복원됨)
		EditingAsset->EditorSelectedRingIndex = -1;
		EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
	}

	PreviewScene->SetSelectedRingIndex(-1);
	SelectionType = EFleshRingSelectionType::None;

	// 델리게이트 호출 (트리 갱신)
	// 트랜잭션 종료 후 호출하여 메시 생성이 Undo 히스토리에 포함되지 않도록 함
	OnRingDeletedInViewport.ExecuteIfBound();

	Invalidate();
}

void FFleshRingEditorViewportClient::SelectRing(int32 RingIndex, FName AttachedBoneName)
{
	if (RingIndex < 0)
	{
		// 이미 선택 해제 상태면 스킵 (중복 트랜잭션 방지 - RefreshTree 등에서 호출 시)
		if (EditingAsset.IsValid() && EditingAsset->EditorSelectionType == EFleshRingSelectionType::None)
		{
			return;
		}
		// 음수 인덱스 = Ring 선택 해제 (본 선택은 유지)
		ClearSelection();
		return;
	}

	// 이미 같은 Ring이 같은 타입으로 선택되어 있으면 스킵 (중복 트랜잭션 방지)
	if (EditingAsset.IsValid() &&
		EditingAsset->EditorSelectedRingIndex == RingIndex &&
		EditingAsset->EditorSelectionType != EFleshRingSelectionType::None)
	{
		// 본 하이라이트만 업데이트
		SelectedBoneName = AttachedBoneName;
		Invalidate();
		return;
	}

	// RingMesh 유무에 따라 SelectionType 결정
	// RingMesh가 없으면 Gizmo(가상 링/밴드) 선택, 있으면 Mesh 선택
	EFleshRingSelectionType NewSelectionType = EFleshRingSelectionType::Mesh;
	if (EditingAsset.IsValid() && EditingAsset->Rings.IsValidIndex(RingIndex))
	{
		const FFleshRingSettings& Ring = EditingAsset->Rings[RingIndex];
		if (Ring.RingMesh.IsNull())
		{
			NewSelectionType = EFleshRingSelectionType::Gizmo;
		}
	}

	// 선택 트랜잭션 생성 (Undo 가능)
	if (EditingAsset.IsValid())
	{
		FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectRing", "Select Ring"));
		EditingAsset->Modify();
		EditingAsset->EditorSelectedRingIndex = RingIndex;
		EditingAsset->EditorSelectionType = NewSelectionType;
	}

	// Ring이 부착된 본 하이라이트 (델리게이트 호출 없이 직접 설정)
	SelectedBoneName = AttachedBoneName;

	if (PreviewScene)
	{
		PreviewScene->SetSelectedRingIndex(RingIndex);
	}
	SelectionType = NewSelectionType;
	Invalidate();
}

void FFleshRingEditorViewportClient::SetAsset(UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	if (PreviewScene && InAsset)
	{
		// Asset 설정 (메시 + 컴포넌트 + Ring 시각화)
		PreviewScene->SetFleshRingAsset(InAsset);

		// 본 그리기 배열 업데이트
		UpdateBonesToDraw();

		// 카메라 포커스
		FocusOnMesh();
	}
}

void FFleshRingEditorViewportClient::SetSelectedBone(FName BoneName)
{
	SelectedBoneName = BoneName;
	UpdateBonesToDraw();
	Invalidate();
}

void FFleshRingEditorViewportClient::ClearSelectedBone()
{
	SelectedBoneName = NAME_None;
	UpdateBonesToDraw();
	Invalidate();

	// 스켈레톤 트리에 알림
	OnBoneSelectionCleared.ExecuteIfBound();
}

void FFleshRingEditorViewportClient::FocusOnMesh()
{
	if (!PreviewScene)
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	FBox FocusBox(ForceInit);
	FName BoneToFocus = SelectedBoneName;  // 포커스할 본 (로컬 변수)

	// 1. 선택된 Ring이 있으면 Ring에 포커스
	int32 SelectedRingIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedRingIndex >= 0 && EditingAsset.IsValid() && EditingAsset->Rings.IsValidIndex(SelectedRingIndex))
	{
		const FFleshRingSettings& Ring = EditingAsset->Rings[SelectedRingIndex];
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex != INDEX_NONE)
		{
			FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);

			// Ring 메시가 있으면 메시 기준으로 포커스
			if (Ring.RingMesh)
			{
				// 메시 바운드 가져오기
				FBoxSphereBounds MeshBounds = Ring.RingMesh->GetBounds();

				// 스케일 적용
				FVector ScaledExtent = MeshBounds.BoxExtent * Ring.MeshScale;
				float BoxExtent = ScaledExtent.GetMax();
				BoxExtent = FMath::Max(BoxExtent, 15.0f);

				// Ring 메시 위치 (MeshOffset 적용)
				FVector RingCenter = BoneTransform.GetLocation() + BoneTransform.GetRotation().RotateVector(Ring.MeshOffset);
				FocusBox = FBox(RingCenter - FVector(BoxExtent), RingCenter + FVector(BoxExtent));
			}
			else
			{
				// Ring 메시가 없으면 본에 포커스
				BoneToFocus = Ring.BoneName;
			}
		}
	}
	// 2. 선택된 본이 있으면 본에 포커스
	if (!FocusBox.IsValid && !BoneToFocus.IsNone())
	{
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(BoneToFocus);
		if (BoneIndex != INDEX_NONE)
		{
			FVector BoneLocation = SkelMeshComp->GetBoneTransform(BoneIndex).GetLocation();

			// 본 크기 추정 (자식 본까지의 거리)
			float BoxExtent = 15.0f;

			const FReferenceSkeleton& RefSkel = SkelMeshComp->GetSkeletalMeshAsset()->GetRefSkeleton();
			for (int32 i = 0; i < RefSkel.GetNum(); ++i)
			{
				if (RefSkel.GetParentIndex(i) == BoneIndex)
				{
					FVector ChildLocation = SkelMeshComp->GetBoneTransform(i).GetLocation();
					float DistToChild = FVector::Dist(BoneLocation, ChildLocation);
					BoxExtent = FMath::Max(BoxExtent, DistToChild * 0.5f);
				}
			}

			FocusBox = FBox(BoneLocation - FVector(BoxExtent), BoneLocation + FVector(BoxExtent));
		}
	}

	// 박스가 유효하지 않으면 메시 전체 바운드 사용 (Persona 방식)
	if (!FocusBox.IsValid)
	{
		// Persona 방식: SkeletalMesh의 GetBounds().GetBox() 사용
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		if (SkelMesh)
		{
			FocusBox = SkelMesh->GetBounds().GetBox();
		}
	}

	// 타겟 위치/회전 계산 (FocusViewportOnBox 로직 참고)
	const FVector BoxCenter = FocusBox.GetCenter();
	const FVector BoxExtent = FocusBox.GetExtent();
	const float BoxRadius = BoxExtent.Size();

	// 현재 카메라 방향 유지하면서 거리만 조정
	const FRotator CurrentRotation = GetViewRotation();
	const FVector ViewDirection = CurrentRotation.Vector();

	// 적절한 거리 계산 (FOV 고려)
	const float HalfFOVRadians = FMath::DegreesToRadians(ViewFOV * 0.5f);
	const float DistanceToFit = BoxRadius / FMath::Tan(HalfFOVRadians);

	// 타겟 위치 설정
	CameraTargetLocation = BoxCenter - ViewDirection * DistanceToFit * 1.5f;  // 1.5배 여유
	CameraTargetRotation = CurrentRotation;

	// 보간 시작
	bIsCameraInterpolating = true;
}

void FFleshRingEditorViewportClient::DrawMeshBones(FPrimitiveDrawInterface* PDI)
{
	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	// 컴포넌트가 등록되지 않았으면 스킵
	if (!MeshComponent->IsRegistered())
	{
		return;
	}

	if (MeshComponent->SkeletonDrawMode == ESkeletonDrawMode::Hidden)
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	// 본이 없으면 리턴
	if (NumBones == 0)
	{
		return;
	}

	// 스켈레탈 메시가 완전히 로드되었는지 확인
	const TArray<FTransform>& ComponentSpaceTransforms = MeshComponent->GetComponentSpaceTransforms();
	if (ComponentSpaceTransforms.Num() < NumBones)
	{
		return;
	}

	// 월드 Transform 배열 생성 (직접 ComponentSpaceTransforms 사용)
	TArray<FTransform> WorldTransforms;
	WorldTransforms.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		WorldTransforms[i] = ComponentSpaceTransforms[i] * MeshComponent->GetComponentTransform();
	}

	// 그릴 본 인덱스 (모든 본)
	TArray<FBoneIndexType> AllBoneIndices;
	AllBoneIndices.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		AllBoneIndices[i] = static_cast<FBoneIndexType>(i);
	}

	// 본 색상 배열 생성 (다중 컬러 사용 시 자동 생성)
	TArray<FLinearColor> BoneColors;
	BoneColors.SetNum(NumBones);
	if (bShowMultiColorBones)
	{
		SkeletalDebugRendering::FillWithMultiColors(BoneColors, NumBones);
	}
	else
	{
		for (int32 i = 0; i < NumBones; ++i)
		{
			BoneColors[i] = MeshComponent->GetBoneColor(i);
		}
	}

	// 선택된 본 인덱스 배열 생성
	TArray<int32> SelectedBones;
	if (!SelectedBoneName.IsNone())
	{
		int32 SelectedBoneIndex = RefSkeleton.FindBoneIndex(SelectedBoneName);
		if (SelectedBoneIndex != INDEX_NONE)
		{
			SelectedBones.Add(SelectedBoneIndex);
		}
	}

	// 본 피킹용 HitProxy 배열 (캐싱 - 스켈레탈 메시 변경 시만 재생성)
	USkeletalMesh* CurrentSkelMesh = MeshComponent->GetSkeletalMeshAsset();
	if (CachedSkeletalMesh.Get() != CurrentSkelMesh || CachedBoneHitProxies.Num() != NumBones)
	{
		CachedSkeletalMesh = CurrentSkelMesh;
		CachedBoneHitProxies.SetNum(NumBones);
		for (int32 i = 0; i < NumBones; ++i)
		{
			CachedBoneHitProxies[i] = new HFleshRingBoneHitProxy(i, RefSkeleton.GetBoneName(i));
		}
	}

	// EFleshRingBoneDrawMode를 EBoneDrawMode로 변환
	EBoneDrawMode::Type EngineBoneDrawMode = EBoneDrawMode::All;
	switch (BoneDrawMode)
	{
	case EFleshRingBoneDrawMode::None:
		EngineBoneDrawMode = EBoneDrawMode::None;
		break;
	case EFleshRingBoneDrawMode::Selected:
		EngineBoneDrawMode = EBoneDrawMode::Selected;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParents:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParents;
		break;
	case EFleshRingBoneDrawMode::SelectedAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndChildren;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParentsAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParentsAndChildren;
		break;
	case EFleshRingBoneDrawMode::All:
	default:
		EngineBoneDrawMode = EBoneDrawMode::All;
		break;
	}

	// DrawConfig 설정
	FSkelDebugDrawConfig DrawConfig;
	DrawConfig.BoneDrawMode = EngineBoneDrawMode;
	DrawConfig.BoneDrawSize = BoneDrawSize;
	DrawConfig.bForceDraw = false;
	DrawConfig.bAddHitProxy = true;  // 본 피킹 활성화
	DrawConfig.bUseMultiColorAsDefaultColor = bShowMultiColorBones;
	DrawConfig.DefaultBoneColor = GetDefault<UPersonaOptions>()->DefaultBoneColor;
	DrawConfig.SelectedBoneColor = GetDefault<UPersonaOptions>()->SelectedBoneColor;  // 초록색
	DrawConfig.AffectedBoneColor = GetDefault<UPersonaOptions>()->AffectedBoneColor;
	DrawConfig.ParentOfSelectedBoneColor = GetDefault<UPersonaOptions>()->ParentOfSelectedBoneColor;  // 노란색

	// 본 렌더링 (BonesToDraw 멤버 변수 사용 - Persona 스타일)
	SkeletalDebugRendering::DrawBones(
		PDI,
		MeshComponent->GetComponentLocation(),
		AllBoneIndices,
		RefSkeleton,
		WorldTransforms,
		SelectedBones,  // 선택된 본 전달 (초록색 + 부모 노란색 연결선)
		BoneColors,
		CachedBoneHitProxies,  // 캐시된 본 피킹용 HitProxy 배열
		DrawConfig,
		BonesToDraw
	);
}

void FFleshRingEditorViewportClient::DrawRingGizmos(FPrimitiveDrawInterface* PDI)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_DrawRingGizmos);
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();

	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		const FFleshRingSettings& Ring = Rings[i];

		// 본 Transform 가져오기
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
		FVector BoneLocation = BoneTransform.GetLocation();
		FQuat BoneRotation = BoneTransform.GetRotation();

		// Ring 메시 피킹 영역 (모든 모드에서 적용, 메시가 있을 때만)
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (RingMesh)
		{
			PDI->SetHitProxy(new HFleshRingMeshHitProxy(i));

			// 메시 위치 계산
			FVector MeshLocation = BoneLocation + BoneRotation.RotateVector(Ring.MeshOffset);

			// 메시 바운드 기반 피킹 영역 크기
			FBoxSphereBounds MeshBounds = RingMesh->GetBounds();
			float MeshRadius = MeshBounds.SphereRadius * FMath::Max3(Ring.MeshScale.X, Ring.MeshScale.Y, Ring.MeshScale.Z);

			// 보이지 않는 구체로 피킹 영역 설정 (SDPG_World로 본보다 뒤에)
			DrawWireSphere(PDI, MeshLocation, FLinearColor(0, 0, 0, 0), MeshRadius, 8, SDPG_World);

			PDI->SetHitProxy(nullptr);
		}

		// Manual 모드일 때만 Ring 기즈모 표시 (SDF 모드에서는 Radius가 의미 없음)
		if (Ring.InfluenceMode != EFleshRingInfluenceMode::Manual)
		{
			// VirtualBand 모드: 4-레이어 밴드 기즈모
			if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
			{
				FLinearColor GizmoColor = (i == SelectedIndex)
					? ((SelectionType == EFleshRingSelectionType::Gizmo) ? FLinearColor::Yellow : FLinearColor(1.0f, 0.0f, 1.0f, 1.0f))
					: FLinearColor(0.0f, 1.0f, 1.0f, 0.8f);

				PDI->SetHitProxy(new HFleshRingGizmoHitProxy(i));

				USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
				const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
				FTransform BindPoseBoneTransform = FTransform::Identity;
				int32 CurrentBoneIdx = BoneIndex;
				while (CurrentBoneIdx != INDEX_NONE)
				{
					BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
					CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
				}

				// Virtual Band 기즈모: 전용 BandOffset/BandRotation 사용
				const FProceduralBandSettings& Band = Ring.ProceduralBand;
				FTransform BandTransform;
				BandTransform.SetLocation(Band.BandOffset);
				BandTransform.SetRotation(Band.BandRotation);
				BandTransform.SetScale3D(FVector::OneVector);  // 밴드 기즈모는 스케일 없음

				FTransform LocalToWorld = BandTransform * BindPoseBoneTransform * SkelMeshComp->GetComponentTransform();
				constexpr int32 Segments = 32;
				constexpr float HeightEpsilon = 0.0001f;
				const bool bHasLowerSection = (Band.Lower.Height > HeightEpsilon);
				const bool bHasUpperSection = (Band.Upper.Height > HeightEpsilon);

				// 새 좌표계: Z=0이 Mid Band 중심
				const float MidOffset = Band.Lower.Height + Band.BandHeight * 0.5f;
				float LowerZ = -MidOffset;  // Lower 하단
				float BandLowerZ = -Band.BandHeight * 0.5f;  // Band 하단
				float BandUpperZ = Band.BandHeight * 0.5f;   // Band 상단
				float UpperZ = Band.Upper.Height + Band.BandHeight * 0.5f;  // Upper 상단

				auto DrawCircle = [&](float R, float Z, float T) {
					for (int32 s = 0; s < Segments; ++s) {
						float A1 = (float)s / Segments * 2.0f * PI, A2 = (float)(s + 1) / Segments * 2.0f * PI;
						PDI->DrawLine(LocalToWorld.TransformPosition(FVector(FMath::Cos(A1) * R, FMath::Sin(A1) * R, Z)),
							LocalToWorld.TransformPosition(FVector(FMath::Cos(A2) * R, FMath::Sin(A2) * R, Z)), GizmoColor, SDPG_Foreground, T);
					}
				};

				// Height=0인 섹션은 스킵하고 Mid 값만 사용
				if (bHasLowerSection)
				{
					DrawCircle(Band.Lower.Radius, LowerZ, 0.0f);
				}
				DrawCircle(Band.MidLowerRadius, BandLowerZ, 0.5f);
				DrawCircle(Band.MidUpperRadius, BandUpperZ, 0.5f);
				if (bHasUpperSection)
				{
					DrawCircle(Band.Upper.Radius, UpperZ, 0.0f);
				}

				for (int32 q = 0; q < 4; ++q) {
					float Angle = (float)q / 4.0f * 2.0f * PI;
					FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
					// Lower → MidLower (Lower 섹션이 있는 경우에만)
					if (bHasLowerSection)
					{
						PDI->DrawLine(LocalToWorld.TransformPosition(Dir * Band.Lower.Radius + FVector(0, 0, LowerZ)),
							LocalToWorld.TransformPosition(Dir * Band.MidLowerRadius + FVector(0, 0, BandLowerZ)), GizmoColor, SDPG_Foreground, 0.0f);
					}
					// MidLower → MidUpper (항상)
					PDI->DrawLine(LocalToWorld.TransformPosition(Dir * Band.MidLowerRadius + FVector(0, 0, BandLowerZ)),
						LocalToWorld.TransformPosition(Dir * Band.MidUpperRadius + FVector(0, 0, BandUpperZ)), GizmoColor, SDPG_Foreground, 0.0f);
					// MidUpper → Upper (Upper 섹션이 있는 경우에만)
					if (bHasUpperSection)
					{
						PDI->DrawLine(LocalToWorld.TransformPosition(Dir * Band.MidUpperRadius + FVector(0, 0, BandUpperZ)),
							LocalToWorld.TransformPosition(Dir * Band.Upper.Radius + FVector(0, 0, UpperZ)), GizmoColor, SDPG_Foreground, 0.0f);
					}
				}
				DrawWireSphere(PDI, LocalToWorld.TransformPosition(FVector::ZeroVector), GizmoColor, 2.0f, 8, SDPG_Foreground);
				PDI->SetHitProxy(nullptr);
			}
			continue;
		}

		// 본 회전 * 링 회전 = 월드 회전 (기본값으로 본의 X축과 링의 Z축이 일치)
		FQuat RingWorldRotation = BoneRotation * Ring.RingRotation;

		// RingOffset 적용
		FVector GizmoLocation = BoneLocation + BoneRotation.RotateVector(Ring.RingOffset);

		// Ring 색상 결정
		FLinearColor GizmoColor;
		if (i == SelectedIndex)
		{
			// 선택된 Ring: Gizmo=노랑, Mesh=마젠타
			GizmoColor = (SelectionType == EFleshRingSelectionType::Gizmo)
				? FLinearColor::Yellow
				: FLinearColor(1.0f, 0.0f, 1.0f, 1.0f); // 마젠타 (Mesh 선택 시)
		}
		else
		{
			// 선택 안 된 Ring: 시안
			GizmoColor = FLinearColor(0.0f, 1.0f, 1.0f, 0.8f);
		}

		// HitProxy 설정 (Ring 기즈모용)
		PDI->SetHitProxy(new HFleshRingGizmoHitProxy(i));

		// Ring 밴드 시각화 (직사각형 단면 원환체 = Hollow Cylinder)
		// RingRadius = 내부 반지름 (링이 살을 누르는 표면)
		// RingThickness = 벽 두께 (안쪽→바깥쪽 한 방향)
		float InnerRadius = Ring.RingRadius;
		float OuterRadius = Ring.RingRadius + Ring.RingThickness;
		float HalfHeight = Ring.RingHeight / 2.0f;

		// 상단/하단 면 채우기 (방사형 직선으로 빈틈없이)
		int32 FillSegments = 360;  // 촘촘한 방사형 선
		float ZOffsets[2] = { -HalfHeight, HalfHeight };

		for (float ZOffset : ZOffsets)
		{
			for (int32 s = 0; s < FillSegments; ++s)
			{
				float Angle = (float)s / FillSegments * 2.0f * PI;
				FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

				FVector InnerPt = GizmoLocation + RingWorldRotation.RotateVector(Dir * InnerRadius + FVector(0, 0, ZOffset));
				FVector OuterPt = GizmoLocation + RingWorldRotation.RotateVector(Dir * OuterRadius + FVector(0, 0, ZOffset));
				PDI->DrawLine(InnerPt, OuterPt, GizmoColor, SDPG_Foreground, 0.0f);  // 최소 굵기
			}
		}

		// 내부/외부 원 경계선 (상/하)
		int32 CircleSegments = 64;
		float Radii[2] = { InnerRadius, OuterRadius };
		for (float Radius : Radii)
		{
			for (float ZOffset : ZOffsets)
			{
				for (int32 s = 0; s < CircleSegments; ++s)
				{
					float Angle1 = (float)s / CircleSegments * 2.0f * PI;
					float Angle2 = (float)(s + 1) / CircleSegments * 2.0f * PI;

					FVector P1 = GizmoLocation + RingWorldRotation.RotateVector(
						FVector(FMath::Cos(Angle1) * Radius, FMath::Sin(Angle1) * Radius, ZOffset));
					FVector P2 = GizmoLocation + RingWorldRotation.RotateVector(
						FVector(FMath::Cos(Angle2) * Radius, FMath::Sin(Angle2) * Radius, ZOffset));
					PDI->DrawLine(P1, P2, GizmoColor, SDPG_Foreground, 0.0f);
				}
			}
		}

		// 수직 연결선 (4개 방향)
		for (int32 q = 0; q < 4; ++q)
		{
			float Angle = (float)q / 4.0f * 2.0f * PI;
			FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

			FVector InnerBottom = GizmoLocation + RingWorldRotation.RotateVector(Dir * InnerRadius + FVector(0, 0, -HalfHeight));
			FVector InnerTop = GizmoLocation + RingWorldRotation.RotateVector(Dir * InnerRadius + FVector(0, 0, HalfHeight));
			PDI->DrawLine(InnerBottom, InnerTop, GizmoColor, SDPG_Foreground, 0.0f);

			FVector OuterBottom = GizmoLocation + RingWorldRotation.RotateVector(Dir * OuterRadius + FVector(0, 0, -HalfHeight));
			FVector OuterTop = GizmoLocation + RingWorldRotation.RotateVector(Dir * OuterRadius + FVector(0, 0, HalfHeight));
			PDI->DrawLine(OuterBottom, OuterTop, GizmoColor, SDPG_Foreground, 0.0f);
		}

		// 본 위치에 작은 구 표시
		DrawWireSphere(PDI, GizmoLocation, GizmoColor, 2.0f, 8, SDPG_Foreground);

		// HitProxy 해제
		PDI->SetHitProxy(nullptr);
	}
}

FVector FFleshRingEditorViewportClient::GetWidgetLocation() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FVector::ZeroVector;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FVector::ZeroVector;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FVector::ZeroVector;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FVector::ZeroVector;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	// VirtualBand 모드: 바인드 포즈 사용 (SDF/변형과 일치)
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
	{
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		FTransform BindPoseBoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}

		// SelectionType에 따라 다른 오프셋 적용
		// Gizmo 선택 → BandOffset 사용 (밴드 트랜스폼)
		// Mesh 선택 → MeshOffset 사용 (메시 트랜스폼)
		const FProceduralBandSettings& Band = Ring.ProceduralBand;
		FTransform LocalTransform;
		if (SelectionType == EFleshRingSelectionType::Gizmo)
		{
			LocalTransform.SetLocation(Band.BandOffset);
			LocalTransform.SetRotation(Band.BandRotation);
			LocalTransform.SetScale3D(FVector::OneVector);  // 밴드는 스케일 없음
		}
		else
		{
			LocalTransform.SetLocation(Ring.MeshOffset);
			LocalTransform.SetRotation(Ring.MeshRotation);
			LocalTransform.SetScale3D(Ring.MeshScale);
		}
		FTransform LocalToWorld = LocalTransform * BindPoseBoneTransform * SkelMeshComp->GetComponentTransform();
		return LocalToWorld.GetLocation();
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FVector BoneLocation = BoneTransform.GetLocation();

	// 선택 타입에 따라 다른 오프셋 적용
	// Manual Gizmo 선택 → RingOffset 사용 (밴드 트랜스폼)
	// Mesh 선택 또는 Auto 모드 → MeshOffset 사용 (메시 트랜스폼)
	if (SelectionType == EFleshRingSelectionType::Gizmo &&
		Ring.InfluenceMode == EFleshRingInfluenceMode::Manual)
	{
		return BoneLocation + BoneTransform.GetRotation().RotateVector(Ring.RingOffset);
	}
	else
	{
		// Mesh 선택 또는 Auto 모드
		return BoneLocation + BoneTransform.GetRotation().RotateVector(Ring.MeshOffset);
	}
}

FMatrix FFleshRingEditorViewportClient::GetWidgetCoordSystem() const
{
	// AlignRotation 좌표계 반환 (내부에서 검증 수행)
	return GetSelectedRingAlignMatrix();
}

FMatrix FFleshRingEditorViewportClient::GetSelectedRingAlignMatrix() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FMatrix::Identity;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FMatrix::Identity;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FMatrix::Identity;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FMatrix::Identity;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FMatrix::Identity;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FQuat BoneRotation = BoneTransform.GetRotation();

	// VirtualBand 모드: 바인드 포즈 사용
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
	{
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		FTransform BindPoseBoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}
		BoneRotation = SkelMeshComp->GetComponentTransform().GetRotation() * BindPoseBoneTransform.GetRotation();
	}

	// 좌표계 모드 확인 (World vs Local) - 커스텀 플래그 사용
	if (bUseLocalCoordSystem)
	{
		FQuat TargetRotation;
		if (bIsDraggingRotation)
		{
			// 드래그 중이면 드래그 시작 시점의 회전 사용 (기즈모 고정)
			TargetRotation = DragStartWorldRotation;
		}
		else
		{
			// 로컬 모드: 본 회전 * 링/메시 회전 = 현재 월드 회전
			FQuat CurrentRotation;
			if (SelectionType == EFleshRingSelectionType::Gizmo)
			{
				if (Ring.InfluenceMode == EFleshRingInfluenceMode::Manual)
				{
					// Manual Gizmo 선택은 RingRotation 사용
					CurrentRotation = Ring.RingRotation;
				}
				else if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
				{
					// Virtual Band Gizmo 선택은 전용 BandRotation 사용
					CurrentRotation = Ring.ProceduralBand.BandRotation;
				}
				else
				{
					CurrentRotation = Ring.MeshRotation;
				}
			}
			else
			{
				// Mesh 선택은 MeshRotation 사용 (메시 트랜스폼)
				CurrentRotation = Ring.MeshRotation;
			}
			TargetRotation = BoneRotation * CurrentRotation;
		}

		// FQuatRotationMatrix 사용 (FRotator 변환 시 Gimbal lock 문제 방지)
		return FQuatRotationMatrix(TargetRotation);
	}
	else
	{
		// 월드 모드: 순수 월드 축 기준
		return FMatrix::Identity;
	}
}

ECoordSystem FFleshRingEditorViewportClient::GetWidgetCoordSystemSpace() const
{
	// 항상 COORD_World를 반환하여 Widget 시스템의 Local Space 회전 반전 로직을 비활성화
	// GetWidgetCoordSystem()에서 이미 회전된 좌표계를 반환하고 있으므로,
	// 추가적인 Local Space 처리가 필요 없음
	return COORD_World;
}

void FFleshRingEditorViewportClient::SetWidgetCoordSystemSpace(ECoordSystem NewCoordSystem)
{
	// 기본 툴바 버튼 클릭 시 호출됨 - 커스텀 플래그 토글
	bUseLocalCoordSystem = (NewCoordSystem == COORD_Local);
	Invalidate();
}

UE::Widget::EWidgetMode FFleshRingEditorViewportClient::GetWidgetMode() const
{
	// ModeTools의 위젯 모드 사용 (툴바 하이라이트 동작)
	if (ModeTools)
	{
		return ModeTools->GetWidgetMode();
	}
	return UE::Widget::WM_Translate;
}

bool FFleshRingEditorViewportClient::InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_InputWidgetDelta);
	// Widget 축이 선택되지 않았으면 처리 안 함
	if (CurrentAxis == EAxisList::None)
	{
		return false;
	}

	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return false;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return false;
	}

	TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return false;
	}

	// 본 Transform 가져오기 (로컬 좌표 변환용)
	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return false;
	}

	FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return false;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FQuat BoneRotation = BoneTransform.GetRotation();

	// VirtualBand 모드: 바인드 포즈 사용
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
	{
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		FTransform BindPoseBoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}
		BoneTransform = BindPoseBoneTransform * SkelMeshComp->GetComponentTransform();
		BoneRotation = SkelMeshComp->GetComponentTransform().GetRotation() * BindPoseBoneTransform.GetRotation();
	}

	// 스냅 적용
	const ULevelEditorViewportSettings* ViewportSettings = GetDefault<ULevelEditorViewportSettings>();

	// 이동 스냅 - 기즈모 축 기준으로 적용
	// Widget 시스템은 월드 좌표로 Drag를 전달하므로, 기즈모 로컬로 변환 후 스냅 적용
	FVector SnappedDrag = Drag;
	if (ViewportSettings->GridEnabled && GEditor && !Drag.IsZero())
	{
		float GridSize = GEditor->GetGridSize();

		// 기즈모 좌표계 행렬 가져오기
		FMatrix GizmoMatrix = GetSelectedRingAlignMatrix();
		FMatrix GizmoMatrixInverse = GizmoMatrix.Inverse();

		// 월드 Drag를 기즈모 로컬 좌표로 변환
		FVector LocalDragForSnap = GizmoMatrixInverse.TransformVector(Drag);

		// 기즈모 로컬 좌표에서 스냅 적용
		FVector SnappedLocalDrag;
		SnappedLocalDrag.X = FMath::GridSnap(LocalDragForSnap.X, GridSize);
		SnappedLocalDrag.Y = FMath::GridSnap(LocalDragForSnap.Y, GridSize);
		SnappedLocalDrag.Z = FMath::GridSnap(LocalDragForSnap.Z, GridSize);

		// 다시 월드 좌표로 변환
		SnappedDrag = GizmoMatrix.TransformVector(SnappedLocalDrag);
	}

	// 회전 스냅
	FRotator SnappedRot = Rot;
	if (ViewportSettings->RotGridEnabled && GEditor)
	{
		FRotator RotGridSize = GEditor->GetRotGridSize();
		SnappedRot.Pitch = FMath::GridSnap(Rot.Pitch, RotGridSize.Pitch);
		SnappedRot.Yaw = FMath::GridSnap(Rot.Yaw, RotGridSize.Yaw);
		SnappedRot.Roll = FMath::GridSnap(Rot.Roll, RotGridSize.Roll);
	}

	// 스케일 스냅
	FVector SnappedScale = Scale;
	if (ViewportSettings->SnapScaleEnabled && GEditor)
	{
		float ScaleGridSize = GEditor->GetScaleGridSize();
		SnappedScale.X = FMath::GridSnap(Scale.X, ScaleGridSize);
		SnappedScale.Y = FMath::GridSnap(Scale.Y, ScaleGridSize);
		SnappedScale.Z = FMath::GridSnap(Scale.Z, ScaleGridSize);
	}

	// 월드 드래그를 본 로컬 좌표로 변환
	// (Widget 시스템은 World/Local 모드 상관없이 항상 월드 좌표로 Drag를 전달함)
	FVector LocalDrag = BoneRotation.UnrotateVector(SnappedDrag);

	// 선택 타입에 따라 다른 오프셋 업데이트
	if (SelectionType == EFleshRingSelectionType::Gizmo)
	{
		// Auto 모드: Gizmo 선택이어도 MeshOffset/MeshRotation 사용 (SDF 기반)
		// (Auto는 기즈모가 없으므로 여기로 오면 안 되지만 안전을 위해 처리)
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto)
		{
			// 이동 -> MeshOffset 업데이트
			Ring.MeshOffset += LocalDrag;

			// 회전 -> MeshRotation 업데이트
			if (bIsDraggingRotation)
			{
				FQuat FrameDeltaRotation = Rot.Quaternion();
				if (!FrameDeltaRotation.IsIdentity())
				{
					AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
					AccumulatedDeltaRotation.Normalize();

					FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
					NewWorldRotation.Normalize();

					FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
					Ring.MeshRotation = NewLocalRotation;
					Ring.MeshEulerRotation = NewLocalRotation.Rotator();
				}
			}

			// 스케일 처리 -> MeshScale 사용
			if (!SnappedScale.IsZero())
			{
				Ring.MeshScale += SnappedScale;
				Ring.MeshScale.X = FMath::Max(Ring.MeshScale.X, 0.01f);
				Ring.MeshScale.Y = FMath::Max(Ring.MeshScale.Y, 0.01f);
				Ring.MeshScale.Z = FMath::Max(Ring.MeshScale.Z, 0.01f);
			}
		}
		else if (Ring.InfluenceMode == EFleshRingInfluenceMode::Manual)
		{
			// Manual 모드: RingOffset/RingRotation 사용
			Ring.RingOffset += LocalDrag;

			if (bIsDraggingRotation)
			{
				FQuat FrameDeltaRotation = Rot.Quaternion();
				if (!FrameDeltaRotation.IsIdentity())
				{
					AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
					AccumulatedDeltaRotation.Normalize();

					FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
					NewWorldRotation.Normalize();

					FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
					Ring.RingRotation = NewLocalRotation;
					Ring.RingEulerRotation = NewLocalRotation.Rotator();
				}
			}

			// 스케일 처리: RingRadius 조절
			if (!SnappedScale.IsZero())
			{
				float ScaleDelta = FMath::Max3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
				if (ScaleDelta == 0.0f)
				{
					ScaleDelta = FMath::Min3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
				}
				Ring.RingRadius = FMath::Clamp(Ring.RingRadius * (1.0f + ScaleDelta), 0.1f, 100.0f);
			}
		}
		else if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
		{
			// Virtual Band 모드: 전용 BandOffset/BandRotation 사용
			FProceduralBandSettings& BandSettings = Ring.ProceduralBand;
			BandSettings.BandOffset += LocalDrag;

			if (bIsDraggingRotation)
			{
				FQuat FrameDeltaRotation = Rot.Quaternion();
				if (!FrameDeltaRotation.IsIdentity())
				{
					AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
					AccumulatedDeltaRotation.Normalize();

					FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
					NewWorldRotation.Normalize();

					FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
					BandSettings.BandRotation = NewLocalRotation;
					BandSettings.BandEulerRotation = NewLocalRotation.Rotator();
				}
			}

			// 스케일 처리: 전체 비례 스케일
			if (!SnappedScale.IsZero())
			{
				float ScaleDelta = FMath::Max3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
				if (ScaleDelta == 0.0f)
				{
					ScaleDelta = FMath::Min3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
				}
				float ScaleFactor = 1.0f + ScaleDelta;
				BandSettings.MidUpperRadius = FMath::Clamp(BandSettings.MidUpperRadius * ScaleFactor, 0.1f, 100.0f);
				BandSettings.MidLowerRadius = FMath::Clamp(BandSettings.MidLowerRadius * ScaleFactor, 0.1f, 100.0f);
				BandSettings.BandHeight = FMath::Clamp(BandSettings.BandHeight * ScaleFactor, 0.1f, 100.0f);
				BandSettings.BandThickness = FMath::Clamp(BandSettings.BandThickness * ScaleFactor, 0.1f, 50.0f);
				BandSettings.Upper.Radius = FMath::Clamp(BandSettings.Upper.Radius * ScaleFactor, 0.1f, 100.0f);
				BandSettings.Upper.Height = FMath::Clamp(BandSettings.Upper.Height * ScaleFactor, 0.0f, 100.0f);
				BandSettings.Lower.Radius = FMath::Clamp(BandSettings.Lower.Radius * ScaleFactor, 0.1f, 100.0f);
				BandSettings.Lower.Height = FMath::Clamp(BandSettings.Lower.Height * ScaleFactor, 0.0f, 100.0f);
			}
		}
	}
	else if (SelectionType == EFleshRingSelectionType::Mesh)
	{
		// Ring 메시 이동 -> MeshOffset 업데이트
		Ring.MeshOffset += LocalDrag;

		// 회전도 적용
		if (bIsDraggingRotation)
		{
			// Widget이 주는 Rot는 월드 좌표계로 분해된 값
			// 로컬 축 회전이 월드 FRotator로 변환되면서 Pitch/Yaw/Roll에 분산됨
			// 따라서 Rot 전체를 쿼터니언으로 변환하여 사용해야 회전이 정확히 360도 누적됨
			FQuat FrameDeltaRotation = Rot.Quaternion();

			if (!FrameDeltaRotation.IsIdentity())
			{
				// 누적 델타에 추가 (짐벌락 방지: FRotator를 다시 읽지 않고 쿼터니언으로 누적)
				AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
				AccumulatedDeltaRotation.Normalize();

				// 드래그 시작 회전에 누적 델타 적용
				FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
				NewWorldRotation.Normalize();

				// 월드 회전을 본 로컬 회전으로 변환 후 저장
				FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
				Ring.MeshRotation = NewLocalRotation;
				Ring.MeshEulerRotation = NewLocalRotation.Rotator();  // EulerRotation 동기화
			}
		}

		// 스케일 적용
		if (!SnappedScale.IsZero())
		{
			Ring.MeshScale += SnappedScale;
			// 최소값만 클램프
			Ring.MeshScale.X = FMath::Max(Ring.MeshScale.X, 0.01f);
			Ring.MeshScale.Y = FMath::Max(Ring.MeshScale.Y, 0.01f);
			Ring.MeshScale.Z = FMath::Max(Ring.MeshScale.Z, 0.01f);
		}

		// StaticMeshComponent Transform 업데이트
		FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(Ring.MeshOffset);
		FQuat WorldRotation = BoneRotation * Ring.MeshRotation;

		// 1. PreviewScene의 RingMeshComponents 업데이트 (Deformer 비활성화 시)
		const TArray<UFleshRingMeshComponent*>& RingMeshComponents = PreviewScene->GetRingMeshComponents();
		if (RingMeshComponents.IsValidIndex(SelectedIndex) && RingMeshComponents[SelectedIndex])
		{
			RingMeshComponents[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
			RingMeshComponents[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
		}

		// 2. FleshRingComponent의 RingMeshComponents 업데이트 (Deformer 활성화 시)
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			const auto& ComponentRingMeshes = FleshRingComp->GetRingMeshComponents();
			if (ComponentRingMeshes.IsValidIndex(SelectedIndex) && ComponentRingMeshes[SelectedIndex])
			{
				ComponentRingMeshes[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
				ComponentRingMeshes[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
			}
		}
	}

	// 성능 최적화: MarkPackageDirty()는 TrackingStopped()에서 한 번만 호출
	// 드래그 중 매 프레임 호출 시 5-10ms 오버헤드 발생

	// 트랜스폼만 업데이트 (Deformer 유지, 깜빡임 방지)
	// 선택된 Ring 인덱스를 전달하여 해당 Ring만 처리 (최적화)
	if (PreviewScene)
	{
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_UpdateRingTransforms);
			int32 SelectedRingIndex = PreviewScene->GetSelectedRingIndex();
			FleshRingComp->UpdateRingTransforms(SelectedRingIndex);
		}
	}

	// 뷰포트 갱신
	{
		SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_Invalidate);
		Invalidate();
	}

	return true;
}

void FFleshRingEditorViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge)
{
	// 드래그 시작 시 트랜잭션 시작
	if (bIsDraggingWidget && EditingAsset.IsValid() && SelectionType != EFleshRingSelectionType::None)
	{
		ScopedTransaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("FleshRingEditor", "ModifyRingTransform", "Modify Ring Transform"));
		EditingAsset->Modify();

		// 회전 모드일 때만 회전 관련 초기화 수행
		bool bIsRotationMode = ModeTools && ModeTools->GetWidgetMode() == UE::Widget::WM_Rotate;
		if (bIsRotationMode)
		{
			// 드래그 시작 시 초기 회전 저장 (기즈모 좌표계 고정용)
			USkeletalMeshComponent* SkelMeshComp = PreviewScene ? PreviewScene->GetSkeletalMeshComponent() : nullptr;
			int32 SelectedIndex = PreviewScene ? PreviewScene->GetSelectedRingIndex() : -1;

			if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() && EditingAsset->Rings.IsValidIndex(SelectedIndex))
			{
				const FFleshRingSettings& Ring = EditingAsset->Rings[SelectedIndex];
				int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
				if (BoneIndex != INDEX_NONE)
				{
					FQuat BoneRotation = SkelMeshComp->GetBoneTransform(BoneIndex).GetRotation();

					// VirtualBand 모드: 바인드 포즈 사용
					if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
					{
						USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
						const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
						FTransform BindPoseBoneTransform = FTransform::Identity;
						int32 CurrentBoneIdx = BoneIndex;
						while (CurrentBoneIdx != INDEX_NONE)
						{
							BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
							CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
						}
						FTransform ComponentToWorld = SkelMeshComp->GetComponentTransform();
						BoneRotation = ComponentToWorld.GetRotation() * BindPoseBoneTransform.GetRotation();
					}

					// Gizmo 선택 시 모드별 회전 사용
					FQuat CurrentRotation;
					if (SelectionType == EFleshRingSelectionType::Gizmo)
					{
						if (Ring.InfluenceMode == EFleshRingInfluenceMode::Manual)
						{
							CurrentRotation = Ring.RingRotation;
						}
						else if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
						{
							CurrentRotation = Ring.ProceduralBand.BandRotation;
						}
						else
						{
							CurrentRotation = Ring.MeshRotation;
						}
					}
					else
					{
						// Mesh 선택은 MeshRotation 사용
						CurrentRotation = Ring.MeshRotation;
					}

					DragStartWorldRotation = BoneRotation * CurrentRotation;
					DragStartWorldRotation.Normalize();  // 정규화

					// 누적 델타 회전 초기화
					AccumulatedDeltaRotation = FQuat::Identity;

					bIsDraggingRotation = true;
				}
			}
		}
	}

	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

void FFleshRingEditorViewportClient::TrackingStopped()
{
	// 드래그 종료 시 트랜잭션 종료
	// 트랜잭션이 있었는지 먼저 체크 (Ring 수정 시에만 트랜잭션 생성됨)
	bool bHadTransaction = ScopedTransaction.IsValid();
	ScopedTransaction.Reset();
	bIsDraggingRotation = false;

	// 드래그 종료 시 에셋 dirty 마킹 (성능 최적화: 드래그 중에는 호출 안 함)
	// 카메라 이동 등 에셋을 수정하지 않는 경우는 제외
	if (bHadTransaction && EditingAsset.IsValid())
	{
		SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_MarkPackageDirty);
		EditingAsset->MarkPackageDirty();
	}

	FEditorViewportClient::TrackingStopped();
}

void FFleshRingEditorViewportClient::InvalidateAndDraw()
{
	Invalidate();

	// 드롭박스가 열려있을 때도 뷰포트 강제 렌더링
	if (Viewport)
	{
		Viewport->Draw();
	}
}

void FFleshRingEditorViewportClient::ToggleShowSkeletalMesh()
{
	bShowSkeletalMesh = !bShowSkeletalMesh;

	if (PreviewScene)
	{
		if (USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent())
		{
			SkelMeshComp->SetVisibility(bShowSkeletalMesh);
		}
	}

	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowRingMeshes()
{
	bShowRingMeshes = !bShowRingMeshes;

	// Ring 메시 컴포넌트 Visibility 토글
	if (PreviewScene)
	{
		PreviewScene->SetRingMeshesVisible(bShowRingMeshes);
	}

	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowGrid()
{
	// UAssetViewerSettings에서 현재 프로파일의 Grid 설정 토글
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings || !PreviewScene)
	{
		return;
	}

	int32 ProfileIndex = PreviewScene->GetCurrentProfileIndex();
	if (!Settings->Profiles.IsValidIndex(ProfileIndex))
	{
		return;
	}

	// 프로파일의 bShowGrid 토글
	Settings->Profiles[ProfileIndex].bShowGrid = !Settings->Profiles[ProfileIndex].bShowGrid;

	// 변경 알림 (다른 리스너들에게)
	Settings->OnAssetViewerSettingsChanged().Broadcast(GET_MEMBER_NAME_CHECKED(FPreviewSceneProfile, bShowGrid));

	// 즉시 적용
	ApplyPreviewSceneShowFlags();
	Invalidate();
}

bool FFleshRingEditorViewportClient::ShouldShowGrid() const
{
	// UAssetViewerSettings에서 현재 프로파일의 Grid 설정 반환
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings || !PreviewScene)
	{
		return false;
	}

	int32 ProfileIndex = PreviewScene->GetCurrentProfileIndex();
	if (!Settings->Profiles.IsValidIndex(ProfileIndex))
	{
		return false;
	}

	return Settings->Profiles[ProfileIndex].bShowGrid;
}

void FFleshRingEditorViewportClient::ApplyShowFlagsToScene()
{
	if (PreviewScene)
	{
		// 스켈레탈 메시 가시성 적용
		if (USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent())
		{
			SkelMeshComp->SetVisibility(bShowSkeletalMesh);
		}

		// Ring 메시 가시성 적용
		PreviewScene->SetRingMeshesVisible(bShowRingMeshes);
	}
}

void FFleshRingEditorViewportClient::OnAssetViewerSettingsChanged(const FName& PropertyName)
{
	// Preview Scene Settings 변경 시 ShowFlags 적용 후 뷰포트 갱신
	ApplyPreviewSceneShowFlags();
	Invalidate();
}

void FFleshRingEditorViewportClient::ApplyPreviewSceneShowFlags()
{
	if (!PreviewScene)
	{
		return;
	}

	// UAssetViewerSettings에서 현재 프로파일 설정 가져오기
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings)
	{
		return;
	}

	int32 ProfileIndex = PreviewScene->GetCurrentProfileIndex();
	if (!Settings->Profiles.IsValidIndex(ProfileIndex))
	{
		return;
	}

	const FPreviewSceneProfile& Profile = Settings->Profiles[ProfileIndex];

	// PostProcessing 적용 (먼저 처리해야 함 - DisableAdvancedFeatures가 다른 플래그 초기화)
	if (Profile.bPostProcessingEnabled)
	{
		EngineShowFlags.EnableAdvancedFeatures();
		EngineShowFlags.SetBloom(true);
	}
	else
	{
		EngineShowFlags.DisableAdvancedFeatures();
		EngineShowFlags.SetBloom(false);
	}

	// ShowFlags 적용
	EngineShowFlags.SetGrid(Profile.bShowGrid);
	EngineShowFlags.SetMeshEdges(Profile.bShowMeshEdges);
	EngineShowFlags.SetTonemapper(Profile.bEnableToneMapping);

	// DrawHelper의 Grid도 동기화
	DrawHelper.bDrawGrid = Profile.bShowGrid;
}

FString FFleshRingEditorViewportClient::GetConfigSectionName() const
{
	if (EditingAsset.IsValid())
	{
		// 에셋 경로를 섹션 이름에 포함 (예: FleshRingEditorViewport:/Game/FleshRings/MyAsset)
		return FString::Printf(TEXT("%s:%s"), *FleshRingViewportConfigSectionBase, *EditingAsset->GetPathName());
	}
	return FleshRingViewportConfigSectionBase;
}

void FFleshRingEditorViewportClient::SaveSettings()
{
	const FString SectionName = GetConfigSectionName();

	// === 뷰포트 타입 저장 ===
	GConfig->SetInt(*SectionName, TEXT("ViewportType"), static_cast<int32>(GetViewportType()), GEditorPerProjectIni);

	// === 원근 카메라 설정 저장 ===
	GConfig->SetVector(*SectionName, TEXT("PerspectiveViewLocation"), ViewTransformPerspective.GetLocation(), GEditorPerProjectIni);
	GConfig->SetRotator(*SectionName, TEXT("PerspectiveViewRotation"), ViewTransformPerspective.GetRotation(), GEditorPerProjectIni);

	// === 직교 카메라 설정 저장 ===
	GConfig->SetVector(*SectionName, TEXT("OrthographicViewLocation"), ViewTransformOrthographic.GetLocation(), GEditorPerProjectIni);
	GConfig->SetRotator(*SectionName, TEXT("OrthographicViewRotation"), ViewTransformOrthographic.GetRotation(), GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("OrthoZoom"), ViewTransformOrthographic.GetOrthoZoom(), GEditorPerProjectIni);

	// === 카메라 속도 저장 ===
	GConfig->SetFloat(*SectionName, TEXT("CameraSpeed"), GetCameraSpeedSettings().GetCurrentSpeed(), GEditorPerProjectIni);

	// === FOV 저장 ===
	GConfig->SetFloat(*SectionName, TEXT("ViewFOV"), ViewFOV, GEditorPerProjectIni);

	// === 클리핑 평면 저장 ===
	GConfig->SetFloat(*SectionName, TEXT("NearClipPlane"), GetNearClipPlane(), GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("FarClipPlane"), GetFarClipPlaneOverride(), GEditorPerProjectIni);

	// 직교 클리핑 평면
	TOptional<double> OrthoNear = GetOrthographicNearPlaneOverride();
	TOptional<double> OrthoFar = GetOrthographicFarPlaneOverride();
	GConfig->SetBool(*SectionName, TEXT("HasOrthoNearClip"), OrthoNear.IsSet(), GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("HasOrthoFarClip"), OrthoFar.IsSet(), GEditorPerProjectIni);
	if (OrthoNear.IsSet())
	{
		GConfig->SetDouble(*SectionName, TEXT("OrthoNearClipPlane"), OrthoNear.GetValue(), GEditorPerProjectIni);
	}
	if (OrthoFar.IsSet())
	{
		GConfig->SetDouble(*SectionName, TEXT("OrthoFarClipPlane"), OrthoFar.GetValue(), GEditorPerProjectIni);
	}

	// === 노출 설정 저장 ===
	GConfig->SetFloat(*SectionName, TEXT("ExposureFixedEV100"), ExposureSettings.FixedEV100, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ExposureBFixed"), ExposureSettings.bFixed, GEditorPerProjectIni);

	// === 뷰모드 저장 (Lit, Unlit, Wireframe 등) ===
	GConfig->SetInt(*SectionName, TEXT("ViewMode"), static_cast<int32>(GetViewMode()), GEditorPerProjectIni);

	// === 커스텀 Show 플래그 저장 ===
	GConfig->SetBool(*SectionName, TEXT("ShowSkeletalMesh"), bShowSkeletalMesh, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowRingGizmos"), bShowRingGizmos, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowRingMeshes"), bShowRingMeshes, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBones"), bShowBones, GEditorPerProjectIni);

	// 본 그리기 옵션 저장
	GConfig->SetBool(*SectionName, TEXT("ShowBoneNames"), bShowBoneNames, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowMultiColorBones"), bShowMultiColorBones, GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("BoneDrawSize"), BoneDrawSize, GEditorPerProjectIni);
	GConfig->SetInt(*SectionName, TEXT("BoneDrawMode"), static_cast<int32>(BoneDrawMode), GEditorPerProjectIni);

	// 디버그 시각화 옵션 저장
	GConfig->SetBool(*SectionName, TEXT("ShowDebugVisualization"), bCachedShowDebugVisualization, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowSdfVolume"), bCachedShowSdfVolume, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowAffectedVertices"), bCachedShowAffectedVertices, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowSDFSlice"), bCachedShowSDFSlice, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBulgeHeatmap"), bCachedShowBulgeHeatmap, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBulgeArrows"), bCachedShowBulgeArrows, GEditorPerProjectIni);
	GConfig->SetInt(*SectionName, TEXT("DebugSliceZ"), CachedDebugSliceZ, GEditorPerProjectIni);

	// Config 파일에 즉시 저장
	GConfig->Flush(false, GEditorPerProjectIni);
}

void FFleshRingEditorViewportClient::LoadSettings()
{
	const FString SectionName = GetConfigSectionName();

	// === 뷰포트 타입 로드 (가장 먼저 - 카메라 위치 적용 전에 타입 설정) ===
	int32 SavedViewportType = static_cast<int32>(ELevelViewportType::LVT_Perspective);
	if (GConfig->GetInt(*SectionName, TEXT("ViewportType"), SavedViewportType, GEditorPerProjectIni))
	{
		SetViewportType(static_cast<ELevelViewportType>(SavedViewportType));
	}

	// === 원근 카메라 설정 로드 ===
	bool bHasSavedPerspectiveLocation = false;
	FVector SavedPerspectiveLocation;
	if (GConfig->GetVector(*SectionName, TEXT("PerspectiveViewLocation"), SavedPerspectiveLocation, GEditorPerProjectIni))
	{
		ViewTransformPerspective.SetLocation(SavedPerspectiveLocation);
		bHasSavedPerspectiveLocation = true;
	}

	bool bHasSavedPerspectiveRotation = false;
	FRotator SavedPerspectiveRotation;
	if (GConfig->GetRotator(*SectionName, TEXT("PerspectiveViewRotation"), SavedPerspectiveRotation, GEditorPerProjectIni))
	{
		ViewTransformPerspective.SetRotation(SavedPerspectiveRotation);
		bHasSavedPerspectiveRotation = true;
	}

	// === 직교 카메라 설정 로드 ===
	bool bHasSavedOrthographicLocation = false;
	FVector SavedOrthographicLocation;
	if (GConfig->GetVector(*SectionName, TEXT("OrthographicViewLocation"), SavedOrthographicLocation, GEditorPerProjectIni))
	{
		ViewTransformOrthographic.SetLocation(SavedOrthographicLocation);
		bHasSavedOrthographicLocation = true;
	}

	bool bHasSavedOrthographicRotation = false;
	FRotator SavedOrthographicRotation;
	if (GConfig->GetRotator(*SectionName, TEXT("OrthographicViewRotation"), SavedOrthographicRotation, GEditorPerProjectIni))
	{
		ViewTransformOrthographic.SetRotation(SavedOrthographicRotation);
		bHasSavedOrthographicRotation = true;
	}

	float SavedOrthoZoom = DEFAULT_ORTHOZOOM;
	if (GConfig->GetFloat(*SectionName, TEXT("OrthoZoom"), SavedOrthoZoom, GEditorPerProjectIni))
	{
		ViewTransformOrthographic.SetOrthoZoom(SavedOrthoZoom);
	}

	// 저장된 카메라 위치를 실제 뷰에 적용 (엔진 재시작 시 카메라 복원)
	// FocusOnMesh()에서 시작된 카메라 보간 중단 (저장된 위치 우선)
	bool bHasSavedCameraSettings = false;
	if (GetViewportType() == LVT_Perspective)
	{
		if (bHasSavedPerspectiveLocation)
		{
			SetViewLocation(SavedPerspectiveLocation);
			bHasSavedCameraSettings = true;
		}
		if (bHasSavedPerspectiveRotation)
		{
			SetViewRotation(SavedPerspectiveRotation);
			bHasSavedCameraSettings = true;
		}
	}
	else
	{
		if (bHasSavedOrthographicLocation)
		{
			SetViewLocation(SavedOrthographicLocation);
			bHasSavedCameraSettings = true;
		}
		if (bHasSavedOrthographicRotation)
		{
			SetViewRotation(SavedOrthographicRotation);
			bHasSavedCameraSettings = true;
		}
	}

	// 저장된 설정이 있으면 FocusOnMesh() 보간 중단
	if (bHasSavedCameraSettings)
	{
		bIsCameraInterpolating = false;
	}

	// === 카메라 속도 로드 ===
	float SavedCameraSpeed = 1.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("CameraSpeed"), SavedCameraSpeed, GEditorPerProjectIni))
	{
		FEditorViewportCameraSpeedSettings SpeedSettings = GetCameraSpeedSettings();
		SpeedSettings.SetCurrentSpeed(SavedCameraSpeed);
		SetCameraSpeedSettings(SpeedSettings);
	}

	// === FOV 로드 ===
	float SavedFOV = 90.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("ViewFOV"), SavedFOV, GEditorPerProjectIni))
	{
		ViewFOV = SavedFOV;
	}

	// === 클리핑 평면 로드 ===
	float SavedNearClip = 1.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("NearClipPlane"), SavedNearClip, GEditorPerProjectIni))
	{
		OverrideNearClipPlane(SavedNearClip);
	}

	float SavedFarClip = 0.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("FarClipPlane"), SavedFarClip, GEditorPerProjectIni))
	{
		if (SavedFarClip > 0.0f)
		{
			OverrideFarClipPlane(SavedFarClip);
		}
	}

	// 직교 클리핑 평면 로드
	bool bHasOrthoNearClip = false;
	bool bHasOrthoFarClip = false;
	GConfig->GetBool(*SectionName, TEXT("HasOrthoNearClip"), bHasOrthoNearClip, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("HasOrthoFarClip"), bHasOrthoFarClip, GEditorPerProjectIni);

	if (bHasOrthoNearClip)
	{
		double OrthoNearValue = 0.0;
		if (GConfig->GetDouble(*SectionName, TEXT("OrthoNearClipPlane"), OrthoNearValue, GEditorPerProjectIni))
		{
			SetOrthographicNearPlaneOverride(OrthoNearValue);
		}
	}

	if (bHasOrthoFarClip)
	{
		double OrthoFarValue = 0.0;
		if (GConfig->GetDouble(*SectionName, TEXT("OrthoFarClipPlane"), OrthoFarValue, GEditorPerProjectIni))
		{
			SetOrthographicFarPlaneOverride(OrthoFarValue);
		}
	}

	// === 노출 설정 로드 ===
	GConfig->GetFloat(*SectionName, TEXT("ExposureFixedEV100"), ExposureSettings.FixedEV100, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ExposureBFixed"), ExposureSettings.bFixed, GEditorPerProjectIni);

	// === 뷰모드 로드 (Lit, Unlit, Wireframe 등) ===
	// 주의: ApplyPreviewSceneShowFlags()가 뷰모드를 덮어쓰므로 나중에 다시 적용
	EViewModeIndex LoadedViewMode = VMI_Lit;
	bool bHasSavedViewMode = false;
	int32 SavedViewMode = static_cast<int32>(VMI_Lit);
	if (GConfig->GetInt(*SectionName, TEXT("ViewMode"), SavedViewMode, GEditorPerProjectIni))
	{
		LoadedViewMode = static_cast<EViewModeIndex>(SavedViewMode);
		bHasSavedViewMode = true;
	}

	// === 커스텀 Show 플래그 로드 ===
	GConfig->GetBool(*SectionName, TEXT("ShowSkeletalMesh"), bShowSkeletalMesh, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowRingGizmos"), bShowRingGizmos, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowRingMeshes"), bShowRingMeshes, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBones"), bShowBones, GEditorPerProjectIni);

	// 본 그리기 옵션 로드
	GConfig->GetBool(*SectionName, TEXT("ShowBoneNames"), bShowBoneNames, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowMultiColorBones"), bShowMultiColorBones, GEditorPerProjectIni);
	GConfig->GetFloat(*SectionName, TEXT("BoneDrawSize"), BoneDrawSize, GEditorPerProjectIni);
	int32 BoneDrawModeInt = static_cast<int32>(EFleshRingBoneDrawMode::All);
	GConfig->GetInt(*SectionName, TEXT("BoneDrawMode"), BoneDrawModeInt, GEditorPerProjectIni);
	BoneDrawMode = static_cast<EFleshRingBoneDrawMode::Type>(FMath::Clamp(BoneDrawModeInt, 0, 5));

	// 디버그 시각화 옵션 로드
	GConfig->GetBool(*SectionName, TEXT("ShowDebugVisualization"), bCachedShowDebugVisualization, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowSdfVolume"), bCachedShowSdfVolume, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowAffectedVertices"), bCachedShowAffectedVertices, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowSDFSlice"), bCachedShowSDFSlice, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBulgeHeatmap"), bCachedShowBulgeHeatmap, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBulgeArrows"), bCachedShowBulgeArrows, GEditorPerProjectIni);
	GConfig->GetInt(*SectionName, TEXT("DebugSliceZ"), CachedDebugSliceZ, GEditorPerProjectIni);

	// 캐싱된 값을 FleshRingComponent에 적용
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowDebugVisualization = bCachedShowDebugVisualization;
			Comp->bShowSdfVolume = bCachedShowSdfVolume;
			Comp->bShowAffectedVertices = bCachedShowAffectedVertices;
			Comp->bShowSDFSlice = bCachedShowSDFSlice;
			Comp->bShowBulgeHeatmap = bCachedShowBulgeHeatmap;
			Comp->bShowBulgeArrows = bCachedShowBulgeArrows;
			Comp->DebugSliceZ = CachedDebugSliceZ;

			// SDFSlice 평면 가시성 적용
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
		}
	}

	// 로드된 Show Flag를 PreviewScene에 적용
	ApplyShowFlagsToScene();

	// Preview Scene Settings도 적용
	ApplyPreviewSceneShowFlags();

	// 뷰모드 재적용 (ApplyPreviewSceneShowFlags()가 ShowFlags를 덮어쓰므로)
	if (bHasSavedViewMode)
	{
		SetViewMode(LoadedViewMode);
	}
}

void FFleshRingEditorViewportClient::ToggleShowDebugVisualization()
{
	bCachedShowDebugVisualization = !bCachedShowDebugVisualization;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowDebugVisualization = bCachedShowDebugVisualization;
			// 평면 액터 즉시 숨기기/보이기
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
		}
	}
	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowSdfVolume()
{
	bCachedShowSdfVolume = !bCachedShowSdfVolume;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowSdfVolume = bCachedShowSdfVolume;
		}
	}
	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowAffectedVertices()
{
	bCachedShowAffectedVertices = !bCachedShowAffectedVertices;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowAffectedVertices = bCachedShowAffectedVertices;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowDebugVisualization() const
{
	return bCachedShowDebugVisualization;
}

bool FFleshRingEditorViewportClient::ShouldShowSdfVolume() const
{
	return bCachedShowSdfVolume;
}

bool FFleshRingEditorViewportClient::ShouldShowAffectedVertices() const
{
	return bCachedShowAffectedVertices;
}

void FFleshRingEditorViewportClient::ToggleShowSDFSlice()
{
	bCachedShowSDFSlice = !bCachedShowSDFSlice;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowSDFSlice = bCachedShowSDFSlice;
			// 평면 액터 즉시 숨기기/보이기
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowSDFSlice() const
{
	return bCachedShowSDFSlice;
}

int32 FFleshRingEditorViewportClient::GetDebugSliceZ() const
{
	return CachedDebugSliceZ;
}

void FFleshRingEditorViewportClient::SetDebugSliceZ(int32 NewValue)
{
	CachedDebugSliceZ = NewValue;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->DebugSliceZ = NewValue;
		}
	}
	Invalidate();
}

void FFleshRingEditorViewportClient::ToggleShowBulgeHeatmap()
{
	bCachedShowBulgeHeatmap = !bCachedShowBulgeHeatmap;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowBulgeHeatmap = bCachedShowBulgeHeatmap;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowBulgeHeatmap() const
{
	return bCachedShowBulgeHeatmap;
}

void FFleshRingEditorViewportClient::ToggleShowBulgeArrows()
{
	bCachedShowBulgeArrows = !bCachedShowBulgeArrows;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowBulgeArrows = bCachedShowBulgeArrows;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowBulgeArrows() const
{
	return bCachedShowBulgeArrows;
}

void FFleshRingEditorViewportClient::SetBoneDrawMode(EFleshRingBoneDrawMode::Type InMode)
{
	BoneDrawMode = InMode;
	UpdateBonesToDraw();
	Invalidate();
}

void FFleshRingEditorViewportClient::UpdateBonesToDraw()
{
	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	if (NumBones == 0)
	{
		BonesToDraw.Empty();
		return;
	}

	// 부모 인덱스 배열 생성
	TArray<int32> ParentIndices;
	ParentIndices.SetNum(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		ParentIndices[BoneIndex] = RefSkeleton.GetParentIndex(BoneIndex);
	}

	// 선택된 본 배열 생성
	TArray<int32> SelectedBones;
	if (!SelectedBoneName.IsNone())
	{
		int32 SelectedBoneIndex = RefSkeleton.FindBoneIndex(SelectedBoneName);
		if (SelectedBoneIndex != INDEX_NONE)
		{
			SelectedBones.Add(SelectedBoneIndex);
		}
	}

	// EFleshRingBoneDrawMode를 EBoneDrawMode로 변환
	EBoneDrawMode::Type EngineBoneDrawMode = EBoneDrawMode::All;
	switch (BoneDrawMode)
	{
	case EFleshRingBoneDrawMode::None:
		EngineBoneDrawMode = EBoneDrawMode::None;
		break;
	case EFleshRingBoneDrawMode::Selected:
		EngineBoneDrawMode = EBoneDrawMode::Selected;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParents:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParents;
		break;
	case EFleshRingBoneDrawMode::SelectedAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndChildren;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParentsAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParentsAndChildren;
		break;
	case EFleshRingBoneDrawMode::All:
	default:
		EngineBoneDrawMode = EBoneDrawMode::All;
		break;
	}

	// SkeletalDebugRendering의 함수를 사용하여 그릴 본 계산
	SkeletalDebugRendering::CalculateBonesToDraw(
		ParentIndices,
		SelectedBones,
		EngineBoneDrawMode,
		BonesToDraw);
}

// ============================================================================
// 우클릭 컨텍스트 메뉴 관련 함수
// ============================================================================

void FFleshRingEditorViewportClient::ShowBoneContextMenu(FName BoneName, const FVector2D& ScreenPos)
{
	// 가중치 본 캐시가 비어있으면 빌드
	if (WeightedBoneIndices.Num() == 0)
	{
		BuildWeightedBoneCache();
	}

	// 선택된 본의 인덱스 가져오기
	int32 BoneIndex = INDEX_NONE;
	if (PreviewScene)
	{
		if (UDebugSkelMeshComponent* SkelComp = PreviewScene->GetSkeletalMeshComponent())
		{
			if (USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset())
			{
				BoneIndex = SkelMesh->GetRefSkeleton().FindBoneIndex(BoneName);
			}
		}
	}

	// Ring 추가 가능 조건:
	// 1. 가중치 있는 자식 본이 있거나 (방향 자동 계산)
	// 2. 말단 본이지만 자기 자신이 가중치가 있는 경우 (기본값 사용)
	bool bHasWeightedChild = (FindWeightedChildBone(BoneIndex) != INDEX_NONE);
	bool bIsSelfWeighted = IsBoneWeighted(BoneIndex);
	bool bCanAddRing = (BoneIndex != INDEX_NONE) && (bHasWeightedChild || bIsSelfWeighted);

	FMenuBuilder MenuBuilder(true, nullptr);

	MenuBuilder.BeginSection("BoneActions", NSLOCTEXT("FleshRingEditor", "BoneActionsSection", "Bone"));
	{
		// Add Ring 메뉴 - 가중치가 있는 자식이 있을 때만 활성화
		MenuBuilder.AddMenuEntry(
			NSLOCTEXT("FleshRingEditor", "AddRingAtPosition", "Add Ring Here..."),
			bCanAddRing
				? NSLOCTEXT("FleshRingEditor", "AddRingAtPositionTooltip", "Select a mesh and add a ring at clicked position")
				: NSLOCTEXT("FleshRingEditor", "AddRingAtPositionDisabledTooltip", "Cannot add ring: This bone has no weighted child bones"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
			FUIAction(
				FExecuteAction::CreateRaw(this, &FFleshRingEditorViewportClient::OnContextMenu_AddRing),
				FCanExecuteAction::CreateLambda([bCanAddRing]() { return bCanAddRing; })
			)
		);

		// Copy Bone Name 메뉴
		MenuBuilder.AddMenuEntry(
			NSLOCTEXT("FleshRingEditor", "CopyBoneName", "Copy Bone Name"),
			FText::GetEmpty(),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
			FUIAction(
				FExecuteAction::CreateLambda([BoneName]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*BoneName.ToString());
				})
			)
		);
	}
	MenuBuilder.EndSection();

	// 메뉴 표시 - 커서 위치에 표시 (ScreenPos는 뷰포트 로컬 좌표이므로 커서 위치 사용)
	TSharedPtr<SWidget> MenuWidget = MenuBuilder.MakeWidget();

	if (TSharedPtr<SFleshRingEditorViewport> ViewportPtr = ViewportWidget.Pin())
	{
		FSlateApplication::Get().PushMenu(
			ViewportPtr.ToSharedRef(),
			FWidgetPath(),
			MenuWidget.ToSharedRef(),
			FSlateApplication::Get().GetCursorPos(),
			FPopupTransitionEffect::ContextMenu
		);
	}
}

void FFleshRingEditorViewportClient::OnContextMenu_AddRing()
{
	if (PendingRingAddBoneName.IsNone())
	{
		return;
	}

	// 에셋 피커 설정
	FAssetPickerConfig AssetPickerConfig;
	AssetPickerConfig.Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	AssetPickerConfig.Filter.bRecursiveClasses = true;
	AssetPickerConfig.SelectionMode = ESelectionMode::Single;
	AssetPickerConfig.bAllowNullSelection = false;  // 버튼으로 처리하므로 비활성화
	AssetPickerConfig.bFocusSearchBoxWhenOpened = true;
	AssetPickerConfig.InitialAssetViewType = EAssetViewType::List;

	// 메쉬 선택 시 콜백 - 멤버 변수를 캡처하기 위해 람다 사용
	FName CapturedBoneName = PendingRingAddBoneName;
	FVector2D CapturedScreenPos = PendingRingAddScreenPos;

	AssetPickerConfig.OnAssetSelected = FOnAssetSelected::CreateLambda(
		[this, CapturedBoneName, CapturedScreenPos](const FAssetData& AssetData)
		{
			// 팝업 닫기
			FSlateApplication::Get().DismissAllMenus();

			UStaticMesh* SelectedMesh = nullptr;
			if (AssetData.IsValid())
			{
				SelectedMesh = Cast<UStaticMesh>(AssetData.GetAsset());
			}

			// 본 로컬 오프셋 및 회전 계산 (본 축 라인에 투영된 위치, 녹색 라인 방향)
			FRotator LocalRotation;
			FVector LocalOffset = CalculateBoneLocalOffsetFromScreenPos(CapturedScreenPos, CapturedBoneName, &LocalRotation);

			// Ring 추가 요청
			OnAddRingAtPositionRequested.ExecuteIfBound(
				CapturedBoneName,
				LocalOffset,
				LocalRotation,
				SelectedMesh
			);
		}
	);

	// 상태 초기화 (에셋 피커 열기 전에)
	PendingRingAddBoneName = NAME_None;

	// 에셋 피커 팝업 표시
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TSharedRef<SWidget> AssetPickerWidget = ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig);

	// 팝업 메뉴로 표시
	if (TSharedPtr<SFleshRingEditorViewport> ViewportPtr = ViewportWidget.Pin())
	{
		// 하단 버튼 바 포함 팝업 (다이얼로그 스타일)
		FSlateApplication::Get().PushMenu(
			ViewportPtr.ToSharedRef(),
			FWidgetPath(),
			SNew(SBox)
				.WidthOverride(400.0f)
				.HeightOverride(500.0f)
				[
					SNew(SVerticalBox)
					// 에셋 피커 (상단)
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						AssetPickerWidget
					]
					// 구분선
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f)
					[
						SNew(SSeparator)
					]
					// 버튼 바 (하단)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(8.0f, 4.0f, 8.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						// 왼쪽 여백
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(SButton)
							.Text(NSLOCTEXT("FleshRingEditor", "SkipMesh", "Skip Mesh"))
							.ToolTipText(NSLOCTEXT("FleshRingEditor", "SkipMeshTooltip", "Add ring without mesh"))
							.OnClicked_Lambda([this, CapturedBoneName, CapturedScreenPos]()
							{
								FSlateApplication::Get().DismissAllMenus();

								// 본 로컬 오프셋 및 회전 계산
								FRotator LocalRotation;
								FVector LocalOffset = CalculateBoneLocalOffsetFromScreenPos(CapturedScreenPos, CapturedBoneName, &LocalRotation);

								// 메쉬 없이 Ring 추가 요청
								OnAddRingAtPositionRequested.ExecuteIfBound(
									CapturedBoneName,
									LocalOffset,
									LocalRotation,
									nullptr
								);
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(NSLOCTEXT("FleshRingEditor", "Cancel", "Cancel"))
							.OnClicked_Lambda([]()
							{
								FSlateApplication::Get().DismissAllMenus();
								return FReply::Handled();
							})
						]
					]
				],
			FSlateApplication::Get().GetCursorPos(),
			FPopupTransitionEffect::ContextMenu
		);
	}
}

FVector FFleshRingEditorViewportClient::CalculateBoneLocalOffsetFromScreenPos(const FVector2D& ScreenPos, FName BoneName, FRotator* OutLocalRotation)
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FVector::ZeroVector;
	}

	// 본 트랜스폼 가져오기
	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return FVector::ZeroVector;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMeshComponent->GetSkeletalMeshAsset()->GetRefSkeleton();
	int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
	FVector BoneOrigin = BoneTransform.GetLocation();

	// 가중치 본 캐시가 비어있으면 빌드
	if (WeightedBoneIndices.Num() == 0)
	{
		const_cast<FFleshRingEditorViewportClient*>(this)->BuildWeightedBoneCache();
	}

	// 본 축 방향 계산: 현재 본에서 가중치가 있는 자식 본으로 향하는 방향 사용
	FVector BoneAxisDir;

	// 가중치가 있는 자식 본 개수 확인
	int32 WeightedChildCount = CountWeightedChildBones(BoneIndex);

	if (WeightedChildCount == 1)
	{
		// 가중치가 있는 자식 본이 하나만 있을 때: 자동 방향 계산
		int32 WeightedChildIndex = FindWeightedChildBone(BoneIndex);
		FVector ChildLocation = SkeletalMeshComponent->GetBoneTransform(WeightedChildIndex).GetLocation();
		BoneAxisDir = (ChildLocation - BoneOrigin).GetSafeNormal();
	}
	else if (WeightedChildCount >= 2)
	{
		// 가중치가 있는 자식 본이 여러 개: 기본 회전값 사용 (방향이 모호함)
		if (OutLocalRotation)
		{
			*OutLocalRotation = FRotator(-90.0f, 0.0f, 0.0f);
		}
		return FVector::ZeroVector;
	}
	else
	{
		// 말단 본 (자식 없음): 기본 회전값 사용
		if (OutLocalRotation)
		{
			*OutLocalRotation = FRotator(-90.0f, 0.0f, 0.0f);
		}
		return FVector::ZeroVector;
	}

	// 스크린 좌표 → 월드 레이 변환
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		Viewport,
		GetScene(),
		EngineShowFlags)
		.SetTime(FGameTime::GetTimeSinceAppStart()));

	FSceneView* View = CalcSceneView(&ViewFamily);
	if (!View)
	{
		return FVector::ZeroVector;
	}

	FVector RayOrigin, RayDirection;
	View->DeprojectFVector2D(ScreenPos, RayOrigin, RayDirection);

	// 레이와 본 축 사이의 최근접점 계산
	// 두 직선 사이의 최근접점을 구하는 공식
	FVector W0 = BoneOrigin - RayOrigin;
	float A = FVector::DotProduct(BoneAxisDir, BoneAxisDir);  // 항상 1 (정규화됨)
	float B = FVector::DotProduct(BoneAxisDir, RayDirection);
	float C = FVector::DotProduct(RayDirection, RayDirection); // 항상 1 (정규화됨)
	float D = FVector::DotProduct(BoneAxisDir, W0);
	float E = FVector::DotProduct(RayDirection, W0);

	float Denom = A * C - B * B;

	float T_BoneAxis;
	if (FMath::Abs(Denom) < SMALL_NUMBER)
	{
		// 두 직선이 평행한 경우 - 본 원점 사용
		T_BoneAxis = 0.0f;
	}
	else
	{
		// 본 축 위의 최근접점 파라미터
		T_BoneAxis = (B * E - C * D) / Denom;
	}

	// 녹색 라인 방향으로의 월드 오프셋 계산
	// BoneAxisDir = 현재→자식 본 방향 (녹색 라인)
	FVector WorldOffset = BoneAxisDir * T_BoneAxis;

	// 월드 오프셋을 본 로컬 좌표계로 변환
	// 이렇게 해야 RingOffset이 녹색 라인 위에 정확히 정렬됨
	FVector LocalOffset = BoneTransform.GetRotation().UnrotateVector(WorldOffset);

	// 회전 계산: 녹색 라인 방향이 Ring의 Z축이 되도록
	if (OutLocalRotation)
	{
		// 녹색 라인 방향을 본 로컬 공간으로 변환
		FVector LocalAxisDir = BoneTransform.GetRotation().UnrotateVector(BoneAxisDir);

		// Z축(0,0,1)에서 녹색 라인 방향으로 회전하는 쿼터니언 계산
		FQuat RotationQuat = FQuat::FindBetweenNormals(FVector::UpVector, LocalAxisDir);
		*OutLocalRotation = RotationQuat.Rotator();
	}

	return LocalOffset;
}

FRotator FFleshRingEditorViewportClient::CalculateDefaultRingRotationForBone(FName BoneName)
{
	const FRotator DefaultRotation(-90.0f, 0.0f, 0.0f);

	if (!PreviewScene || BoneName.IsNone())
	{
		return DefaultRotation;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return DefaultRotation;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return DefaultRotation;
	}

	int32 BoneIndex = SkelMesh->GetRefSkeleton().FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return DefaultRotation;
	}

	// 가중치 본 캐시가 비어있으면 빌드
	if (WeightedBoneIndices.Num() == 0)
	{
		const_cast<FFleshRingEditorViewportClient*>(this)->BuildWeightedBoneCache();
	}

	// 가중치가 있는 자식 본 개수 확인
	int32 WeightedChildCount = CountWeightedChildBones(BoneIndex);

	if (WeightedChildCount == 1)
	{
		// 가중치가 있는 자식 본이 하나만 있을 때: 자동 방향 계산
		int32 WeightedChildIndex = FindWeightedChildBone(BoneIndex);
		FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
		FVector BoneOrigin = BoneTransform.GetLocation();
		FVector ChildLocation = SkeletalMeshComponent->GetBoneTransform(WeightedChildIndex).GetLocation();
		FVector BoneAxisDir = (ChildLocation - BoneOrigin).GetSafeNormal();

		// 녹색 라인 방향을 본 로컬 공간으로 변환
		FVector LocalAxisDir = BoneTransform.GetRotation().UnrotateVector(BoneAxisDir);

		// Z축(0,0,1)에서 녹색 라인 방향으로 회전하는 쿼터니언 계산
		FQuat RotationQuat = FQuat::FindBetweenNormals(FVector::UpVector, LocalAxisDir);
		return RotationQuat.Rotator();
	}

	// 가중치가 있는 자식 본이 0개 또는 2개 이상: 기본값 사용
	return DefaultRotation;
}

void FFleshRingEditorViewportClient::BuildWeightedBoneCache()
{
	WeightedBoneIndices.Empty();

	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return;
	}

	// LOD 0의 렌더 데이터에서 웨이팅된 본 찾기
	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];

	// 각 섹션의 BoneMap에 있는 본들이 웨이팅된 본들
	for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
	{
		for (FBoneIndexType BoneIndex : Section.BoneMap)
		{
			WeightedBoneIndices.Add(BoneIndex);
		}
	}
}

bool FFleshRingEditorViewportClient::IsBoneWeighted(int32 BoneIndex) const
{
	return WeightedBoneIndices.Contains(BoneIndex);
}

int32 FFleshRingEditorViewportClient::FindWeightedChildBone(int32 ParentBoneIndex) const
{
	if (!PreviewScene)
	{
		return INDEX_NONE;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return INDEX_NONE;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return INDEX_NONE;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();

	// 자식 본 중에서 가중치가 있는 본 찾기
	for (int32 i = 0; i < RefSkeleton.GetNum(); ++i)
	{
		if (RefSkeleton.GetParentIndex(i) == ParentBoneIndex)
		{
			if (IsBoneWeighted(i))
			{
				return i;
			}
		}
	}

	return INDEX_NONE;
}

int32 FFleshRingEditorViewportClient::CountWeightedChildBones(int32 ParentBoneIndex) const
{
	if (!PreviewScene)
	{
		return 0;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return 0;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return 0;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();

	int32 Count = 0;
	for (int32 i = 0; i < RefSkeleton.GetNum(); ++i)
	{
		if (RefSkeleton.GetParentIndex(i) == ParentBoneIndex)
		{
			if (IsBoneWeighted(i))
			{
				++Count;
			}
		}
	}

	return Count;
}

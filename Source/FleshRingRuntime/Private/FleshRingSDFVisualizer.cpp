// FleshRingSDFVisualizer.cpp
#include "FleshRingSDFVisualizer.h"
#include "FleshRingSDF.h"
#include "FleshRingMeshExtractor.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

FSDFVisualizationResult UFleshRingSDFVisualizer::VisualizeSDFSlice(
    UObject* WorldContextObject,
    UStaticMesh* Mesh,
    FVector WorldLocation,
    int32 SliceZ /*= 32*/,
    int32 Resolution /*= 64 */)
{
    FSDFVisualizationResult Result;

    if (!WorldContextObject || !Mesh)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Invalid parameters"));
        return Result;
    }

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Could not get world"));
        return Result;
    }

    // 1. 메시 데이터 추출
    FFleshRingMeshData MeshData;
    if (!UFleshRingMeshExtractor::ExtractMeshData(Mesh, MeshData))
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Failed to extract mesh data"));
        return Result;
    }

    // 2. 바운딩 박스 계산 (마진 추가)
    FVector3f BoundsSize = MeshData.Bounds.Max - MeshData.Bounds.Min;
    FVector3f Margin = BoundsSize * 0.1f;
    FVector3f BoundsMin = MeshData.Bounds.Min - Margin;
    FVector3f BoundsMax = MeshData.Bounds.Max + Margin;

    Result.BoundsMin = FVector(BoundsMin);
    Result.BoundsMax = FVector(BoundsMax);
    Result.CurrentSliceZ = FMath::Clamp(SliceZ, 0, Resolution - 1);
    Result.Resolution = Resolution;

    // 3. 렌더 타겟 생성
    Result.SliceTexture = NewObject<UTextureRenderTarget2D>(WorldContextObject);
    Result.SliceTexture->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
    Result.SliceTexture->UpdateResourceImmediate(true);

    // 4. 평면 액터 스폰
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AActor* PlaneActor = World->SpawnActor<AActor>(AActor::StaticClass(), WorldLocation, FRotator::ZeroRotator, SpawnParams);
    if (!PlaneActor)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Failed to spawn plane actor"));
        return Result;
    }

    // 루트 컴포넌트 설정
    USceneComponent* RootComp = NewObject<USceneComponent>(PlaneActor, TEXT("Root"));
    PlaneActor->SetRootComponent(RootComp);
    RootComp->RegisterComponent();

    // 평면 메시 컴포넌트 추가
    UStaticMeshComponent* PlaneMeshComp = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("PlaneMesh"));
    PlaneMeshComp->SetupAttachment(RootComp);

    // 엔진 기본 평면 메시 사용
    UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (PlaneMesh)
    {
        PlaneMeshComp->SetStaticMesh(PlaneMesh);
    }

    // 평면 스케일 조정 - 메시 바운드 크기에 맞춤
    FVector3f BoundsSizeWithMargin = BoundsMax - BoundsMin;
    float PlaneScaleX = BoundsSizeWithMargin.X / 100.0f;  // 기본 평면은 100x100 유닛
    float PlaneScaleY = BoundsSizeWithMargin.Y / 100.0f;
    PlaneMeshComp->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));

    // Z 슬라이스 위치 계산 (로컬 바운드 + 월드 오프셋)
    float LocalSliceZ = FMath::Lerp(
        BoundsMin.Z,
        BoundsMax.Z,
        (float)Result.CurrentSliceZ / (float)(Resolution - 1)
    );
    // 월드 위치 = 메시 로컬 바운드 중심 + 월드 오프셋
    FVector PlaneCenter(
        WorldLocation.X + (BoundsMin.X + BoundsMax.X) * 0.5f,
        WorldLocation.Y + (BoundsMin.Y + BoundsMax.Y) * 0.5f,
        WorldLocation.Z + LocalSliceZ
    );
    PlaneMeshComp->SetWorldLocation(PlaneCenter);

    // 5. 머티리얼 생성 및 적용
    // Widget3DPassThrough 머티리얼 사용 (텍스처 파라미터 지원)
    UMaterial* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
    if (!BaseMaterial)
    {
        // 폴백: 기본 머티리얼
        BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    }
    UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, PlaneActor);

    // 렌더 타겟을 텍스처 파라미터로 설정
    if (DynMaterial && Result.SliceTexture)
    {
        DynMaterial->SetTextureParameterValue(TEXT("SlateUI"), Result.SliceTexture);
    }

    PlaneMeshComp->SetMaterial(0, DynMaterial);
    PlaneMeshComp->RegisterComponent();

    // 6. 뒷면용 평면 추가 (양면 렌더링)
    UStaticMeshComponent* BackPlaneMeshComp = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("BackPlaneMesh"));
    BackPlaneMeshComp->SetupAttachment(RootComp);
    if (PlaneMesh)
    {
        BackPlaneMeshComp->SetStaticMesh(PlaneMesh);
    }
    BackPlaneMeshComp->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));
    BackPlaneMeshComp->SetWorldLocation(PlaneCenter);
    BackPlaneMeshComp->SetWorldRotation(FRotator(180.0f, 0.0f, 0.0f));  // X축 기준 180도 회전 (뒤집기)
    BackPlaneMeshComp->SetMaterial(0, DynMaterial);
    BackPlaneMeshComp->RegisterComponent();

    Result.PlaneActor = PlaneActor;
    
    UE_LOG(LogTemp, Warning, TEXT("Plane spawned at: %s, Scale: (%.2f, %.2f)"),
        *PlaneCenter.ToString(), PlaneScaleX, PlaneScaleY);

    // 6. GPU에서 SDF 생성 + 슬라이스 시각화
    TArray<FVector3f> Vertices = MeshData.Vertices;
    TArray<uint32> Indices = MeshData.Indices;
    FIntVector SDFResolution(Resolution, Resolution, Resolution);
    int32 CapturedSliceZ = Result.CurrentSliceZ;
    float MaxDisplayDist = BoundsSize.GetMax() * 0.5f;
    UTextureRenderTarget2D* RenderTarget = Result.SliceTexture;

    ENQUEUE_RENDER_COMMAND(GenerateSDFAndSlice)(
        [Vertices, Indices, BoundsMin, BoundsMax, SDFResolution, CapturedSliceZ, MaxDisplayDist, RenderTarget](FRHICommandListImmediate& RHICmdList)
        {
            if (!RenderTarget)
            {
                UE_LOG(LogTemp, Error, TEXT("RenderTarget is null"));
                return;
            }

            FTextureRenderTargetResource* RTResource = RenderTarget->GetRenderTargetResource();
            if (!RTResource)
            {
                UE_LOG(LogTemp, Error, TEXT("RenderTargetResource is null"));
                return;
            }

            FRDGBuilder GraphBuilder(RHICmdList);

            // SDF 3D 텍스처 생성 (Ray Casting으로 부호 결정됨)
            FRDGTextureDesc SDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef SDFTexture = GraphBuilder.CreateTexture(SDFDesc, TEXT("SDFTexture"));

            // SDF 생성 (Multiple Ray Casting으로 부호 결정)
            GenerateMeshSDF(
                GraphBuilder,
                SDFTexture,
                Vertices,
                Indices,
                BoundsMin,
                BoundsMax,
                SDFResolution
            );

            // 2D Slice Flood Fill로 도넛홀 보정
            FRDGTextureDesc CorrectedSDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef CorrectedSDF = GraphBuilder.CreateTexture(CorrectedSDFDesc, TEXT("CorrectedSDFTexture"));

            Apply2DSliceFloodFill(
                GraphBuilder,
                SDFTexture,
                CorrectedSDF,
                SDFResolution
            );

            // 슬라이스 2D 텍스처 생성
            FRDGTextureDesc SliceDesc = FRDGTextureDesc::Create2D(
                FIntPoint(SDFResolution.X, SDFResolution.Y),
                PF_B8G8R8A8,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
            );
            FRDGTextureRef SliceTexture = GraphBuilder.CreateTexture(SliceDesc, TEXT("SDFSliceTexture"));

            // 슬라이스 시각화 (보정된 SDF 사용)
            GenerateSDFSlice(
                GraphBuilder,
                CorrectedSDF,
                SliceTexture,
                SDFResolution,
                CapturedSliceZ,
                MaxDisplayDist
            );

            // 외부 렌더 타겟 등록 및 복사
            FRHITexture* DestRHI = RTResource->GetRenderTargetTexture();
            if (DestRHI)
            {
                FRDGTextureRef DestTexture = GraphBuilder.RegisterExternalTexture(
                    CreateRenderTarget(DestRHI, TEXT("DestRenderTarget"))
                );
                AddCopyTexturePass(GraphBuilder, SliceTexture, DestTexture);
            }

            GraphBuilder.Execute();

            UE_LOG(LogTemp, Warning, TEXT("SDF Slice Visualization Generated: Z=%d"), CapturedSliceZ);
        }
    );

    UE_LOG(LogTemp, Warning, TEXT("VisualizeSDFSlice: Created visualization at Z=%d"), Result.CurrentSliceZ);
    return Result;
}

void UFleshRingSDFVisualizer::UpdateSliceZ(FSDFVisualizationResult& Result, int32 NewSliceZ)
{
    if (!Result.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("UpdateSliceZ: Invalid result"));
        return;
    }

    Result.CurrentSliceZ = FMath::Clamp(NewSliceZ, 0, Result.Resolution - 1);

    // TODO: SDF를 캐시하고 슬라이스만 업데이트
    // 현재는 전체 재생성이 필요함
    UE_LOG(LogTemp, Warning, TEXT("UpdateSliceZ: Updated to Z=%d (requires re-visualization for now)"), Result.CurrentSliceZ);
}

void UFleshRingSDFVisualizer::CleanupVisualization(FSDFVisualizationResult& Result)
{
    if (Result.PlaneActor)
    {
        Result.PlaneActor->Destroy();
        Result.PlaneActor = nullptr;
    }

    if (Result.SliceTexture)
    {
        Result.SliceTexture = nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("CleanupVisualization: Cleaned up"));
}

TArray<FSDFVisualizationResult> UFleshRingSDFVisualizer::VisualizeAllSDFSlices(
    UObject* WorldContextObject,
    UStaticMesh* Mesh,
    FVector WorldLocation,
    int32 Resolution)
{
    TArray<FSDFVisualizationResult> Results;

    if (!WorldContextObject || !Mesh)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeAllSDFSlices: Invalid parameters"));
        return Results;
    }

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeAllSDFSlices: Could not get world"));
        return Results;
    }

    // 1. 메시 데이터 추출 (1회만)
    FFleshRingMeshData MeshData;
    if (!UFleshRingMeshExtractor::ExtractMeshData(Mesh, MeshData))
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeAllSDFSlices: Failed to extract mesh data"));
        return Results;
    }

    // 2. 바운딩 박스 계산
    FVector3f BoundsSize = MeshData.Bounds.Max - MeshData.Bounds.Min;
    FVector3f Margin = BoundsSize * 0.1f;
    FVector3f BoundsMin = MeshData.Bounds.Min - Margin;
    FVector3f BoundsMax = MeshData.Bounds.Max + Margin;
    FVector3f BoundsSizeWithMargin = BoundsMax - BoundsMin;

    float PlaneScaleX = BoundsSizeWithMargin.X / 100.0f;
    float PlaneScaleY = BoundsSizeWithMargin.Y / 100.0f;

    UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    UMaterial* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
    if (!BaseMaterial)
    {
        BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    }

    // 3. 모든 슬라이스에 대해 렌더 타겟 + 평면 액터 생성
    Results.SetNum(Resolution);
    TArray<UTextureRenderTarget2D*> RenderTargets;
    RenderTargets.SetNum(Resolution);

    for (int32 SliceZ = 0; SliceZ < Resolution; SliceZ++)
    {
        FSDFVisualizationResult& Result = Results[SliceZ];
        Result.BoundsMin = FVector(BoundsMin);
        Result.BoundsMax = FVector(BoundsMax);
        Result.CurrentSliceZ = SliceZ;
        Result.Resolution = Resolution;

        // 렌더 타겟 생성
        Result.SliceTexture = NewObject<UTextureRenderTarget2D>(WorldContextObject);
        Result.SliceTexture->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
        Result.SliceTexture->UpdateResourceImmediate(true);
        RenderTargets[SliceZ] = Result.SliceTexture;

        // 평면 액터 스폰
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        AActor* PlaneActor = World->SpawnActor<AActor>(AActor::StaticClass(), WorldLocation, FRotator::ZeroRotator, SpawnParams);
        if (!PlaneActor) continue;

        USceneComponent* RootComp = NewObject<USceneComponent>(PlaneActor, TEXT("Root"));
        PlaneActor->SetRootComponent(RootComp);
        RootComp->RegisterComponent();

        // Z 슬라이스 위치 계산
        float LocalSliceZ = FMath::Lerp(BoundsMin.Z, BoundsMax.Z, (float)SliceZ / (float)(Resolution - 1));
        FVector PlaneCenter(
            WorldLocation.X + (BoundsMin.X + BoundsMax.X) * 0.5f,
            WorldLocation.Y + (BoundsMin.Y + BoundsMax.Y) * 0.5f,
            WorldLocation.Z + LocalSliceZ
        );

        // 앞면 평면
        UStaticMeshComponent* FrontPlane = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("FrontPlane"));
        FrontPlane->SetupAttachment(RootComp);
        if (PlaneMesh) FrontPlane->SetStaticMesh(PlaneMesh);
        FrontPlane->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));
        FrontPlane->SetWorldLocation(PlaneCenter);

        UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, PlaneActor);
        if (DynMaterial && Result.SliceTexture)
        {
            DynMaterial->SetTextureParameterValue(TEXT("SlateUI"), Result.SliceTexture);
        }
        FrontPlane->SetMaterial(0, DynMaterial);
        FrontPlane->RegisterComponent();

        // 뒷면 평면
        UStaticMeshComponent* BackPlane = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("BackPlane"));
        BackPlane->SetupAttachment(RootComp);
        if (PlaneMesh) BackPlane->SetStaticMesh(PlaneMesh);
        BackPlane->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));
        BackPlane->SetWorldLocation(PlaneCenter);
        BackPlane->SetWorldRotation(FRotator(180.0f, 0.0f, 0.0f));
        BackPlane->SetMaterial(0, DynMaterial);
        BackPlane->RegisterComponent();

        Result.PlaneActor = PlaneActor;
    }

    // 4. GPU 작업: SDF 1회 생성 + 모든 슬라이스 시각화
    TArray<FVector3f> Vertices = MeshData.Vertices;
    TArray<uint32> Indices = MeshData.Indices;
    FIntVector SDFResolution(Resolution, Resolution, Resolution);
    float MaxDisplayDist = BoundsSize.GetMax() * 0.5f;

    ENQUEUE_RENDER_COMMAND(GenerateSDFAndAllSlices)(
        [Vertices, Indices, BoundsMin, BoundsMax, SDFResolution, MaxDisplayDist, RenderTargets](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);

            // SDF 3D 텍스처 생성 (1회만!)
            FRDGTextureDesc SDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef SDFTexture = GraphBuilder.CreateTexture(SDFDesc, TEXT("SDFTexture"));

            // SDF 생성 (1회만!)
            GenerateMeshSDF(
                GraphBuilder,
                SDFTexture,
                Vertices,
                Indices,
                BoundsMin,
                BoundsMax,
                SDFResolution
            );

            // Flood Fill 보정 (1회만!)
            FRDGTextureDesc CorrectedSDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef CorrectedSDF = GraphBuilder.CreateTexture(CorrectedSDFDesc, TEXT("CorrectedSDFTexture"));

            Apply2DSliceFloodFill(
                GraphBuilder,
                SDFTexture,
                CorrectedSDF,
                SDFResolution
            );

            // 모든 슬라이스 시각화
            for (int32 SliceZ = 0; SliceZ < SDFResolution.Z; SliceZ++)
            {
                UTextureRenderTarget2D* RenderTarget = RenderTargets[SliceZ];
                if (!RenderTarget) continue;

                FTextureRenderTargetResource* RTResource = RenderTarget->GetRenderTargetResource();
                if (!RTResource) continue;

                // 슬라이스 2D 텍스처 생성
                FRDGTextureDesc SliceDesc = FRDGTextureDesc::Create2D(
                    FIntPoint(SDFResolution.X, SDFResolution.Y),
                    PF_B8G8R8A8,
                    FClearValueBinding::Black,
                    TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
                );
                FRDGTextureRef SliceTexture = GraphBuilder.CreateTexture(SliceDesc, *FString::Printf(TEXT("SDFSlice_%d"), SliceZ));

                // 슬라이스 시각화
                GenerateSDFSlice(
                    GraphBuilder,
                    CorrectedSDF,
                    SliceTexture,
                    SDFResolution,
                    SliceZ,
                    MaxDisplayDist
                );

                // 렌더 타겟에 복사
                FRHITexture* DestRHI = RTResource->GetRenderTargetTexture();
                if (DestRHI)
                {
                    FRDGTextureRef DestTexture = GraphBuilder.RegisterExternalTexture(
                        CreateRenderTarget(DestRHI, TEXT("DestRenderTarget"))
                    );
                    AddCopyTexturePass(GraphBuilder, SliceTexture, DestTexture);
                }
            }

            GraphBuilder.Execute();

            UE_LOG(LogTemp, Warning, TEXT("VisualizeAllSDFSlices: Generated SDF once, visualized %d slices"), SDFResolution.Z);
        }
    );

    UE_LOG(LogTemp, Warning, TEXT("VisualizeAllSDFSlices: Created %d slice visualizations"), Resolution);
    return Results;
}

UTexture2D* UFleshRingSDFVisualizer::GenerateSDFSliceTexture(
    UStaticMesh* Mesh,
    int32 SliceZ,
    int32 Resolution)
{
    // TODO: 구현 - 평면 없이 텍스처만 생성
    UE_LOG(LogTemp, Warning, TEXT("GenerateSDFSliceTexture: Not yet implemented"));
    return nullptr;
}

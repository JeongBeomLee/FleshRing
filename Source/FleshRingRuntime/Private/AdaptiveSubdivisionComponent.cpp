// AdaptiveSubdivisionComponent.cpp
// Runtime adaptive mesh subdivision with ring deformation

#include "AdaptiveSubdivisionComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

UAdaptiveSubdivisionComponent::UAdaptiveSubdivisionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = true;
	bAutoActivate = true;
}

void UAdaptiveSubdivisionComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!ProceduralMesh)
	{
		AActor* Owner = GetOwner();
		if (Owner)
		{
			ProceduralMesh = Owner->FindComponentByClass<UProceduralMeshComponent>();
		}
	}

	if (ProceduralMesh)
	{
		GenerateMesh();
	}
}

void UAdaptiveSubdivisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bAutoUpdate)
	{
		if (bMeshDirty)
		{
			GenerateMesh();
		}
		else
		{
			UpdateDeformation();
		}
	}

	if (bShowDebug)
	{
		DrawDebugVisualization();
	}
}

#if WITH_EDITOR
void UAdaptiveSubdivisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const bool bIsInteractive = PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive;

	const bool bNeedsRegenerate =
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, MeshType) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, PlaneSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, CubeSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, SphereRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, SphereSegments) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, CylinderRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, CylinderHeight) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, CylinderRadialSegments) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, CylinderHeightSegments) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, bCylinderCaps) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, InitialSubdivisions) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, MaxAdaptiveLevel) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, bEnableAdaptive) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, SubdivisionTriggerDistance) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, SubdivisionMethod) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, LEBMaxLevel) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, LEBMinEdgeLength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, LEBInfluenceMultiplier);

	const bool bNeedsDeformUpdate =
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, RingProfile) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, RingCenter) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, RingDirection) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, RingInnerRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, RingOuterRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, RingThickness) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, TorusMajorRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, TorusMinorRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, ConeTaperRatio) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, DeformStrength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UAdaptiveSubdivisionComponent, DeformFalloff);

	if (bNeedsRegenerate && !bIsInteractive)
	{
		GenerateMesh();
	}
	else if (bNeedsDeformUpdate)
	{
		UpdateDeformation();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UAdaptiveSubdivisionComponent::GenerateMesh()
{
	bLEBCached = false;
	HalfEdgeMeshData.Clear();

	CreateBaseMesh(BaseMeshData);

	int32 BaseTriangles = BaseMeshData.GetTriangleCount();

	switch (SubdivisionMethod)
	{
	case ESubdivisionMethod::LEB:
		{
			if (InitialSubdivisions > 0)
			{
				LoopSubdivide(BaseMeshData);
			}

			PerformLEBSubdivision(BaseMeshData);
		}
		break;

	case ESubdivisionMethod::Adaptive:
		{
			int32 SafeSubdivLevel = InitialSubdivisions;
			int64 EstimatedTriangles = BaseTriangles;

			for (int32 i = 0; i < InitialSubdivisions; i++)
			{
				int64 NextEstimate = EstimatedTriangles * 4;
				if (NextEstimate > MaxTriangleCount)
				{
					SafeSubdivLevel = i;
					break;
				}
				EstimatedTriangles = NextEstimate;
			}

			for (int32 i = 0; i < SafeSubdivLevel; i++)
			{
				LoopSubdivide(BaseMeshData);
				if (BaseMeshData.GetTriangleCount() > MaxTriangleCount)
				{
					break;
				}
			}

			if (bEnableAdaptive && MaxAdaptiveLevel > 0 && BaseMeshData.GetTriangleCount() < MaxTriangleCount)
			{
				for (int32 Level = 0; Level < MaxAdaptiveLevel; Level++)
				{
					AdaptiveSubdivide(BaseMeshData, Level);
					if (BaseMeshData.GetTriangleCount() > MaxTriangleCount)
					{
						break;
					}
				}
			}
		}
		break;

	case ESubdivisionMethod::Uniform:
	default:
		{
			int32 SafeSubdivLevel = InitialSubdivisions;
			int64 EstimatedTriangles = BaseTriangles;

			for (int32 i = 0; i < InitialSubdivisions; i++)
			{
				int64 NextEstimate = EstimatedTriangles * 4;
				if (NextEstimate > MaxTriangleCount)
				{
					SafeSubdivLevel = i;
					break;
				}
				EstimatedTriangles = NextEstimate;
			}

			for (int32 i = 0; i < SafeSubdivLevel; i++)
			{
				LoopSubdivide(BaseMeshData);
				if (BaseMeshData.GetTriangleCount() > MaxTriangleCount)
				{
					break;
				}
			}
		}
		break;
	}

	CurrentMeshData = BaseMeshData;
	ApplyRingDeformation(CurrentMeshData);
	RecalculateNormals(CurrentMeshData);

	UpdateProceduralMesh();

	bMeshDirty = false;
}

void UAdaptiveSubdivisionComponent::UpdateDeformation()
{
	if (BaseMeshData.Vertices.Num() == 0) return;

	CurrentMeshData = BaseMeshData;
	ApplyRingDeformation(CurrentMeshData);
	RecalculateNormals(CurrentMeshData);

	UpdateProceduralMesh();
}

void UAdaptiveSubdivisionComponent::SetRingFromWorldTransform(FVector WorldCenter, FVector WorldDirection)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	FTransform OwnerTransform = Owner->GetActorTransform();
	RingCenter = OwnerTransform.InverseTransformPosition(WorldCenter);
	RingDirection = OwnerTransform.InverseTransformVectorNoScale(WorldDirection).GetSafeNormal();
}

void UAdaptiveSubdivisionComponent::CreateBaseMesh(FSubdivisionMeshData& OutMesh)
{
	switch (MeshType)
	{
	case EBaseMeshType::Plane:
		CreateBasePlane(OutMesh);
		break;
	case EBaseMeshType::Cube:
		CreateBaseCube(OutMesh);
		break;
	case EBaseMeshType::Sphere:
		CreateBaseSphere(OutMesh);
		break;
	case EBaseMeshType::Cylinder:
		CreateBaseCylinder(OutMesh);
		break;
	default:
		CreateBasePlane(OutMesh);
		break;
	}
}

void UAdaptiveSubdivisionComponent::AddQuad(FSubdivisionMeshData& Mesh, int32 V0, int32 V1, int32 V2, int32 V3)
{
	Mesh.Triangles.Add(V0);
	Mesh.Triangles.Add(V2);
	Mesh.Triangles.Add(V1);

	Mesh.Triangles.Add(V0);
	Mesh.Triangles.Add(V3);
	Mesh.Triangles.Add(V2);
}

void UAdaptiveSubdivisionComponent::CreateBasePlane(FSubdivisionMeshData& OutMesh)
{
	OutMesh.Clear();

	float HalfX = PlaneSize.X * 0.5f;
	float HalfY = PlaneSize.Y * 0.5f;

	OutMesh.Vertices.Add(FVector(-HalfX, -HalfY, 0));
	OutMesh.Vertices.Add(FVector(HalfX, -HalfY, 0));
	OutMesh.Vertices.Add(FVector(HalfX, HalfY, 0));
	OutMesh.Vertices.Add(FVector(-HalfX, HalfY, 0));

	OutMesh.UVs.Add(FVector2D(0, 1));
	OutMesh.UVs.Add(FVector2D(1, 1));
	OutMesh.UVs.Add(FVector2D(1, 0));
	OutMesh.UVs.Add(FVector2D(0, 0));

	OutMesh.Triangles.Add(0);
	OutMesh.Triangles.Add(1);
	OutMesh.Triangles.Add(2);

	OutMesh.Triangles.Add(0);
	OutMesh.Triangles.Add(2);
	OutMesh.Triangles.Add(3);

	OutMesh.Normals.SetNum(OutMesh.Vertices.Num());
	for (int32 i = 0; i < OutMesh.Normals.Num(); i++)
	{
		OutMesh.Normals[i] = FVector(0, 0, 1);
	}

	OutMesh.VertexColors.SetNum(OutMesh.Vertices.Num());
	for (int32 i = 0; i < OutMesh.VertexColors.Num(); i++)
	{
		OutMesh.VertexColors[i] = FColor::White;
	}
}

void UAdaptiveSubdivisionComponent::CreateBaseCube(FSubdivisionMeshData& OutMesh)
{
	OutMesh.Clear();

	float H = CubeSize * 0.5f;

	// Front face (Z+)
	OutMesh.Vertices.Add(FVector(-H, -H, H));
	OutMesh.Vertices.Add(FVector(H, -H, H));
	OutMesh.Vertices.Add(FVector(H, H, H));
	OutMesh.Vertices.Add(FVector(-H, H, H));

	// Back face (Z-)
	OutMesh.Vertices.Add(FVector(H, -H, -H));
	OutMesh.Vertices.Add(FVector(-H, -H, -H));
	OutMesh.Vertices.Add(FVector(-H, H, -H));
	OutMesh.Vertices.Add(FVector(H, H, -H));

	// Right face (X+)
	OutMesh.Vertices.Add(FVector(H, -H, H));
	OutMesh.Vertices.Add(FVector(H, -H, -H));
	OutMesh.Vertices.Add(FVector(H, H, -H));
	OutMesh.Vertices.Add(FVector(H, H, H));

	// Left face (X-)
	OutMesh.Vertices.Add(FVector(-H, -H, -H));
	OutMesh.Vertices.Add(FVector(-H, -H, H));
	OutMesh.Vertices.Add(FVector(-H, H, H));
	OutMesh.Vertices.Add(FVector(-H, H, -H));

	// Top face (Y+)
	OutMesh.Vertices.Add(FVector(-H, H, H));
	OutMesh.Vertices.Add(FVector(H, H, H));
	OutMesh.Vertices.Add(FVector(H, H, -H));
	OutMesh.Vertices.Add(FVector(-H, H, -H));

	// Bottom face (Y-)
	OutMesh.Vertices.Add(FVector(-H, -H, -H));
	OutMesh.Vertices.Add(FVector(H, -H, -H));
	OutMesh.Vertices.Add(FVector(H, -H, H));
	OutMesh.Vertices.Add(FVector(-H, -H, H));

	for (int32 i = 0; i < 6; i++)
	{
		OutMesh.UVs.Add(FVector2D(0, 1));
		OutMesh.UVs.Add(FVector2D(1, 1));
		OutMesh.UVs.Add(FVector2D(1, 0));
		OutMesh.UVs.Add(FVector2D(0, 0));
	}

	AddQuad(OutMesh, 0, 1, 2, 3);
	AddQuad(OutMesh, 4, 5, 6, 7);
	AddQuad(OutMesh, 8, 9, 10, 11);
	AddQuad(OutMesh, 12, 13, 14, 15);
	AddQuad(OutMesh, 16, 17, 18, 19);
	AddQuad(OutMesh, 20, 21, 22, 23);

	OutMesh.Normals.SetNum(24);
	for (int32 i = 0; i < 4; i++) OutMesh.Normals[i] = FVector(0, 0, 1);
	for (int32 i = 4; i < 8; i++) OutMesh.Normals[i] = FVector(0, 0, -1);
	for (int32 i = 8; i < 12; i++) OutMesh.Normals[i] = FVector(1, 0, 0);
	for (int32 i = 12; i < 16; i++) OutMesh.Normals[i] = FVector(-1, 0, 0);
	for (int32 i = 16; i < 20; i++) OutMesh.Normals[i] = FVector(0, 1, 0);
	for (int32 i = 20; i < 24; i++) OutMesh.Normals[i] = FVector(0, -1, 0);

	OutMesh.VertexColors.SetNum(24);
	for (int32 i = 0; i < 24; i++)
	{
		OutMesh.VertexColors[i] = FColor::White;
	}
}

void UAdaptiveSubdivisionComponent::CreateBaseSphere(FSubdivisionMeshData& OutMesh)
{
	OutMesh.Clear();

	const int32 Rings = SphereSegments;
	const int32 Sectors = SphereSegments * 2;
	const float R = SphereRadius;

	for (int32 r = 0; r <= Rings; r++)
	{
		float Phi = PI * float(r) / float(Rings);
		float Y = R * FMath::Cos(Phi);
		float RingRadius = R * FMath::Sin(Phi);

		for (int32 s = 0; s <= Sectors; s++)
		{
			float Theta = 2.0f * PI * float(s) / float(Sectors);
			float X = RingRadius * FMath::Cos(Theta);
			float Z = RingRadius * FMath::Sin(Theta);

			OutMesh.Vertices.Add(FVector(X, Y, Z));

			float U = float(s) / float(Sectors);
			float V = float(r) / float(Rings);
			OutMesh.UVs.Add(FVector2D(U, V));

			FVector Normal = FVector(X, Y, Z).GetSafeNormal();
			OutMesh.Normals.Add(Normal);

			OutMesh.VertexColors.Add(FColor::White);
		}
	}

	for (int32 r = 0; r < Rings; r++)
	{
		for (int32 s = 0; s < Sectors; s++)
		{
			int32 Current = r * (Sectors + 1) + s;
			int32 Next = Current + Sectors + 1;

			OutMesh.Triangles.Add(Current);
			OutMesh.Triangles.Add(Next);
			OutMesh.Triangles.Add(Current + 1);

			OutMesh.Triangles.Add(Next);
			OutMesh.Triangles.Add(Next + 1);
			OutMesh.Triangles.Add(Current + 1);
		}
	}
}

void UAdaptiveSubdivisionComponent::CreateBaseCylinder(FSubdivisionMeshData& OutMesh)
{
	OutMesh.Clear();

	const int32 RadialSegs = CylinderRadialSegments;
	const int32 HeightSegs = CylinderHeightSegments;
	const float Radius = CylinderRadius;
	const float HalfHeight = CylinderHeight * 0.5f;

	for (int32 h = 0; h <= HeightSegs; h++)
	{
		float Y = FMath::Lerp(-HalfHeight, HalfHeight, (float)h / HeightSegs);
		float V = (float)h / HeightSegs;

		for (int32 r = 0; r <= RadialSegs; r++)
		{
			float Angle = 2.0f * PI * (float)r / RadialSegs;
			float X = Radius * FMath::Cos(Angle);
			float Z = Radius * FMath::Sin(Angle);

			OutMesh.Vertices.Add(FVector(X, Y, Z));
			OutMesh.UVs.Add(FVector2D((float)r / RadialSegs, V));

			FVector Normal = FVector(FMath::Cos(Angle), 0, FMath::Sin(Angle));
			OutMesh.Normals.Add(Normal);
			OutMesh.VertexColors.Add(FColor::White);
		}
	}

	for (int32 h = 0; h < HeightSegs; h++)
	{
		for (int32 r = 0; r < RadialSegs; r++)
		{
			int32 Current = h * (RadialSegs + 1) + r;
			int32 Next = Current + RadialSegs + 1;

			OutMesh.Triangles.Add(Current);
			OutMesh.Triangles.Add(Current + 1);
			OutMesh.Triangles.Add(Next);

			OutMesh.Triangles.Add(Next);
			OutMesh.Triangles.Add(Current + 1);
			OutMesh.Triangles.Add(Next + 1);
		}
	}

	if (bCylinderCaps)
	{
		int32 BottomCenterIdx = OutMesh.Vertices.Num();
		OutMesh.Vertices.Add(FVector(0, -HalfHeight, 0));
		OutMesh.UVs.Add(FVector2D(0.5f, 0.5f));
		OutMesh.Normals.Add(FVector(0, -1, 0));
		OutMesh.VertexColors.Add(FColor::White);

		int32 BottomRingStart = OutMesh.Vertices.Num();
		for (int32 r = 0; r <= RadialSegs; r++)
		{
			float Angle = 2.0f * PI * (float)r / RadialSegs;
			float X = Radius * FMath::Cos(Angle);
			float Z = Radius * FMath::Sin(Angle);

			OutMesh.Vertices.Add(FVector(X, -HalfHeight, Z));
			OutMesh.UVs.Add(FVector2D(FMath::Cos(Angle) * 0.5f + 0.5f, FMath::Sin(Angle) * 0.5f + 0.5f));
			OutMesh.Normals.Add(FVector(0, -1, 0));
			OutMesh.VertexColors.Add(FColor::White);
		}

		for (int32 r = 0; r < RadialSegs; r++)
		{
			OutMesh.Triangles.Add(BottomCenterIdx);
			OutMesh.Triangles.Add(BottomRingStart + r + 1);
			OutMesh.Triangles.Add(BottomRingStart + r);
		}

		int32 TopCenterIdx = OutMesh.Vertices.Num();
		OutMesh.Vertices.Add(FVector(0, HalfHeight, 0));
		OutMesh.UVs.Add(FVector2D(0.5f, 0.5f));
		OutMesh.Normals.Add(FVector(0, 1, 0));
		OutMesh.VertexColors.Add(FColor::White);

		int32 TopRingStart = OutMesh.Vertices.Num();
		for (int32 r = 0; r <= RadialSegs; r++)
		{
			float Angle = 2.0f * PI * (float)r / RadialSegs;
			float X = Radius * FMath::Cos(Angle);
			float Z = Radius * FMath::Sin(Angle);

			OutMesh.Vertices.Add(FVector(X, HalfHeight, Z));
			OutMesh.UVs.Add(FVector2D(FMath::Cos(Angle) * 0.5f + 0.5f, FMath::Sin(Angle) * 0.5f + 0.5f));
			OutMesh.Normals.Add(FVector(0, 1, 0));
			OutMesh.VertexColors.Add(FColor::White);
		}

		for (int32 r = 0; r < RadialSegs; r++)
		{
			OutMesh.Triangles.Add(TopCenterIdx);
			OutMesh.Triangles.Add(TopRingStart + r);
			OutMesh.Triangles.Add(TopRingStart + r + 1);
		}
	}
}

void UAdaptiveSubdivisionComponent::LoopSubdivide(FSubdivisionMeshData& InOutMesh)
{
	if (InOutMesh.Triangles.Num() < 3) return;

	FSubdivisionMeshData NewMesh;
	NewMesh.Vertices = InOutMesh.Vertices;
	NewMesh.UVs = InOutMesh.UVs;
	NewMesh.VertexColors = InOutMesh.VertexColors;

	InOutMesh.EdgeToMidpoint.Empty();

	int32 NumTriangles = InOutMesh.Triangles.Num() / 3;

	for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
	{
		int32 BaseIdx = TriIdx * 3;
		int32 V0 = InOutMesh.Triangles[BaseIdx];
		int32 V1 = InOutMesh.Triangles[BaseIdx + 1];
		int32 V2 = InOutMesh.Triangles[BaseIdx + 2];

		int32 M01 = GetOrCreateEdgeMidpoint(InOutMesh, V0, V1);
		int32 M12 = GetOrCreateEdgeMidpoint(InOutMesh, V1, V2);
		int32 M20 = GetOrCreateEdgeMidpoint(InOutMesh, V2, V0);

		while (NewMesh.Vertices.Num() <= FMath::Max3(M01, M12, M20))
		{
			int32 NewIdx = NewMesh.Vertices.Num();
			if (NewIdx < InOutMesh.Vertices.Num())
			{
				NewMesh.Vertices.Add(InOutMesh.Vertices[NewIdx]);
				NewMesh.UVs.Add(InOutMesh.UVs.IsValidIndex(NewIdx) ? InOutMesh.UVs[NewIdx] : FVector2D(0.5f, 0.5f));
				NewMesh.VertexColors.Add(FColor::Yellow);
			}
		}

		NewMesh.Triangles.Add(V0);
		NewMesh.Triangles.Add(M01);
		NewMesh.Triangles.Add(M20);

		NewMesh.Triangles.Add(M01);
		NewMesh.Triangles.Add(V1);
		NewMesh.Triangles.Add(M12);

		NewMesh.Triangles.Add(M20);
		NewMesh.Triangles.Add(M12);
		NewMesh.Triangles.Add(V2);

		NewMesh.Triangles.Add(M01);
		NewMesh.Triangles.Add(M12);
		NewMesh.Triangles.Add(M20);
	}

	NewMesh.Normals.SetNum(NewMesh.Vertices.Num());
	for (int32 i = 0; i < NewMesh.Normals.Num(); i++)
	{
		NewMesh.Normals[i] = FVector(0, 0, 1);
	}

	while (NewMesh.VertexColors.Num() < NewMesh.Vertices.Num())
	{
		NewMesh.VertexColors.Add(FColor::Yellow);
	}

	InOutMesh = NewMesh;
}

void UAdaptiveSubdivisionComponent::AdaptiveSubdivide(FSubdivisionMeshData& InOutMesh, int32 Level)
{
	if (InOutMesh.Triangles.Num() < 3) return;

	FSubdivisionMeshData NewMesh;
	NewMesh.Vertices = InOutMesh.Vertices;
	NewMesh.UVs = InOutMesh.UVs;
	NewMesh.VertexColors = InOutMesh.VertexColors;

	InOutMesh.EdgeToMidpoint.Empty();

	int32 NumTriangles = InOutMesh.Triangles.Num() / 3;

	for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
	{
		int32 BaseIdx = TriIdx * 3;
		int32 V0 = InOutMesh.Triangles[BaseIdx];
		int32 V1 = InOutMesh.Triangles[BaseIdx + 1];
		int32 V2 = InOutMesh.Triangles[BaseIdx + 2];

		if (ShouldSubdivideTriangle(InOutMesh, TriIdx, Level))
		{
			int32 M01 = GetOrCreateEdgeMidpoint(InOutMesh, V0, V1);
			int32 M12 = GetOrCreateEdgeMidpoint(InOutMesh, V1, V2);
			int32 M20 = GetOrCreateEdgeMidpoint(InOutMesh, V2, V0);

			while (NewMesh.Vertices.Num() <= FMath::Max3(M01, M12, M20))
			{
				int32 NewIdx = NewMesh.Vertices.Num();
				if (NewIdx < InOutMesh.Vertices.Num())
				{
					NewMesh.Vertices.Add(InOutMesh.Vertices[NewIdx]);
					NewMesh.UVs.Add(InOutMesh.UVs.IsValidIndex(NewIdx) ? InOutMesh.UVs[NewIdx] : FVector2D(0.5f, 0.5f));
					NewMesh.VertexColors.Add(FColor::Red);
				}
			}

			NewMesh.Triangles.Add(V0);
			NewMesh.Triangles.Add(M01);
			NewMesh.Triangles.Add(M20);

			NewMesh.Triangles.Add(M01);
			NewMesh.Triangles.Add(V1);
			NewMesh.Triangles.Add(M12);

			NewMesh.Triangles.Add(M20);
			NewMesh.Triangles.Add(M12);
			NewMesh.Triangles.Add(V2);

			NewMesh.Triangles.Add(M01);
			NewMesh.Triangles.Add(M12);
			NewMesh.Triangles.Add(M20);
		}
		else
		{
			NewMesh.Triangles.Add(V0);
			NewMesh.Triangles.Add(V1);
			NewMesh.Triangles.Add(V2);
		}
	}

	NewMesh.Normals.SetNum(NewMesh.Vertices.Num());
	for (int32 i = 0; i < NewMesh.Normals.Num(); i++)
	{
		NewMesh.Normals[i] = FVector(0, 0, 1);
	}

	while (NewMesh.VertexColors.Num() < NewMesh.Vertices.Num())
	{
		NewMesh.VertexColors.Add(FColor::Red);
	}

	InOutMesh = NewMesh;
}

bool UAdaptiveSubdivisionComponent::ShouldSubdivideTriangle(const FSubdivisionMeshData& Mesh, int32 TriIndex, int32 CurrentLevel)
{
	int32 BaseIdx = TriIndex * 3;
	if (!Mesh.Triangles.IsValidIndex(BaseIdx + 2)) return false;

	int32 V0 = Mesh.Triangles[BaseIdx];
	int32 V1 = Mesh.Triangles[BaseIdx + 1];
	int32 V2 = Mesh.Triangles[BaseIdx + 2];

	FVector Center = (Mesh.Vertices[V0] + Mesh.Vertices[V1] + Mesh.Vertices[V2]) / 3.0f;

	float Distance = CalculateRingDistance(Center);

	float LevelMultiplier = 1.0f / (float)(CurrentLevel + 1);
	float Threshold = SubdivisionTriggerDistance * LevelMultiplier;

	return Distance < Threshold;
}

int32 UAdaptiveSubdivisionComponent::GetOrCreateEdgeMidpoint(FSubdivisionMeshData& Mesh, int32 V0, int32 V1)
{
	TPair<int32, int32> EdgeKey(FMath::Min(V0, V1), FMath::Max(V0, V1));

	if (int32* ExistingIdx = Mesh.EdgeToMidpoint.Find(EdgeKey))
	{
		return *ExistingIdx;
	}

	int32 NewIdx = Mesh.Vertices.Num();

	FVector MidPos = (Mesh.Vertices[V0] + Mesh.Vertices[V1]) * 0.5f;
	Mesh.Vertices.Add(MidPos);

	if (Mesh.UVs.IsValidIndex(V0) && Mesh.UVs.IsValidIndex(V1))
	{
		FVector2D MidUV = (Mesh.UVs[V0] + Mesh.UVs[V1]) * 0.5f;
		Mesh.UVs.Add(MidUV);
	}
	else
	{
		Mesh.UVs.Add(FVector2D(0.5f, 0.5f));
	}

	Mesh.EdgeToMidpoint.Add(EdgeKey, NewIdx);

	return NewIdx;
}

float UAdaptiveSubdivisionComponent::CalculateRingDistance(const FVector& Position) const
{
	FVector ToPos = Position - RingCenter;
	float AlongAxis = FVector::DotProduct(ToPos, RingDirection);
	FVector RadialPos = ToPos - (AlongAxis * RingDirection);
	float RadialDist = RadialPos.Size();

	switch (RingProfile)
	{
	case ERingProfileType::Torus:
		{
			FVector2D q(RadialDist - TorusMajorRadius, AlongAxis);
			float TorusDist = q.Size() - TorusMinorRadius;
			return FMath::Max(0.0f, TorusDist);
		}

	case ERingProfileType::Cone:
		{
			float NormalizedHeight = FMath::Clamp(AlongAxis / RingThickness, -1.0f, 1.0f);
			float TaperFactor = FMath::Lerp(1.0f, ConeTaperRatio, (NormalizedHeight + 1.0f) * 0.5f);
			float AdjustedOuter = RingOuterRadius * TaperFactor;

			if (FMath::Abs(AlongAxis) > RingThickness)
			{
				return FMath::Abs(AlongAxis) - RingThickness;
			}
			if (RadialDist > AdjustedOuter)
			{
				return RadialDist - AdjustedOuter;
			}
			return 0.0f;
		}

	case ERingProfileType::Cylinder:
	default:
		{
			if (FMath::Abs(AlongAxis) > RingThickness)
			{
				return FMath::Abs(AlongAxis) - RingThickness;
			}
			if (RadialDist > RingOuterRadius)
			{
				return RadialDist - RingOuterRadius;
			}
			return 0.0f;
		}
	}
}

void UAdaptiveSubdivisionComponent::ApplyRingDeformation(FSubdivisionMeshData& InOutMesh)
{
	if (DeformStrength <= 0.0f) return;

	FVector NormRingDir = RingDirection.GetSafeNormal();
	if (NormRingDir.IsNearlyZero())
	{
		NormRingDir = FVector::UpVector;
	}

	TSet<int32> CompressionVertices;
	TSet<int32> BulgeVertices;

	for (int32 i = 0; i < InOutMesh.Vertices.Num(); i++)
	{
		FVector& Pos = InOutMesh.Vertices[i];
		FVector ToPos = Pos - RingCenter;
		float AlongAxis = FVector::DotProduct(ToPos, NormRingDir);
		FVector RadialPos = ToPos - (AlongAxis * NormRingDir);
		float RadialDist = RadialPos.Size();

		if (RadialDist <= 0.001f) continue;

		FVector RadialDir = RadialPos / RadialDist;
		float DeformFactor = 0.0f;
		FVector TargetPos = Pos;

		switch (RingProfile)
		{
		case ERingProfileType::Torus:
			{
				float InnerEdge = TorusMajorRadius - TorusMinorRadius;
				float OuterEdge = TorusMajorRadius + TorusMinorRadius;
				float AxisDist = FMath::Abs(AlongAxis);

				float CompressionZoneEnd = TorusMinorRadius;
				float BulgeZoneEnd = TorusMinorRadius + DeformFalloff;

				if (AxisDist > BulgeZoneEnd) continue;
				if (RadialDist < InnerEdge * 0.3f) continue;
				if (RadialDist > OuterEdge + DeformFalloff * 1.5f) continue;

				if (AxisDist <= CompressionZoneEnd)
				{
					if (RadialDist <= InnerEdge) continue;

					float AxisFactor = 1.0f - FMath::Clamp(AxisDist / CompressionZoneEnd, 0.0f, 1.0f);
					AxisFactor = FMath::SmoothStep(0.0f, 1.0f, AxisFactor);

					float RadialFactor = 1.0f;
					if (RadialDist > OuterEdge)
					{
						RadialFactor = 1.0f - FMath::Clamp((RadialDist - OuterEdge) / DeformFalloff, 0.0f, 1.0f);
					}

					float CompressFactor = AxisFactor * RadialFactor * DeformStrength;

					if (CompressFactor > 0.001f)
					{
						float TargetRadius = InnerEdge;
						float NewRadius = FMath::Lerp(RadialDist, TargetRadius, CompressFactor);

						FVector NewPos = RingCenter + (AlongAxis * NormRingDir) + (RadialDir * NewRadius);
						Pos = NewPos;
						CompressionVertices.Add(i);

						if (InOutMesh.VertexColors.IsValidIndex(i))
						{
							uint8 Val = FMath::Clamp((int32)(CompressFactor * 255), 0, 255);
							InOutMesh.VertexColors[i] = FColor(Val, 255 - Val, 0, 255);
						}
					}
				}
				else
				{
					float DistFromCompression = AxisDist - CompressionZoneEnd;
					float BulgeRange = BulgeZoneEnd - CompressionZoneEnd;
					float BulgeFalloff = 1.0f - (DistFromCompression / BulgeRange);
					BulgeFalloff = FMath::SmoothStep(0.0f, 1.0f, BulgeFalloff);

					float RadialBulgeFactor = 1.0f;
					if (RadialDist < InnerEdge)
					{
						RadialBulgeFactor = FMath::Clamp((RadialDist - InnerEdge * 0.3f) / (InnerEdge * 0.7f), 0.0f, 1.0f);
					}
					else if (RadialDist > OuterEdge)
					{
						RadialBulgeFactor = 1.0f - FMath::Clamp((RadialDist - OuterEdge) / (DeformFalloff * 1.5f), 0.0f, 1.0f);
					}

					float BulgeStrength = 0.18f;
					float MaxBulge = (OuterEdge - InnerEdge) * BulgeStrength * DeformStrength;
					float BulgeAmount = MaxBulge * BulgeFalloff * RadialBulgeFactor;

					if (BulgeAmount > 0.01f)
					{
						float NewRadius = RadialDist + BulgeAmount;
						FVector NewPos = RingCenter + (AlongAxis * NormRingDir) + (RadialDir * NewRadius);
						Pos = NewPos;
						BulgeVertices.Add(i);

						if (InOutMesh.VertexColors.IsValidIndex(i))
						{
							uint8 Val = FMath::Clamp((int32)(BulgeAmount * 25), 0, 255);
							InOutMesh.VertexColors[i] = FColor(0, Val, 255, 255);
						}
					}
				}

				DeformFactor = 0.0f;
			}
			break;

		case ERingProfileType::Cone:
			{
				float NormalizedHeight = FMath::Clamp(AlongAxis / RingThickness, -1.0f, 1.0f);
				float TaperFactor = FMath::Lerp(1.0f, ConeTaperRatio, (NormalizedHeight + 1.0f) * 0.5f);
				float AdjustedOuter = RingOuterRadius * TaperFactor;
				float AdjustedInner = RingInnerRadius * TaperFactor;

				float AxisFalloff = 1.0f - FMath::Clamp(FMath::Abs(AlongAxis) / RingThickness, 0.0f, 1.0f);
				if (AxisFalloff <= 0.0f || RadialDist > AdjustedOuter) continue;

				if (RadialDist > AdjustedInner)
				{
					float T = (RadialDist - AdjustedInner) / (AdjustedOuter - AdjustedInner);
					float SmoothT = T * T * (3.0f - 2.0f * T);
					DeformFactor = (1.0f - SmoothT) * AxisFalloff * DeformStrength;
				}
				else
				{
					DeformFactor = AxisFalloff * DeformStrength;
				}

				float NewDist = FMath::Lerp(RadialDist, AdjustedInner, DeformFactor);
				TargetPos = RingCenter + (AlongAxis * RingDirection) + (RadialDir * NewDist);
			}
			break;

		case ERingProfileType::Cylinder:
		default:
			{
				float AxisFalloff = 1.0f - FMath::Clamp(FMath::Abs(AlongAxis) / RingThickness, 0.0f, 1.0f);
				if (AxisFalloff <= 0.0f || RadialDist > RingOuterRadius) continue;

				if (RadialDist > RingInnerRadius)
				{
					float T = (RadialDist - RingInnerRadius) / (RingOuterRadius - RingInnerRadius);
					float SmoothT = T * T * (3.0f - 2.0f * T);
					DeformFactor = (1.0f - SmoothT) * AxisFalloff * DeformStrength;
				}
				else
				{
					DeformFactor = AxisFalloff * DeformStrength;
				}

				float NewDist = FMath::Lerp(RadialDist, RingInnerRadius, DeformFactor);
				TargetPos = RingCenter + (AlongAxis * RingDirection) + (RadialDir * NewDist);
			}
			break;
		}

		if (DeformFactor > 0.0f)
		{
			Pos = FMath::Lerp(Pos, TargetPos, DeformFactor);

			if (InOutMesh.VertexColors.IsValidIndex(i))
			{
				uint8 RedVal = FMath::Clamp((int32)(DeformFactor * 255), 0, 255);
				InOutMesh.VertexColors[i] = FColor(RedVal, 255 - RedVal, 0, 255);
			}
		}
	}

	if (bEnableSmoothing && BulgeVertices.Num() > 0)
	{
		ApplyLaplacianSmoothing(InOutMesh, BulgeVertices, CompressionVertices);
	}
}

void UAdaptiveSubdivisionComponent::ApplyLaplacianSmoothing(FSubdivisionMeshData& InOutMesh, const TSet<int32>& BulgeVertices, const TSet<int32>& CompressionVertices)
{
	if (BulgeVertices.Num() == 0 || SmoothingStrength <= 0.0f) return;

	const int32 NumVerts = InOutMesh.Vertices.Num();
	const int32 NumTris = InOutMesh.Triangles.Num() / 3;

	TMap<int32, TSet<int32>> Adjacency;
	for (int32 TriIdx = 0; TriIdx < NumTris; TriIdx++)
	{
		int32 BaseIdx = TriIdx * 3;
		int32 V0 = InOutMesh.Triangles[BaseIdx];
		int32 V1 = InOutMesh.Triangles[BaseIdx + 1];
		int32 V2 = InOutMesh.Triangles[BaseIdx + 2];

		Adjacency.FindOrAdd(V0).Add(V1);
		Adjacency.FindOrAdd(V0).Add(V2);
		Adjacency.FindOrAdd(V1).Add(V0);
		Adjacency.FindOrAdd(V1).Add(V2);
		Adjacency.FindOrAdd(V2).Add(V0);
		Adjacency.FindOrAdd(V2).Add(V1);
	}

	TSet<int32> SmoothingSet;
	TSet<int32> BoundaryNeighbors;

	for (int32 VertIdx : BulgeVertices)
	{
		SmoothingSet.Add(VertIdx);

		if (TSet<int32>* Neighbors = Adjacency.Find(VertIdx))
		{
			for (int32 NeighborIdx : *Neighbors)
			{
				if (!CompressionVertices.Contains(NeighborIdx) && !BulgeVertices.Contains(NeighborIdx))
				{
					BoundaryNeighbors.Add(NeighborIdx);
				}
			}
		}
	}

	SmoothingSet.Append(BoundaryNeighbors);

	for (int32 Iter = 0; Iter < SmoothingIterations; Iter++)
	{
		TArray<FVector> NewPositions;
		NewPositions.SetNum(NumVerts);

		for (int32 i = 0; i < NumVerts; i++)
		{
			NewPositions[i] = InOutMesh.Vertices[i];
		}

		for (int32 VertIdx : SmoothingSet)
		{
			if (CompressionVertices.Contains(VertIdx)) continue;

			TSet<int32>* Neighbors = Adjacency.Find(VertIdx);
			if (!Neighbors || Neighbors->Num() == 0) continue;

			FVector Average = FVector::ZeroVector;
			int32 ValidNeighborCount = 0;
			for (int32 NeighborIdx : *Neighbors)
			{
				if (CompressionVertices.Contains(NeighborIdx)) continue;

				Average += InOutMesh.Vertices[NeighborIdx];
				ValidNeighborCount++;
			}

			if (ValidNeighborCount == 0) continue;
			Average /= ValidNeighborCount;

			float BlendFactor = SmoothingStrength;
			if (BoundaryNeighbors.Contains(VertIdx))
			{
				BlendFactor *= 0.4f;
			}

			NewPositions[VertIdx] = FMath::Lerp(InOutMesh.Vertices[VertIdx], Average, BlendFactor);
		}

		for (int32 VertIdx : SmoothingSet)
		{
			if (!CompressionVertices.Contains(VertIdx))
			{
				InOutMesh.Vertices[VertIdx] = NewPositions[VertIdx];
			}
		}
	}
}

void UAdaptiveSubdivisionComponent::RecalculateNormals(FSubdivisionMeshData& InOutMesh)
{
	int32 NumVerts = InOutMesh.Vertices.Num();
	if (NumVerts == 0) return;

	InOutMesh.Normals.SetNum(NumVerts);

	for (int32 i = 0; i < NumVerts; i++)
	{
		InOutMesh.Normals[i] = FVector::ZeroVector;
	}

	int32 NumTriangles = InOutMesh.Triangles.Num() / 3;
	for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
	{
		int32 BaseIdx = TriIdx * 3;
		int32 V0 = InOutMesh.Triangles[BaseIdx];
		int32 V1 = InOutMesh.Triangles[BaseIdx + 1];
		int32 V2 = InOutMesh.Triangles[BaseIdx + 2];

		if (V0 < 0 || V1 < 0 || V2 < 0 || V0 >= NumVerts || V1 >= NumVerts || V2 >= NumVerts) continue;

		FVector Edge1 = InOutMesh.Vertices[V1] - InOutMesh.Vertices[V0];
		FVector Edge2 = InOutMesh.Vertices[V2] - InOutMesh.Vertices[V0];
		FVector FaceNormal = FVector::CrossProduct(Edge2, Edge1).GetSafeNormal();

		if (!FaceNormal.IsNearlyZero())
		{
			InOutMesh.Normals[V0] += FaceNormal;
			InOutMesh.Normals[V1] += FaceNormal;
			InOutMesh.Normals[V2] += FaceNormal;
		}
	}

	for (int32 i = 0; i < NumVerts; i++)
	{
		InOutMesh.Normals[i] = InOutMesh.Normals[i].GetSafeNormal();
		if (InOutMesh.Normals[i].IsNearlyZero())
		{
			InOutMesh.Normals[i] = FVector(0, 0, 1);
		}
	}
}

void UAdaptiveSubdivisionComponent::UpdateProceduralMesh()
{
	if (!ProceduralMesh)
	{
		AActor* Owner = GetOwner();
		if (Owner)
		{
			ProceduralMesh = Owner->FindComponentByClass<UProceduralMeshComponent>();
		}
	}

	if (!ProceduralMesh) return;

	TArray<FProcMeshTangent> Tangents;
	Tangents.SetNum(CurrentMeshData.Vertices.Num());
	for (int32 i = 0; i < Tangents.Num(); i++)
	{
		Tangents[i] = FProcMeshTangent(FVector(1, 0, 0), false);
	}

	TArray<FLinearColor> LinearColors;
	LinearColors.SetNum(CurrentMeshData.VertexColors.Num());
	for (int32 i = 0; i < LinearColors.Num(); i++)
	{
		LinearColors[i] = FLinearColor(CurrentMeshData.VertexColors[i]);
	}

	ProceduralMesh->CreateMeshSection_LinearColor(
		0,
		CurrentMeshData.Vertices,
		CurrentMeshData.Triangles,
		CurrentMeshData.Normals,
		CurrentMeshData.UVs,
		LinearColors,
		Tangents,
		true
	);
}

void UAdaptiveSubdivisionComponent::DrawDebugVisualization()
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (!World) return;

	AActor* Owner = GetOwner();
	if (!Owner) return;

	FTransform WorldTransform = Owner->GetActorTransform();
	FVector WorldRingCenter = WorldTransform.TransformPosition(RingCenter);
	FVector WorldRingDir = WorldTransform.TransformVectorNoScale(RingDirection).GetSafeNormal();

	if (WorldRingDir.IsNearlyZero())
	{
		WorldRingDir = FVector::UpVector;
	}

	FVector RingAxisX, RingAxisY;
	WorldRingDir.FindBestAxisVectors(RingAxisX, RingAxisY);

	const int32 NumSegments = 32;
	const int32 NumTubeSegments = 16;
	const float AngleStep = 2.0f * PI / NumSegments;

	switch (RingProfile)
	{
	case ERingProfileType::Torus:
		{
			float InnerEdge = TorusMajorRadius - TorusMinorRadius;

			for (int32 i = 0; i < NumSegments; i++)
			{
				float Angle1 = i * AngleStep;
				float Angle2 = (i + 1) * AngleStep;

				FVector P1 = WorldRingCenter + (FMath::Cos(Angle1) * RingAxisX + FMath::Sin(Angle1) * RingAxisY) * InnerEdge;
				FVector P2 = WorldRingCenter + (FMath::Cos(Angle2) * RingAxisX + FMath::Sin(Angle2) * RingAxisY) * InnerEdge;

				DrawDebugLine(World, P1, P2, FColor::Cyan, false, -1.0f, 0, 3.0f);
			}

			for (int32 i = 0; i < NumSegments; i++)
			{
				float Angle1 = i * AngleStep;
				float Angle2 = (i + 1) * AngleStep;

				FVector P1 = WorldRingCenter + (FMath::Cos(Angle1) * RingAxisX + FMath::Sin(Angle1) * RingAxisY) * TorusMajorRadius;
				FVector P2 = WorldRingCenter + (FMath::Cos(Angle2) * RingAxisX + FMath::Sin(Angle2) * RingAxisY) * TorusMajorRadius;

				DrawDebugLine(World, P1, P2, FColor::Green, false, -1.0f, 0, 2.0f);
			}

			for (int32 j = 0; j < 8; j++)
			{
				float MajorAngle = j * PI / 4.0f;
				FVector RadialDir = FMath::Cos(MajorAngle) * RingAxisX + FMath::Sin(MajorAngle) * RingAxisY;
				FVector TubeCenter = WorldRingCenter + RadialDir * TorusMajorRadius;

				FVector TubeAxisX = RadialDir;
				FVector TubeAxisY = WorldRingDir;

				float TubeAngleStep = 2.0f * PI / NumTubeSegments;
				for (int32 i = 0; i < NumTubeSegments; i++)
				{
					float Angle1 = i * TubeAngleStep;
					float Angle2 = (i + 1) * TubeAngleStep;

					FVector P1 = TubeCenter + (FMath::Cos(Angle1) * TubeAxisX + FMath::Sin(Angle1) * TubeAxisY) * TorusMinorRadius;
					FVector P2 = TubeCenter + (FMath::Cos(Angle2) * TubeAxisX + FMath::Sin(Angle2) * TubeAxisY) * TorusMinorRadius;

					DrawDebugLine(World, P1, P2, FColor::Red, false, -1.0f, 0, 1.0f);
				}
			}

			float FalloffRadius = TorusMinorRadius + DeformFalloff;
			for (int32 j = 0; j < 4; j++)
			{
				float MajorAngle = j * PI / 2.0f;
				FVector RadialDir = FMath::Cos(MajorAngle) * RingAxisX + FMath::Sin(MajorAngle) * RingAxisY;
				FVector TubeCenter = WorldRingCenter + RadialDir * TorusMajorRadius;

				FVector TubeAxisX = RadialDir;
				FVector TubeAxisY = WorldRingDir;

				float TubeAngleStep = 2.0f * PI / NumTubeSegments;
				for (int32 i = 0; i < NumTubeSegments; i++)
				{
					float Angle1 = i * TubeAngleStep;
					float Angle2 = (i + 1) * TubeAngleStep;

					FVector P1 = TubeCenter + (FMath::Cos(Angle1) * TubeAxisX + FMath::Sin(Angle1) * TubeAxisY) * FalloffRadius;
					FVector P2 = TubeCenter + (FMath::Cos(Angle2) * TubeAxisX + FMath::Sin(Angle2) * TubeAxisY) * FalloffRadius;

					DrawDebugLine(World, P1, P2, FColor::Yellow, false, -1.0f, 0, 0.5f);
				}
			}
		}
		break;

	case ERingProfileType::Cone:
	case ERingProfileType::Cylinder:
	default:
		{
			for (int32 Offset = -1; Offset <= 1; Offset++)
			{
				FVector OffsetCenter = WorldRingCenter + WorldRingDir * (Offset * RingThickness);

				float TaperFactor = 1.0f;
				if (RingProfile == ERingProfileType::Cone)
				{
					float NormalizedHeight = (float)Offset * 0.5f + 0.5f;
					TaperFactor = FMath::Lerp(1.0f, ConeTaperRatio, NormalizedHeight);
				}

				float AdjustedInner = RingInnerRadius * TaperFactor;
				float AdjustedOuter = RingOuterRadius * TaperFactor;

				FColor InnerColor = (Offset == 0) ? FColor::Red : FColor(128, 0, 0);
				FColor OuterColor = (Offset == 0) ? FColor::Yellow : FColor(128, 128, 0);

				for (int32 i = 0; i < NumSegments; i++)
				{
					float Angle1 = i * AngleStep;
					float Angle2 = (i + 1) * AngleStep;

					FVector Dir1 = FMath::Cos(Angle1) * RingAxisX + FMath::Sin(Angle1) * RingAxisY;
					FVector Dir2 = FMath::Cos(Angle2) * RingAxisX + FMath::Sin(Angle2) * RingAxisY;

					DrawDebugLine(World, OffsetCenter + Dir1 * AdjustedInner, OffsetCenter + Dir2 * AdjustedInner,
						InnerColor, false, -1.0f, 0, (Offset == 0) ? 2.0f : 1.0f);

					DrawDebugLine(World, OffsetCenter + Dir1 * AdjustedOuter, OffsetCenter + Dir2 * AdjustedOuter,
						OuterColor, false, -1.0f, 0, (Offset == 0) ? 1.5f : 0.5f);
				}
			}

			for (int32 i = 0; i < 4; i++)
			{
				float Angle = i * PI / 2.0f;
				FVector RadialDir = FMath::Cos(Angle) * RingAxisX + FMath::Sin(Angle) * RingAxisY;

				float TopTaper = (RingProfile == ERingProfileType::Cone) ? ConeTaperRatio : 1.0f;
				float BotTaper = 1.0f;

				FVector InnerTop = WorldRingCenter + WorldRingDir * RingThickness + RadialDir * RingInnerRadius * TopTaper;
				FVector InnerBot = WorldRingCenter - WorldRingDir * RingThickness + RadialDir * RingInnerRadius * BotTaper;
				DrawDebugLine(World, InnerTop, InnerBot, FColor::Red, false, -1.0f, 0, 1.0f);

				FVector OuterTop = WorldRingCenter + WorldRingDir * RingThickness + RadialDir * RingOuterRadius * TopTaper;
				FVector OuterBot = WorldRingCenter - WorldRingDir * RingThickness + RadialDir * RingOuterRadius * BotTaper;
				DrawDebugLine(World, OuterTop, OuterBot, FColor::Yellow, false, -1.0f, 0, 0.5f);
			}
		}
		break;
	}

	float AxisLength = (RingProfile == ERingProfileType::Torus) ? TorusMinorRadius * 2.0f : RingThickness * 1.5f;
	DrawDebugLine(World, WorldRingCenter - WorldRingDir * AxisLength, WorldRingCenter + WorldRingDir * AxisLength,
		FColor::Blue, false, -1.0f, 0, 3.0f);

	DrawDebugPoint(World, WorldRingCenter, 10.0f, FColor::White, false, -1.0f, 0);

	FString ProfileName = (RingProfile == ERingProfileType::Torus) ? TEXT("Torus") :
		(RingProfile == ERingProfileType::Cone) ? TEXT("Cone") : TEXT("Cylinder");

	FString Info = FString::Printf(TEXT("%s | Verts: %d | Tris: %d"),
		*ProfileName,
		CurrentMeshData.Vertices.Num(),
		CurrentMeshData.GetTriangleCount());

	DrawDebugString(World, WorldRingCenter + FVector(0, 0, 30), Info, nullptr, FColor::White, 0.0f, true);

	if (RingProfile == ERingProfileType::Torus)
	{
		float InnerEdge = TorusMajorRadius - TorusMinorRadius;
		FString Legend = FString::Printf(TEXT("Cyan=Target(%.0f) Green=Center(%.0f) Red=Surface Yellow=Falloff"),
			InnerEdge, TorusMajorRadius);
		DrawDebugString(World, WorldRingCenter + FVector(0, 0, 45), Legend, nullptr, FColor::Yellow, 0.0f, true);
	}

	if (SubdivisionMethod == ESubdivisionMethod::LEB)
	{
		float InfluenceMargin = TorusMinorRadius + DeformFalloff * 0.5f;
		float InfluenceRadius = TorusMinorRadius + InfluenceMargin;

		for (int32 j = 0; j < 4; j++)
		{
			float MajorAngle = j * PI / 2.0f;
			FVector RadialDir = FMath::Cos(MajorAngle) * RingAxisX + FMath::Sin(MajorAngle) * RingAxisY;
			FVector TubeCenter = WorldRingCenter + RadialDir * TorusMajorRadius;

			FVector TubeAxisX = RadialDir;
			FVector TubeAxisY = WorldRingDir;

			float TubeAngleStep = 2.0f * PI / NumTubeSegments;
			for (int32 i = 0; i < NumTubeSegments; i++)
			{
				float Angle1 = i * TubeAngleStep;
				float Angle2 = (i + 1) * TubeAngleStep;

				FVector P1 = TubeCenter + (FMath::Cos(Angle1) * TubeAxisX + FMath::Sin(Angle1) * TubeAxisY) * InfluenceRadius;
				FVector P2 = TubeCenter + (FMath::Cos(Angle2) * TubeAxisX + FMath::Sin(Angle2) * TubeAxisY) * InfluenceRadius;

				DrawDebugLine(World, P1, P2, FColor::Magenta, false, -1.0f, 0, 0.5f);
			}
		}

		FString LEBInfo = FString::Printf(TEXT("LEB: Margin=%.1f, MaxLvl=%d, MinEdge=%.1f"),
			InfluenceMargin, LEBMaxLevel, LEBMinEdgeLength);
		DrawDebugString(World, WorldRingCenter + FVector(0, 0, 60), LEBInfo, nullptr, FColor::Magenta, 0.0f, true);
	}
#endif
}

//=============================================================================
// LEB Subdivision Implementation
//=============================================================================

void UAdaptiveSubdivisionComponent::PerformLEBSubdivision(FSubdivisionMeshData& InOutMesh)
{
	float InfluenceMargin = TorusMinorRadius + DeformFalloff * 0.5f;
	bool bNeedsRecalc = !bLEBCached ||
		!CachedRingCenter.Equals(RingCenter, 0.1f) ||
		!FMath::IsNearlyEqual(CachedInfluenceRadius, InfluenceMargin, 0.1f);

	if (!bNeedsRecalc && HalfEdgeMeshData.GetFaceCount() > 0)
	{
		TArray<FVector> OutVerts;
		TArray<int32> OutTris;
		TArray<FVector2D> OutUVs;
		TArray<FVector> OutNormals;
		TArray<int32> OutMaterialIndices;
		HalfEdgeMeshData.ExportToTriangles(OutVerts, OutTris, OutUVs, OutNormals, OutMaterialIndices);

		InOutMesh.Vertices = MoveTemp(OutVerts);
		InOutMesh.Triangles = MoveTemp(OutTris);
		InOutMesh.UVs = MoveTemp(OutUVs);
		InOutMesh.Normals = MoveTemp(OutNormals);
		InOutMesh.VertexColors.SetNum(InOutMesh.Vertices.Num());
		for (FColor& Color : InOutMesh.VertexColors)
		{
			Color = FColor::White;
		}
		return;
	}

	if (!HalfEdgeMeshData.BuildFromTriangles(InOutMesh.Vertices, InOutMesh.Triangles, InOutMesh.UVs))
	{
		return;
	}

	if (!HalfEdgeMeshData.Validate())
	{
		UE_LOG(LogTemp, Warning, TEXT("LEB: Half-edge mesh validation failed, but continuing..."));
	}

	FTorusParams TorusParams;
	TorusParams.Center = RingCenter;
	TorusParams.Axis = RingDirection;
	TorusParams.MajorRadius = TorusMajorRadius;
	TorusParams.MinorRadius = TorusMinorRadius;
	TorusParams.InfluenceMargin = TorusMinorRadius + DeformFalloff * 0.5f;

	int32 AddedFaces = FLEBSubdivision::SubdivideRegion(
		HalfEdgeMeshData,
		TorusParams,
		LEBMaxLevel,
		LEBMinEdgeLength
	);

	TArray<FVector> OutVerts;
	TArray<int32> OutTris;
	TArray<FVector2D> OutUVs;
	TArray<FVector> OutNormals;
	TArray<int32> OutMaterialIndices;
	HalfEdgeMeshData.ExportToTriangles(OutVerts, OutTris, OutUVs, OutNormals, OutMaterialIndices);

	InOutMesh.Vertices = MoveTemp(OutVerts);
	InOutMesh.Triangles = MoveTemp(OutTris);
	InOutMesh.UVs = MoveTemp(OutUVs);
	InOutMesh.Normals = MoveTemp(OutNormals);

	InOutMesh.VertexColors.SetNum(InOutMesh.Vertices.Num());
	for (int32 i = 0; i < InOutMesh.VertexColors.Num(); i++)
	{
		FVector ToP = InOutMesh.Vertices[i] - RingCenter;
		FVector NormAxis = RingDirection.GetSafeNormal();
		float AxisDist = FVector::DotProduct(ToP, NormAxis);
		FVector RadialVec = ToP - (AxisDist * NormAxis);
		float RadialDist = RadialVec.Size();
		FVector2D Q(RadialDist - TorusMajorRadius, AxisDist);
		float TorusDist = Q.Size() - TorusMinorRadius;

		float T = FMath::Clamp(TorusDist / InfluenceMargin, 0.0f, 1.0f);

		uint8 G = FMath::Clamp((int32)(255 * (1.0f - T * 0.5f)), 0, 255);
		InOutMesh.VertexColors[i] = FColor(255, G, 255, 255);
	}

	bLEBCached = true;
	CachedRingCenter = RingCenter;
	CachedInfluenceRadius = InfluenceMargin;
}

void UAdaptiveSubdivisionComponent::GetLEBInfluenceRegion(FVector& OutCenter, float& OutRadius) const
{
	OutCenter = RingCenter;

	switch (RingProfile)
	{
	case ERingProfileType::Torus:
		{
			OutRadius = TorusMajorRadius;
		}
		break;

	case ERingProfileType::Cone:
	case ERingProfileType::Cylinder:
	default:
		{
			OutRadius = RingOuterRadius;
		}
		break;
	}
}

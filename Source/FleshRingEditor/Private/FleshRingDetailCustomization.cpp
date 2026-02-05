// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "IDetailPropertyRow.h"
#include "PropertyCustomizationHelpers.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "AssetThumbnail.h"
#include "Widgets/Input/SComboBox.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Blueprint.h"

#define LOCTEXT_NAMESPACE "FleshRingDetailCustomization"

TSharedRef<IDetailCustomization> FFleshRingDetailCustomization::MakeInstance()
{
	return MakeShareable(new FFleshRingDetailCustomization);
}

void FFleshRingDetailCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Cache selected objects
	DetailBuilder.GetObjectsBeingCustomized(SelectedObjects);

	// =====================================
	// Define category order
	// =====================================

	// FleshRing Asset category (top)
	IDetailCategoryBuilder& AssetCategory = DetailBuilder.EditCategory(
		TEXT("FleshRing Asset"),
		LOCTEXT("FleshRingAssetCategory", "FleshRing Asset"),
		ECategoryPriority::Important
	);

	// General category
	IDetailCategoryBuilder& GeneralCategory = DetailBuilder.EditCategory(
		TEXT("General"),
		LOCTEXT("GeneralCategory", "General"),
		ECategoryPriority::Default
	);

	// Target Settings category
	IDetailCategoryBuilder& TargetCategory = DetailBuilder.EditCategory(
		TEXT("Target Settings"),
		LOCTEXT("TargetSettingsCategory", "Target Settings"),
		ECategoryPriority::Default
	);

	// Debug category (editor only)
	IDetailCategoryBuilder& DebugCategory = DetailBuilder.EditCategory(
		TEXT("Debug"),
		LOCTEXT("DebugCategory", "Debug / Visualization"),
		ECategoryPriority::Default
	);

	// =====================================
	// Apply FleshRingAsset property filtering
	// =====================================

	TSharedRef<IPropertyHandle> AssetPropertyHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UFleshRingComponent, FleshRingAsset));

	// Hide existing property and replace with filtered version
	DetailBuilder.HideProperty(AssetPropertyHandle);

	AssetCategory.AddCustomRow(LOCTEXT("FleshRingAssetRow", "FleshRing Asset"))
		.NameContent()
		[
			AssetPropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(SObjectPropertyEntryBox)
				.PropertyHandle(AssetPropertyHandle)
				.AllowedClass(UFleshRingAsset::StaticClass())
				.OnShouldFilterAsset(this, &FFleshRingDetailCustomization::OnShouldFilterAsset)
				.AllowClear(true)
				.DisplayThumbnail(true)
				.ThumbnailPool(DetailBuilder.GetThumbnailPool())
		];

	// =====================================
	// Custom TargetSkeletalMeshComponent dropdown
	// =====================================

	TargetSkeletalMeshPropertyHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UFleshRingComponent, TargetSkeletalMeshComponent));

	// Hide default FComponentReference widget
	DetailBuilder.HideProperty(TargetSkeletalMeshPropertyHandle);

	// Build combo box options
	RefreshTargetMeshOptions();

	// Add custom dropdown showing only SkeletalMeshComponents
	TargetCategory.AddCustomRow(LOCTEXT("TargetSkeletalMeshComponentRow", "Target Skeletal Mesh Component"))
		.NameContent()
		[
			SNew(STextBlock)
				.Text(LOCTEXT("TargetSkeletalMeshComponentLabel", "Target Skeletal Mesh Component"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(170.0f)
		[
			SAssignNew(TargetMeshComboBox, SComboBox<TSharedPtr<FTargetMeshOption>>)
				.OptionsSource(&TargetMeshOptions)
				.OnSelectionChanged(this, &FFleshRingDetailCustomization::OnTargetMeshSelectionChanged)
				.OnGenerateWidget(this, &FFleshRingDetailCustomization::GenerateTargetMeshComboItem)
				.InitiallySelectedItem(CurrentTargetMeshSelection)
				.Content()
				[
					SNew(STextBlock)
						.Text(this, &FFleshRingDetailCustomization::GetCurrentTargetMeshText)
						.Font(IDetailLayoutBuilder::GetDetailFont())
				]
		];

	// =====================================
	// Hide default categories
	// =====================================

	DetailBuilder.HideCategory(TEXT("ComponentTick"));
	DetailBuilder.HideCategory(TEXT("Tags"));
	DetailBuilder.HideCategory(TEXT("AssetUserData"));
	DetailBuilder.HideCategory(TEXT("Collision"));
	DetailBuilder.HideCategory(TEXT("Cooking"));
	DetailBuilder.HideCategory(TEXT("ComponentReplication"));
}

UFleshRingComponent* FFleshRingDetailCustomization::GetFirstSelectedComponent() const
{
	for (const TWeakObjectPtr<UObject>& Obj : SelectedObjects)
	{
		if (UFleshRingComponent* Component = Cast<UFleshRingComponent>(Obj.Get()))
		{
			return Component;
		}
	}
	return nullptr;
}

USkeletalMesh* FFleshRingDetailCustomization::GetOwnerSkeletalMesh() const
{
	UFleshRingComponent* Component = GetFirstSelectedComponent();
	if (!Component)
	{
		return nullptr;
	}

	// No target set
	FName TargetPropertyName = Component->TargetSkeletalMeshComponent.ComponentProperty;
	if (TargetPropertyName.IsNone())
	{
		return nullptr;
	}

	// Helper lambda to find SkeletalMesh from Blueprint (including parent chain)
	auto FindMeshFromBlueprint = [&TargetPropertyName](UBlueprint* Blueprint) -> USkeletalMesh*
	{
		UBlueprint* CurrentBP = Blueprint;
		while (CurrentBP)
		{
			if (USimpleConstructionScript* SCS = CurrentBP->SimpleConstructionScript)
			{
				const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
				for (USCS_Node* Node : AllNodes)
				{
					if (!Node || !Node->ComponentTemplate)
					{
						continue;
					}

					if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
					{
						// Match by variable name (used by FComponentReference)
						if (Node->GetVariableName() == TargetPropertyName)
						{
							return SkelMeshComp->GetSkeletalMeshAsset();
						}
					}
				}
			}

			// Move to parent Blueprint
			UClass* ParentClass = CurrentBP->ParentClass;
			CurrentBP = ParentClass ? Cast<UBlueprint>(ParentClass->ClassGeneratedBy) : nullptr;
		}
		return nullptr;
	};

	// Helper lambda to find SkeletalMesh from CDO (native C++ components)
	auto FindMeshFromCDO = [&TargetPropertyName](UClass* ActorClass) -> USkeletalMesh*
	{
		if (!ActorClass) return nullptr;

		AActor* CDO = ActorClass->GetDefaultObject<AActor>();
		if (!CDO) return nullptr;

		TArray<USkeletalMeshComponent*> NativeComponents;
		CDO->GetComponents<USkeletalMeshComponent>(NativeComponents);
		for (USkeletalMeshComponent* SkelMeshComp : NativeComponents)
		{
			if (SkelMeshComp && SkelMeshComp->GetFName() == TargetPropertyName)
			{
				return SkelMeshComp->GetSkeletalMeshAsset();
			}
		}
		return nullptr;
	};

	// 1. Try Blueprint editor context (Outer is UClass)
	if (UClass* OuterClass = Cast<UClass>(Component->GetOuter()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(OuterClass->ClassGeneratedBy))
		{
			if (USkeletalMesh* Mesh = FindMeshFromBlueprint(Blueprint))
			{
				return Mesh;
			}
		}
		// Try native C++ components
		if (USkeletalMesh* Mesh = FindMeshFromCDO(OuterClass))
		{
			return Mesh;
		}
	}

	// 2. Try via Actor owner (Level editor or runtime)
	AActor* Owner = Component->GetOwner();
	if (Owner)
	{
		UClass* ActorClass = Owner->GetClass();

		// Try Blueprint (including parent chain)
		if (UBlueprint* Blueprint = Cast<UBlueprint>(ActorClass->ClassGeneratedBy))
		{
			if (USkeletalMesh* Mesh = FindMeshFromBlueprint(Blueprint))
			{
				return Mesh;
			}
		}

		// Try native C++ components
		if (USkeletalMesh* Mesh = FindMeshFromCDO(ActorClass))
		{
			return Mesh;
		}

		// Try runtime component via FComponentReference
		UActorComponent* TargetComp = Component->TargetSkeletalMeshComponent.GetComponent(Owner);
		if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(TargetComp))
		{
			return SkelMeshComp->GetSkeletalMeshAsset();
		}

		// Fallback: Find by name from runtime components
		TArray<USkeletalMeshComponent*> SkelMeshComponents;
		Owner->GetComponents<USkeletalMeshComponent>(SkelMeshComponents);
		for (USkeletalMeshComponent* SkelMeshComp : SkelMeshComponents)
		{
			if (SkelMeshComp && SkelMeshComp->GetFName() == TargetPropertyName)
			{
				return SkelMeshComp->GetSkeletalMeshAsset();
			}
		}
	}

	return nullptr;
}

bool FFleshRingDetailCustomization::OnShouldFilterAsset(const FAssetData& AssetData) const
{
	// return true = filtered (hidden), return false = shown

	USkeletalMesh* OwnerMesh = GetOwnerSkeletalMesh();

	// If no target mesh selected, hide all assets (user must select target first)
	if (!OwnerMesh)
	{
		return true;
	}

	// Load Asset to check TargetSkeletalMesh
	UFleshRingAsset* Asset = Cast<UFleshRingAsset>(AssetData.GetAsset());
	if (!Asset)
	{
		return false;  // Show on load failure
	}

	// Always show Assets with no TargetSkeletalMesh set
	if (Asset->TargetSkeletalMesh.IsNull())
	{
		return false;
	}

	// Compare TargetSkeletalMesh with Owner's SkeletalMesh
	USkeletalMesh* AssetTargetMesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (!AssetTargetMesh)
	{
		return false;  // Show on load failure
	}

	// Match = show (false), mismatch = hide (true)
	return AssetTargetMesh != OwnerMesh;
}

TArray<USkeletalMeshComponent*> FFleshRingDetailCustomization::GetOwnerSkeletalMeshComponents() const
{
	TArray<USkeletalMeshComponent*> Result;

	UFleshRingComponent* Component = GetFirstSelectedComponent();
	if (!Component)
	{
		return Result;
	}

	// 1. Try Outer as UClass -> ClassGeneratedBy -> Blueprint
	if (UClass* OuterClass = Cast<UClass>(Component->GetOuter()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(OuterClass->ClassGeneratedBy))
		{
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
				for (USCS_Node* Node : AllNodes)
				{
					if (Node && Node->ComponentTemplate)
					{
						if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
						{
							Result.Add(SkelMeshComp);
						}
					}
				}
			}
			if (Result.Num() > 0)
			{
				return Result;
			}
		}
	}

	// 2. Try via Actor owner
	AActor* OwnerActor = Component->GetOwner();
	if (OwnerActor)
	{
		UClass* ActorClass = OwnerActor->GetClass();

		// Try Blueprint from class
		if (UBlueprint* Blueprint = Cast<UBlueprint>(ActorClass->ClassGeneratedBy))
		{
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
				for (USCS_Node* Node : AllNodes)
				{
					if (Node && Node->ComponentTemplate)
					{
						if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
						{
							Result.Add(SkelMeshComp);
						}
					}
				}
			}
			if (Result.Num() > 0)
			{
				return Result;
			}
		}

		// Fallback: Try GetComponents on owner (runtime/native components)
		OwnerActor->GetComponents<USkeletalMeshComponent>(Result);
	}

	return Result;
}

void FFleshRingDetailCustomization::RefreshTargetMeshOptions()
{
	TargetMeshOptions.Empty();
	CurrentTargetMeshSelection.Reset();

	// Add "None" option first
	TSharedPtr<FTargetMeshOption> NoneOption = MakeShared<FTargetMeshOption>();
	NoneOption->DisplayName = TEXT("None");
	NoneOption->ComponentProperty = NAME_None;
	TargetMeshOptions.Add(NoneOption);

	// Get current selection from component
	UFleshRingComponent* Component = GetFirstSelectedComponent();
	FName CurrentPropertyName = NAME_None;
	if (Component)
	{
		CurrentPropertyName = Component->TargetSkeletalMeshComponent.ComponentProperty;
	}

	// Default to None
	CurrentTargetMeshSelection = NoneOption;

	if (!Component)
	{
		return;
	}

	// Track added display names to avoid duplicates
	TSet<FString> AddedDisplayNames;
	bool bFoundCurrentSelection = CurrentPropertyName.IsNone();  // None is always found

	// Helper lambda to add option (with duplicate check by display name)
	auto AddOption = [this, &AddedDisplayNames, &CurrentPropertyName, &bFoundCurrentSelection](const FString& DisplayName, FName ComponentProperty)
	{
		if (AddedDisplayNames.Contains(DisplayName))
		{
			return;  // Skip duplicate
		}
		AddedDisplayNames.Add(DisplayName);

		TSharedPtr<FTargetMeshOption> Option = MakeShared<FTargetMeshOption>();
		Option->DisplayName = DisplayName;
		Option->ComponentProperty = ComponentProperty;
		TargetMeshOptions.Add(Option);

		if (ComponentProperty == CurrentPropertyName)
		{
			CurrentTargetMeshSelection = Option;
			bFoundCurrentSelection = true;
		}
	};

	// Helper lambda to add options from SCS (including inherited)
	auto AddOptionsFromBlueprint = [&AddOption](UBlueprint* Blueprint)
	{
		if (!Blueprint) return;

		// Walk up the Blueprint parent chain to get inherited components
		UBlueprint* CurrentBP = Blueprint;
		while (CurrentBP)
		{
			if (USimpleConstructionScript* SCS = CurrentBP->SimpleConstructionScript)
			{
				const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
				for (USCS_Node* Node : AllNodes)
				{
					if (!Node || !Node->ComponentTemplate)
					{
						continue;
					}

					if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
					{
						// Use variable name for both display and ComponentProperty
						// FComponentReference uses variable name to find runtime components
						FName VariableName = Node->GetVariableName();
						AddOption(VariableName.ToString(), VariableName);
					}
				}
			}

			// Move to parent Blueprint
			UClass* ParentClass = CurrentBP->ParentClass;
			CurrentBP = ParentClass ? Cast<UBlueprint>(ParentClass->ClassGeneratedBy) : nullptr;
		}
	};

	// Helper lambda to add C++ native components from CDO
	auto AddNativeComponentsFromCDO = [&AddOption](UClass* ActorClass)
	{
		if (!ActorClass) return;

		AActor* CDO = ActorClass->GetDefaultObject<AActor>();
		if (!CDO) return;

		TArray<USkeletalMeshComponent*> NativeComponents;
		CDO->GetComponents<USkeletalMeshComponent>(NativeComponents);
		for (USkeletalMeshComponent* SkelMeshComp : NativeComponents)
		{
			if (SkelMeshComp)
			{
				AddOption(SkelMeshComp->GetName(), SkelMeshComp->GetFName());
			}
		}
	};

	// 1. Try Outer as UClass -> ClassGeneratedBy -> Blueprint (SCS + inherited)
	if (UClass* OuterClass = Cast<UClass>(Component->GetOuter()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(OuterClass->ClassGeneratedBy))
		{
			AddOptionsFromBlueprint(Blueprint);
		}
		// Also check for C++ native components
		AddNativeComponentsFromCDO(OuterClass);
	}

	// 2. Try via Actor owner
	AActor* OwnerActor = Component->GetOwner();
	if (OwnerActor)
	{
		UClass* ActorClass = OwnerActor->GetClass();

		// Try Blueprint SCS (including parent chain)
		if (UBlueprint* Blueprint = Cast<UBlueprint>(ActorClass->ClassGeneratedBy))
		{
			AddOptionsFromBlueprint(Blueprint);
		}

		// Add C++ native components
		AddNativeComponentsFromCDO(ActorClass);

		// Also add runtime components (Level Editor instances, dynamically added components)
		TArray<USkeletalMeshComponent*> SkelMeshComponents;
		OwnerActor->GetComponents<USkeletalMeshComponent>(SkelMeshComponents);
		for (USkeletalMeshComponent* SkelMeshComp : SkelMeshComponents)
		{
			if (SkelMeshComp)
			{
				AddOption(SkelMeshComp->GetName(), SkelMeshComp->GetFName());
			}
		}
	}

	// 3. Handle invalid/renamed component (selection exists but not found)
	if (!bFoundCurrentSelection && !CurrentPropertyName.IsNone())
	{
		// Don't add to dropdown list - just track the invalid state
		bCurrentSelectionInvalid = true;
		InvalidComponentName = CurrentPropertyName;
		// Set to nullptr so clicking any option will trigger OnSelectionChanged
		CurrentTargetMeshSelection = nullptr;
	}
	else
	{
		bCurrentSelectionInvalid = false;
		InvalidComponentName = NAME_None;
	}
}

TSharedRef<SWidget> FFleshRingDetailCustomization::GenerateTargetMeshComboItem(TSharedPtr<FTargetMeshOption> InItem)
{
	return SNew(STextBlock)
		.Text(FText::FromString(InItem->DisplayName))
		.Font(IDetailLayoutBuilder::GetDetailFont());
}

void FFleshRingDetailCustomization::OnTargetMeshSelectionChanged(TSharedPtr<FTargetMeshOption> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid())
	{
		return;
	}

	CurrentTargetMeshSelection = NewSelection;

	// Clear invalid state when user selects a valid option
	bCurrentSelectionInvalid = false;
	InvalidComponentName = NAME_None;

	UFleshRingComponent* Component = GetFirstSelectedComponent();
	if (!Component)
	{
		return;
	}

	// Set the component property (use the internal FName, not the display name)
	Component->TargetSkeletalMeshComponent.ComponentProperty = NewSelection->ComponentProperty;

	// Force combo box to update its selected item display
	if (TargetMeshComboBox.IsValid())
	{
		TargetMeshComboBox->SetSelectedItem(NewSelection);
	}

	// Mark component as modified
	Component->Modify();
}

FText FFleshRingDetailCustomization::GetCurrentTargetMeshText() const
{
	// Check actual component state for most up-to-date display
	UFleshRingComponent* Component = GetFirstSelectedComponent();
	if (Component)
	{
		FName CurrentProperty = Component->TargetSkeletalMeshComponent.ComponentProperty;

		// None selected
		if (CurrentProperty.IsNone())
		{
			return LOCTEXT("SelectTarget", "None");
		}

		// Check if current property matches any valid option
		for (const TSharedPtr<FTargetMeshOption>& Option : TargetMeshOptions)
		{
			if (Option.IsValid() && Option->ComponentProperty == CurrentProperty)
			{
				return FText::FromString(Option->DisplayName);
			}
		}

		// Property exists but not in options - invalid
		return FText::Format(LOCTEXT("InvalidTarget", "⚠ {0} (Invalid)"), FText::FromName(CurrentProperty));
	}

	// Fallback to cached selection
	if (CurrentTargetMeshSelection.IsValid())
	{
		return FText::FromString(CurrentTargetMeshSelection->DisplayName);
	}
	return LOCTEXT("SelectTarget", "None");
}

#undef LOCTEXT_NAMESPACE

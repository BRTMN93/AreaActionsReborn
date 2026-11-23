#include "AreaActionsEquipment.h"
#include "AreaActionBase.h"
#include "AreaActionsBuildings.h"
#include "AreaActionsLog.h"
#include "AreaActionsWorldModule.h"
#include "Buildables/FGBuildable.h"
#include "InstanceData.h"

void AAreaActionsEquipment::ProcessEvent(UFunction* Function, void* Parameters)
{
	static const FName Equipped(TEXT("WasEquipped")), UnEquipped(TEXT("WasUnEquipped"));
	const FName Name = Function ? Function->GetFName() : NAME_None;
	if (Name != Equipped && Name != UnEquipped)
	{
		Super::ProcessEvent(Function, Parameters);
		return;
	}
	// On taking the tool away the instigator may be gone already, so the answer of WasEquipped is kept
	const bool bLocal = Name == Equipped ? IsLocalInstigator() : bEquippedLocally;
	bEquippedLocally = Name == Equipped && bLocal;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Tool %s: %s player's, net mode %d"), *Name.ToString(), bLocal ? TEXT("this") : TEXT("another"),
		GetWorld() ? static_cast<int32>(GetWorld()->GetNetMode()) : -1);
	if (bLocal)
	{
		Super::ProcessEvent(Function, Parameters);
	}
	else if (Name == Equipped)
	{
		AFGEquipment::WasEquipped_Implementation();
	}
	else
	{
		AFGEquipment::WasUnEquipped_Implementation();
	}
}

bool AAreaActionsEquipment::GetLocalBox(const AFGBuildable* Buildable, FBox& OutBox)
{
	OutBox.Init();
	if (!Buildable)
	{
		return false;
	}
	OutBox = Buildable->GetCombinedClearanceBox();
	if (OutBox.IsValid)
	{
		return true;
	}
	if (AreaActionsBuildings::ShouldIgnore(Buildable->GetClass()))
	{
		return false;
	}
	// The class default stands at the origin, so its bounds are local
	const AActor* Default = Buildable->GetClass()->GetDefaultObject<AActor>();
	FVector Origin, Extent;
	Default->GetActorBounds(true, Origin, Extent, false);
	if (!Extent.IsNearlyZero())
	{
		OutBox += FBox(Origin - Extent, Origin + Extent);
	}
	if (const UAbstractInstanceDataObject* Data = Buildable->GetLightweightInstanceData())
	{
		for (const FInstanceData& Instance : Data->GetInstanceData())
		{
			if (Instance.StaticMesh)
			{
				OutBox += Instance.StaticMesh->GetBoundingBox().TransformBy(Instance.RelativeTransform);
			}
		}
	}
	return OutBox.IsValid != 0;
}

void AAreaActionsEquipment::GetActorCorners(AFGBuildable* Buildable, TArray<FVector>& Corners)
{
	Corners.Reset();
	FBox Box;
	if (!GetLocalBox(Buildable, Box))
	{
		return;
	}
	const FTransform Transform = Buildable->GetActorTransform();
	const FVector Center = Box.GetCenter();
	const FVector Extent = Box.GetExtent();
	// Gray code order, as the Blueprint's GetBoxCorners: neighbours differ in one axis, so the first four corners (the top face, +Z) go round
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const int32 Gray = Index ^ (Index >> 1);
		const FVector Sign((Gray & 1) ? -1.0 : 1.0, (Gray & 2) ? -1.0 : 1.0, (Gray & 4) ? -1.0 : 1.0);
		Corners.Add(Transform.TransformPosition(Center + Extent * Sign));
	}
}

void AAreaActionsEquipment::GetAvailableActions(TArray<TSubclassOf<AAreaActionBase>>& Actions)
{
	const UAreaActionsWorldModule* Module = UAreaActionsWorldModule::Get(this);
	Actions = Module ? Module->AvailableActions : TArray<TSubclassOf<AAreaActionBase>>();
}

bool AAreaActionsEquipment::GetArea(FAreaActionsArea& Out) const
{
	const FArrayProperty* Corners = FindFProperty<FArrayProperty>(GetClass(), TEXT("CornerLocations"));
	const FDoubleProperty* MinZ = FindFProperty<FDoubleProperty>(GetClass(), TEXT("MinZ"));
	const FDoubleProperty* MaxZ = FindFProperty<FDoubleProperty>(GetClass(), TEXT("MaxZ"));
	if (!Corners || !CastField<FStructProperty>(Corners->Inner) || CastField<FStructProperty>(Corners->Inner)->Struct != TBaseStructure<FVector>::Get() || !MinZ || !MaxZ)
	{
		return false;
	}
	Out.Polygon.Reset();
	for (const FVector& Corner : *Corners->ContainerPtrToValuePtr<TArray<FVector>>(this))
	{
		Out.Polygon.Add(FVector2D(Corner));
	}
	// The Blueprint's "no limit" is its +Inf/-Inf variables; a request carries it as +/-UE_BIG_NUMBER
	Out.MinZ = FMath::Clamp(MinZ->GetPropertyValue_InContainer(this), -UE_BIG_NUMBER, UE_BIG_NUMBER);
	Out.MaxZ = FMath::Clamp(MaxZ->GetPropertyValue_InContainer(this), -UE_BIG_NUMBER, UE_BIG_NUMBER);
	return true;
}

bool AAreaActionsEquipment::RaycastMouse(FHitResult& Out)
{
	UFunction* Raycast = FindFunction(TEXT("RaycastMouseWithRange"));
	const FStructProperty* Hit = Raycast ? FindFProperty<FStructProperty>(Raycast, TEXT("Hit")) : nullptr;
	const FBoolProperty* Success = Raycast ? FindFProperty<FBoolProperty>(Raycast, TEXT("Success")) : nullptr;
	if (!Hit || Hit->Struct != FHitResult::StaticStruct() || !Success)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: no RaycastMouseWithRange(...) -> Hit, Success"), *GetClass()->GetName());
		return false;
	}
	// The parameters are a struct of their own: construct them, so the hit result starts valid
	uint8* Params = static_cast<uint8*>(FMemory_Alloca(Raycast->ParmsSize));
	FMemory::Memzero(Params, Raycast->ParmsSize);
	for (TFieldIterator<FProperty> It(Raycast); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Params);
		if (FBoolProperty* Flag = CastField<FBoolProperty>(*It); Flag && Flag != Success && Flag->GetName().StartsWith(TEXT("Ignore")))
		{
			Flag->SetPropertyValue_InContainer(Params, true);
		}
	}
	ProcessEvent(Raycast, Params);
	const bool bHit = Success->GetPropertyValue_InContainer(Params);
	Out = *Hit->ContainerPtrToValuePtr<FHitResult>(Params);
	for (TFieldIterator<FProperty> It(Raycast); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Params);
	}
	return bHit;
}

void AAreaActionsEquipment::SelectBuilding()
{
	FHitResult Hit;
	AreaActionsBuildings::FSnapshot Building;
	if (!RaycastMouse(Hit) || !AreaActionsBuildings::FromHit(GetWorld(), Hit, Building))
	{
		return;
	}
	TArray<FVector> Box;
	AreaActionsBuildings::GetCorners(Building, Box);
	FArrayProperty* Corners = FindFProperty<FArrayProperty>(GetClass(), TEXT("CornerLocations"));
	FDoubleProperty* MinZ = FindFProperty<FDoubleProperty>(GetClass(), TEXT("MinZ"));
	FDoubleProperty* MaxZ = FindFProperty<FDoubleProperty>(GetClass(), TEXT("MaxZ"));
	UFunction* Clear = FindFunction(TEXT("Clear"));
	UFunction* UpdateIndicators = FindFunction(TEXT("UpdateIndicators"));
	const FStructProperty* Inner = Corners ? CastField<FStructProperty>(Corners->Inner) : nullptr;
	if (!Inner || Inner->Struct != TBaseStructure<FVector>::Get() || !MinZ || !MaxZ || !Clear || Clear->ParmsSize || !UpdateIndicators || UpdateIndicators->ParmsSize)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: SelectBuilding needs CornerLocations, MinZ, MaxZ, Clear() and UpdateIndicators()"), *GetClass()->GetName());
		return;
	}
	if (Box.IsEmpty())
	{
		UE_LOG(LogAreaActionsCpp, Display, TEXT("SelectBuilding: %s has no box"), *GetNameSafe(Building.Class));
		return;
	}
	// The original's SelectBuildingData: the corners without their height (the top face), the box's lowest and highest point
	TArray<FVector> Flat;
	double Low = TNumericLimits<double>::Max(), High = TNumericLimits<double>::Lowest();
	for (const FVector& Corner : Box)
	{
		Flat.AddUnique(FVector(Corner.X, Corner.Y, 0.0));
		Low = FMath::Min(Low, Corner.Z);
		High = FMath::Max(High, Corner.Z);
	}
	ProcessEvent(Clear, nullptr);
	*Corners->ContainerPtrToValuePtr<TArray<FVector>>(this) = Flat;
	MinZ->SetPropertyValue_InContainer(this, Low);
	MaxZ->SetPropertyValue_InContainer(this, High);
	ProcessEvent(UpdateIndicators, nullptr);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("SelectBuilding: %s, %d corners, Z %.0f..%.0f"), *GetNameSafe(Building.Class), Flat.Num(), Low, High);
}

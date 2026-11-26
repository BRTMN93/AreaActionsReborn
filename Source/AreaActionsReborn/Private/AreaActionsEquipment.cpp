#include "AreaActionsEquipment.h"
#include "AreaActionBase.h"
#include "AreaActionsBuildings.h"
#include "AreaActionsLog.h"
#include "AreaActionsWorldModule.h"
#include "Buildables/FGBuildable.h"
#include "InstanceData.h"
#include "Misc/CommandLine.h"
#include "Net/Core/Misc/NetContext.h"
#include "AreaActionsRCO.h"
#include "Equipment/FGBuildGun.h"
#include "FGCharacterPlayer.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Resources/FGBuildingDescriptor.h"

static const FName InitOnServerName(TEXT("InitOnServer"));
static const FName ToggleFlyName(TEXT("ToggleFly"));
static const FName WasEquippedName(TEXT("WasEquipped"));

int32 AAreaActionsEquipment::GetFunctionCallspace(UFunction* Function, FFrame* Stack)
{
	if (Function && Function->GetFName() == InitOnServerName)
	{
		// Only the tool's own player starts an action, and only while the tool's Blueprint side runs for that player (AA-096 NEW-2: the
		// answer of WasEquipped, not who the instigator is at the moment of the click); a call for anybody else's tool does nothing
		return bEquippedLocally ? FunctionCallspace::Local : FunctionCallspace::Absorbed;
	}
	if (Function && Function->GetFName() == ToggleFlyName)
	{
		// The Blueprint's ToggleFly ("Run on Server") spawned the original's FlyHandler on the server; the menu's button flies natively
		// (ToggleFlying), so the Blueprint's event never runs
		return FunctionCallspace::Absorbed;
	}
	return Super::GetFunctionCallspace(Function, Stack);
}

void AAreaActionsEquipment::ApplyFlying(ACharacter* Character, bool bFly)
{
	UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
	if (!Movement)
	{
		return;
	}
	// The engine's cheat flying: the character stays in the air when it touches the ground; the game's movement flies with the usual keys
	Movement->bCheatFlying = bFly;
	Movement->SetMovementMode(bFly ? MOVE_Flying : MOVE_Falling);
}

bool AAreaActionsEquipment::IsFlying(const ACharacter* Character)
{
	const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
	return Movement && Movement->MovementMode == MOVE_Flying;
}

void AAreaActionsEquipment::ToggleFlying()
{
	ACharacter* Character = Cast<ACharacter>(GetInstigator());
	UAreaActionsRCO* Rco = UAreaActionsRCO::Of(Character ? Cast<APlayerController>(Character->GetController()) : nullptr);
	if (!bEquippedLocally || !Rco)
	{
		return;
	}
	const bool bFly = !IsFlying(Character);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Tool: flying %s"), bFly ? TEXT("on") : TEXT("off"));
	// A client moves its character itself and the server takes its moves once the server's copy flies as well
	if (!Character->HasAuthority())
	{
		ApplyFlying(Character, bFly);
	}
	Rco->ServerSetFlying(bFly);
}

TSubclassOf<UFGRecipe> AAreaActionsEquipment::FindBuildRecipe(UWorld* World, const AreaActionsBuildings::FSnapshot& Building, TSubclassOf<UObject> BuildGun)
{
	AFGRecipeManager* Recipes = AFGRecipeManager::Get(World);
	if (!Recipes || !Building.Class)
	{
		return nullptr;
	}
	auto Builds = [&Building](TSubclassOf<UFGRecipe> Recipe)
	{
		for (const FItemAmount& Product : Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>())
		{
			const TSubclassOf<UFGBuildingDescriptor> Descriptor(Product.ItemClass.Get());
			if (Descriptor && UFGBuildingDescriptor::GetBuildableClass(Descriptor) == Building.Class)
			{
				return true;
			}
		}
		return false;
	};
	// The recipe it was built with first (a foundation's material), then any the Build Gun has that builds the class
	const TSubclassOf<UFGRecipe> BuiltWith = AreaActionsBuildings::GetRecipe(World, Building);
	if (BuiltWith && Recipes->IsRecipeAvailable(BuiltWith) && Builds(BuiltWith))
	{
		return BuiltWith;
	}
	TArray<TSubclassOf<UFGRecipe>> Available;
	Recipes->GetAvailableRecipesForProducer(BuildGun, Available);
	const TSubclassOf<UFGRecipe>* Found = Available.FindByPredicate(Builds);
	return Found ? *Found : nullptr;
}

void AAreaActionsEquipment::SampleBuilding()
{
	FHitResult Hit;
	AreaActionsBuildings::FSnapshot Building;
	AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(GetInstigator());
	AFGBuildGun* BuildGun = Character ? Character->GetBuildGun() : nullptr;
	if (!BuildGun || !RaycastMouse(Hit) || !AreaActionsBuildings::FromHit(GetWorld(), Hit, Building))
	{
		UE_LOG(LogAreaActionsCpp, Display, TEXT("Tool: middle mouse button, no building under the mouse"));
		return;
	}
	const TSubclassOf<UFGRecipe> Recipe = FindBuildRecipe(GetWorld(), Building, BuildGun->GetClass());
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Tool: middle mouse button on %s (%s): recipe %s"), *GetNameSafe(Building.Class), Building.Actor.IsValid() ? TEXT("actor") : TEXT("lightweight"),
		*GetNameSafe(Recipe));
	if (Recipe)
	{
		// As the original: the Build Gun gets the recipe, then goes into the hand
		BuildGun->GotoBuildState(Recipe);
		Character->ToggleBuildGun();
	}
}

void AAreaActionsEquipment::WasEquipped_Implementation()
{
	if (!bSkipNativeEquip)
	{
		Super::WasEquipped_Implementation();
	}
}

void AAreaActionsEquipment::ProcessEvent(UFunction* Function, void* Parameters)
{
	static const FName UnEquippedName(TEXT("WasUnEquipped")), TickName(TEXT("ReceiveTick"));
	const FName Name = Function ? Function->GetFName() : NAME_None;
	// Only for the log lines (AA-096 NEW-3)
	auto NetMode = [this]() { return GetWorld() ? static_cast<int32>(GetWorld()->GetNetMode()) : -1; };
	// The Blueprint's graph calls InitOnServer without ProcessEvent; one that came over the network is a client asking the server to
	// spawn an actor of a class it names
	if (Name == InitOnServerName && UE::Net::FNetContext::IsInsideNetRPC())
	{
		if (!bRejectLogged)
		{
			bRejectLogged = true;
			UE_LOG(LogAreaActionsCpp, Warning, TEXT("reject InitOnServer sent over the network for the tool %s (said once per tool)"), *GetName());
		}
		return;
	}
	// The input events' names carry a generated suffix; the tick, every frame, skips making the string
	const FString Text = Name == TickName ? FString() : Name.ToString();
	// The Blueprint's InputKey MiddleMouseButton (an input binding calls it through here): the original only knew actors, the building under
	// the mouse here is an actor or a lightweight
	if (Text.StartsWith(TEXT("InpActEvt_MiddleMouseButton")))
	{
		if (bEquippedLocally)
		{
			SampleBuilding();
		}
		return;
	}
	// The Blueprint's InputKey LeftMouseButton places or deletes a corner at the end of the list, and its walls join the corners in that
	// order: corners placed out of order around the area crossed into an X (AA-074). Crossing walls are untangled.
	if (Text.StartsWith(TEXT("InpActEvt_LeftMouseButton")))
	{
		Super::ProcessEvent(Function, Parameters);
		if (bEquippedLocally)
		{
			OrderCorners();
		}
		return;
	}
	if (Name == TickName)
	{
		if (!bEquippedLocally && bEquipPending && IsEquipped() && IsLocalInstigator())
		{
			// The game called WasEquipped before this player's character was the local one (it had no controller yet): the Blueprint's
			// part runs now. The game's own part ran then, so the graph's call of it is skipped. Only for a WasEquipped with no
			// WasUnEquipped after it (AA-096 NEW-1): a tool put away must not open its windows again.
			bEquippedLocally = true;
			bEquipPending = false;
			UE_LOG(LogAreaActionsCpp, Log, TEXT("Tool WasEquipped: this player's, late, net mode %d"), NetMode());
			if (UFunction* Graph = FindFunction(WasEquippedName))
			{
				TGuardValue<bool> Guard(bSkipNativeEquip, true);
				Super::ProcessEvent(Graph, nullptr);
			}
		}
		// The Blueprint's Tick shows the mouse cursor for the tool's windows and switches the tool's input with photo mode, all for this
		// game's own player: it must not run for a tool another player holds
		if (bEquippedLocally)
		{
			Super::ProcessEvent(Function, Parameters);
		}
		return;
	}
	if (Name != WasEquippedName && Name != UnEquippedName)
	{
		Super::ProcessEvent(Function, Parameters);
		return;
	}
	bool bLocal = Name == WasEquippedName ? IsLocalInstigator() : bEquippedLocally;
#if AA_WITH_TESTS
	// The test mode's stand-in for a WasEquipped that comes before the character is the local one
	static const bool bLateEquip = FParse::Param(FCommandLine::Get(), TEXT("AATestLateEquip")) && FCString::Stristr(FCommandLine::Get(), TEXT("-AATest=")) != nullptr;
	bLocal &= Name != WasEquippedName || !bLateEquip;
#endif
	// On taking the tool away the instigator may be gone already, so the answer of WasEquipped is kept
	bEquippedLocally = Name == WasEquippedName && bLocal;
	// A WasEquipped for a character not yet the local one may turn out to be this player's on a later tick
	bEquipPending = Name == WasEquippedName && !bLocal;
	UE_LOG(LogAreaActionsCpp, Log, TEXT("Tool %s: %s player's, net mode %d"), *Name.ToString(), bLocal ? TEXT("this") : TEXT("another"), NetMode());
	if (bLocal)
	{
		Super::ProcessEvent(Function, Parameters);
	}
	else if (Name == WasEquippedName)
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

bool AAreaActionsEquipment::OrderCorners()
{
	FArrayProperty* Corners = FindFProperty<FArrayProperty>(GetClass(), TEXT("CornerLocations"));
	FDoubleProperty* MinZ = FindFProperty<FDoubleProperty>(GetClass(), TEXT("MinZ"));
	FDoubleProperty* MaxZ = FindFProperty<FDoubleProperty>(GetClass(), TEXT("MaxZ"));
	UFunction* Clear = FindFunction(TEXT("Clear"));
	UFunction* UpdateIndicators = FindFunction(TEXT("UpdateIndicators"));
	const FStructProperty* Inner = Corners ? CastField<FStructProperty>(Corners->Inner) : nullptr;
	if (!Inner || Inner->Struct != TBaseStructure<FVector>::Get() || !MinZ || !MaxZ || !Clear || Clear->ParmsSize || !UpdateIndicators || UpdateIndicators->ParmsSize)
	{
		return false;
	}
	TArray<FVector>& List = *Corners->ContainerPtrToValuePtr<TArray<FVector>>(this);
	if (List.Num() < 4)
	{
		// Three corners or fewer never cross
		return false;
	}
	// Walls that do not cross stay as the player drew them (a concave L stays an L); only a crossing changes the order
	auto Crosses = [](const TArray<FVector>& Points)
	{
		const int32 N = Points.Num();
		for (int32 I = 0; I < N; ++I)
		{
			for (int32 J = I + 2; J < N; ++J)
			{
				if ((J + 1) % N == I)
				{
					continue;
				}
				FVector Point;
				if (FMath::SegmentIntersection2D(FVector(FVector2D(Points[I]), 0.0), FVector(FVector2D(Points[(I + 1) % N]), 0.0),
					FVector(FVector2D(Points[J]), 0.0), FVector(FVector2D(Points[(J + 1) % N]), 0.0), Point))
				{
					return true;
				}
			}
		}
		return false;
	};
	if (!Crosses(List))
	{
		return false;
	}
	// Sorted by angle around a point inside them the corners make a polygon whose edges never cross (a star-shaped one)
	FVector2D Centre = FVector2D::ZeroVector;
	for (const FVector& Corner : List)
	{
		Centre += FVector2D(Corner);
	}
	Centre /= List.Num();
	TArray<FVector> Ordered = List;
	Ordered.StableSort([&Centre](const FVector& A, const FVector& B)
	{
		return FMath::Atan2(A.Y - Centre.Y, A.X - Centre.X) < FMath::Atan2(B.Y - Centre.Y, B.X - Centre.X);
	});
	const int32 Count = List.Num();
	// As SelectBuilding: the Blueprint's Clear takes the indicators and the height limits away, UpdateIndicators draws them again
	const double Low = MinZ->GetPropertyValue_InContainer(this), High = MaxZ->GetPropertyValue_InContainer(this);
	ProcessEvent(Clear, nullptr);
	*Corners->ContainerPtrToValuePtr<TArray<FVector>>(this) = Ordered;
	MinZ->SetPropertyValue_InContainer(this, Low);
	MaxZ->SetPropertyValue_InContainer(this, High);
	ProcessEvent(UpdateIndicators, nullptr);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Corners put in order around the area: %d"), Count);
	return true;
}

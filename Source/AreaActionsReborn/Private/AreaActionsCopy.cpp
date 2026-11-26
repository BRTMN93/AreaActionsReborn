#include "AreaActionsCopy.h"
#include "AreaActionsWindows.h"
#include "AkComponent.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsJobs.h"
#include "AreaActionsLog.h"
#include "AreaActionsSettings.h"

static const TCHAR* UseBuildCostKey = TEXT("UseBuildCost");
static const TCHAR* ContainerDistanceKey = TEXT("ContainerDistance");
#include "AreaActionsRCO.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Components/CheckBox.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "FGFactorySettings.h"
#include "HAL/IConsoleManager.h"
#include "Hologram/FGHologram.h"
#include "Kismet/GameplayStatics.h"
#include "Resources/FGItemDescriptor.h"

#define LOCTEXT_NAMESPACE "AreaActions"

int32 AAreaActionsPreview::LiveGhosts = 0;

static TAutoConsoleVariable<int32> CVarPreviewHolograms(TEXT("AreaActions.PreviewHolograms"), 300,
	TEXT("Most copies the Copy and Fill preview shows as the game's own holograms (each is an actor); the rest are instanced meshes"));

namespace
{
	template <typename T>
	T* StructParam(UFunction* Function, uint8* Params, const TCHAR* Name)
	{
		const FStructProperty* Property = FindFProperty<FStructProperty>(Function, Name);
		return Property && Property->Struct == TBaseStructure<T>::Get() ? Property->ContainerPtrToValuePtr<T>(Params) : nullptr;
	}


	// The original's Remap0To1AndNegative: the cell the click is in, counted as the window counts copies
	int32 Remap(int32 Cells)
	{
		return Cells > 0 ? Cells + 1 : Cells - 1;
	}

	bool SamePlace(const AreaActionsBuildings::FSnapshot& A, const AreaActionsBuildings::FSnapshot& B)
	{
		return A.Class == B.Class && FVector::Dist(A.Transform.GetLocation(), B.Transform.GetLocation()) <= 1.0;
	}

	// The hologram material takes its colour from the custom depth stencil, which only the protected SetMaterialState sets; the public
	// SetPlacementMaterialState skips a state equal to the current one, and a fresh hologram's state is already HMS_OK. Never instantiated.
	struct FHologramAccess : AFGHologram
	{
		static void SetState(AFGHologram* Hologram, EHologramMaterialState State) { (Hologram->*(&FHologramAccess::SetMaterialState))(State); }
		static uint8 GetStencil(const AFGHologram* Hologram, EHologramMaterialState State) { return (Hologram->*(&FHologramAccess::GetStencilForHologramMaterialState))(State); }
	};
}

uint8 AAreaActionsPreview::GetValidStencil()
{
	return FHologramAccess::GetStencil(GetDefault<AFGHologram>(), EHologramMaterialState::HMS_OK);
}

AAreaActionsPreview::AAreaActionsPreview()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AAreaActionsPreview::Show(const TArray<AreaActionsBuildings::FSnapshot>& Buildings, const TArray<AreaActionsBuildings::FPlanned>& Copies)
{
	Clear();
	const UFGFactorySettings* Settings = UFGFactorySettings::Get();
	UMaterialInterface* Material = Settings ? Settings->mDefaultValidPlacementMaterial.Get() : nullptr;
	const uint8 Stencil = GetValidStencil();
	TMap<int32, TArray<TPair<UStaticMesh*, FTransform>>> Parts;
	const int32 MaxHolograms = FMath::Max(0, CVarPreviewHolograms.GetValueOnGameThread());
	const APlayerController* Player = GetOwner() ? Cast<APlayerController>(GetOwner()->GetOwner()) : nullptr;
	APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	for (const AreaActionsBuildings::FPlanned& Copy : Copies)
	{
		const AreaActionsBuildings::FSnapshot& Source = Buildings[Copy.Source];
		// The game's hologram shows the class at its defaults: a building with data of its own (a beam's length) keeps the meshes
		const TSubclassOf<UFGRecipe> Recipe = Holograms.Num() < MaxHolograms && !Source.TypeData.IsValid() ? AreaActionsBuildings::GetRecipe(GetWorld(), Source) : nullptr;
		AFGHologram* Hologram = Recipe ? AFGHologram::SpawnHologramFromRecipe(Recipe, this, Copy.Transform.GetLocation(), Pawn) : nullptr;
		if (Hologram && !Hologram->GetHologramChildren().IsEmpty())
		{
			// Only the build gun places a composite hologram's children: the copy takes the meshes
			for (AFGHologram* Child : Hologram->GetHologramChildren())
			{
				if (IsValid(Child))
				{
					Child->Destroy();
				}
			}
			Hologram->Destroy();
			Hologram = nullptr;
		}
		if (Hologram)
		{
			// Only this player's: not sent to clients from a host, nothing hits it or overlaps it
			Hologram->SetReplicates(false);
			GetWorld()->RemoveNetworkActor(Hologram);
			Hologram->SetActorEnableCollision(false);
			Hologram->SetActorTransform(Copy.Transform);
			// Nothing moves it: the build gun's updates and placement checks stay off
			Hologram->SetActorTickEnabled(false);
			FHologramAccess::SetState(Hologram, EHologramMaterialState::HMS_OK);
			// The game gives a hologram the build gun's loop sound; a preview is silent
			TInlineComponentArray<UAkComponent*> Sounds(Hologram);
			for (UAkComponent* Sound : Sounds)
			{
				Sound->Stop();
			}
			Holograms.Add(Hologram);
			++Ghosts;
			continue;
		}
		TArray<TPair<UStaticMesh*, FTransform>>* Own = Parts.Find(Copy.Source);
		if (!Own)
		{
			Own = &Parts.Add(Copy.Source);
			AreaActionsBuildings::GetMeshes(GetWorld(), Source, *Own);
		}
		// A building without meshes of its own (ladder, foundation passthrough, vehicle path, elevator, blueprint designer) has no box
		// either, in the world or on its class default (AA-091), so its copy shows nothing
		for (const TPair<UStaticMesh*, FTransform>& Part : *Own)
		{
			TObjectPtr<UInstancedStaticMeshComponent>& Component = Meshes.FindOrAdd(Part.Key);
			if (!Component)
			{
				Component = NewObject<UInstancedStaticMeshComponent>(this);
				Component->SetStaticMesh(Part.Key);
				Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Component->SetCastShadow(false);
				// The colour of the build gun's valid hologram (see FHologramAccess)
				Component->SetRenderCustomDepth(true);
				Component->SetCustomDepthStencilValue(Stencil);
				for (int32 Slot = 0; Material && Slot < Component->GetNumMaterials(); ++Slot)
				{
					Component->SetMaterial(Slot, Material);
				}
				Component->SetupAttachment(RootComponent);
				Component->RegisterComponent();
			}
			Component->AddInstance(Part.Value * Copy.Transform, true);
		}
		++Ghosts;
	}
	LiveGhosts += Ghosts;
}

void AAreaActionsPreview::Clear()
{
	for (const TPair<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>>& Entry : Meshes)
	{
		if (Entry.Value)
		{
			Entry.Value->DestroyComponent();
		}
	}
	Meshes.Reset();
	for (AFGHologram* Hologram : Holograms)
	{
		if (IsValid(Hologram))
		{
			Hologram->Destroy();
		}
	}
	Holograms.Reset();
	LiveGhosts -= Ghosts;
	Ghosts = 0;
}

void AAreaActionsPreview::EndPlay(const EEndPlayReason::Type Reason)
{
	Clear();
	Super::EndPlay(Reason);
}

void UAreaActionPlaceCopies::Start()
{
	AAreaActionBase* Shell = GetShell();
	AreaActionsBuildings::CollectAll(Shell->GetWorld(), Shell->GetArea(), Buildings);
	if (Buildings.IsEmpty())
	{
		Shell->ShowMessage(IsFill() ? LOCTEXT("FillNoneTitle", "Couldn't fill") : LOCTEXT("CopyNoneTitle", "Couldn't copy"), LOCTEXT("NoBuildings", "No buildings in the area"));
		return;
	}
	Frame = AreaActionsBuildings::ComputeFrame(Buildings);
	// The action shows the area's axes above its buildings (Fill's ramp arrows), as the original's InitLocationAndRotation
	Shell->SetActorLocationAndRotation(FVector(Frame.Position.X, Frame.Position.Y, Frame.MaxZ + 200.0), FRotator(0.0, Frame.Yaw, 0.0));
	// The window reads the settings actor when it is made: the player's own settings go into it first (D21)
	LoadSettings();
	Window = Shell->OpenWindow(Shell->ParamsWidgetClass);
	if (!Window || !BindWindow() || !AAreaActionBase::BindWindowEvent(Window, TEXT("WidgetCanceled"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionPlaceCopies, Canceled)))
	{
		Shell->Finish();
		return;
	}
	const AActor* Settings = GetSettings();
	const FBoolProperty* UseCost = Settings ? FindFProperty<FBoolProperty>(Settings->GetClass(), TEXT("UseBuildCost")) : nullptr;
	if (UAreaActionsPlaceWindow* Place = Cast<UAreaActionsPlaceWindow>(Window); Place && UseCost)
	{
		Place->SetUseCost(UseCost->GetPropertyValue_InContainer(Settings));
	}
	UE_LOG(LogAreaActionsCpp, Display, TEXT("%s: %d buildings, reference %s, yaw %.2f, pattern %s"), *GetClass()->GetName(), Buildings.Num(), *Frame.Position.ToString(), Frame.Yaw,
		*Frame.PatternSize.ToString());
}

void UAreaActionPlaceCopies::PlanCopies(const FAreaActionsPlacement& Placement, TArray<AreaActionsBuildings::FPlanned>& Out) const
{
	TArray<AreaActionsBuildings::FPlanned> Planned;
	AreaActionsBuildings::Plan(Buildings, Frame, Placement, AreaActionsJobs::GetMaxSpawn(), Planned);
	TSet<UClass*> Classes;
	for (const AreaActionsBuildings::FPlanned& Copy : Planned)
	{
		Classes.Add(Buildings[Copy.Source].Class);
	}
	AreaActionsBuildings::FExisting Existing;
	Existing.Build(GetShell()->GetWorld(), Classes);
	Out.Reset();
	for (const AreaActionsBuildings::FPlanned& Copy : Planned)
	{
		UClass* Class = Buildings[Copy.Source].Class;
		if (!Existing.Contains(Class, Copy.Transform))
		{
			Existing.Add(Class, Copy.Transform);
			Out.Add(Copy);
		}
	}
}

void UAreaActionPlaceCopies::ShowPreview(const FAreaActionsPlacement& Placement)
{
	AAreaActionBase* Shell = GetActiveShell();
	if (!Shell)
	{
		return;
	}
	TArray<AreaActionsBuildings::FPlanned> Copies;
	PlanCopies(Placement, Copies);
	if (!Preview)
	{
		FActorSpawnParameters Parameters;
		Parameters.Owner = Shell;
		Preview = Shell->GetWorld()->SpawnActor<AAreaActionsPreview>(Parameters);
	}
	if (Preview)
	{
		Preview->Show(Buildings, Copies);
		UE_LOG(LogAreaActionsCpp, Display, TEXT("%s preview: %d ghosts, %d of them holograms"), *GetClass()->GetName(), Preview->GetGhosts(), Preview->GetHolograms());
	}
}

void UAreaActionPlaceCopies::Send(const FAreaActionsPlacement& Placement, bool bUseCost)
{
	AAreaActionBase* Shell = GetActiveShell();
	UAreaActionsRCO* Rco = Shell ? Shell->GetRco() : nullptr;
	if (!Rco)
	{
		return;
	}
	const int32 Id = Shell->BeginRequest();
	if (Id == INDEX_NONE)
	{
		return;
	}
	// The ghosts go: the copies themselves come
	if (Preview)
	{
		Preview->Clear();
	}
	AActor* Settings = GetSettings();
	float Distance = 0.0f;
	if (Settings)
	{
		if (const FBoolProperty* UseCost = FindFProperty<FBoolProperty>(Settings->GetClass(), TEXT("UseBuildCost")))
		{
			UseCost->SetPropertyValue_InContainer(Settings, bUseCost);
		}
		if (const FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("ContainerDistance")))
		{
			Distance = static_cast<float>(Property->GetPropertyValue_InContainer(Settings));
		}
	}
	AreaActionsSettings::SetBool(UseBuildCostKey, bUseCost);
	AreaActionsSettings::SetDouble(ContainerDistanceKey, Distance);
	Rco->ServerPlaceCopies(Id, Shell->GetArea(), Placement, bUseCost, Distance);
}

void UAreaActionPlaceCopies::LoadSettings() const
{
	AActor* Settings = GetSettings();
	if (!Settings)
	{
		return;
	}
	// Without a file the settings actor's own defaults stay (the original's)
	if (const FBoolProperty* UseCost = FindFProperty<FBoolProperty>(Settings->GetClass(), TEXT("UseBuildCost")))
	{
		UseCost->SetPropertyValue_InContainer(Settings, AreaActionsSettings::GetBool(UseBuildCostKey, UseCost->GetPropertyValue_InContainer(Settings)));
	}
	if (const FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Settings->GetClass(), TEXT("ContainerDistance")))
	{
		Property->SetPropertyValue_InContainer(Settings, FMath::Max(0.0, AreaActionsSettings::GetDouble(ContainerDistanceKey, Property->GetPropertyValue_InContainer(Settings))));
	}
}

void UAreaActionPlaceCopies::Canceled()
{
	if (AAreaActionBase* Shell = GetActiveShell())
	{
		End();
		Shell->Finish();
	}
}

void UAreaActionPlaceCopies::End()
{
	if (Preview)
	{
		Preview->Destroy();
		Preview = nullptr;
	}
}

void UAreaActionPlaceCopies::OnResult(const FAreaActionsResult& Result)
{
	AAreaActionBase* Shell = GetShell();
	if (Shell->ShowFailure(Result.Code))
	{
		return;
	}
	const bool bFill = IsFill();
	const FText Failed = bFill ? LOCTEXT("FillNoneTitle", "Couldn't fill") : LOCTEXT("CopyNoneTitle", "Couldn't copy");
	switch (Result.Code)
	{
	case EAreaActionsResultCode::Ok:
		Shell->ShowMessage(bFill ? LOCTEXT("FillDone", "Fill complete") : LOCTEXT("CopyDone", "Copy complete"),
			FText::Format(bFill ? LOCTEXT("Filled", "Filled with {Count} buildings.") : LOCTEXT("Copied", "Copied {Count} buildings."), FFormatNamedArguments{ { TEXT("Count"), Result.Buildings } }));
		return;
	case EAreaActionsResultCode::NothingInArea:
		Shell->ShowMessage(Failed, LOCTEXT("NoBuildings", "No buildings in the area"));
		return;
	case EAreaActionsResultCode::MissingItems:
	{
		// "Missing: 3 Iron Plates, 1 Concrete", as the original
		TArray<FString> Parts;
		for (const FAreaActionsClassCount& Entry : Result.Missing)
		{
			const TSubclassOf<UFGItemDescriptor> Item(Entry.Class.Get());
			Parts.Add(FString::Printf(TEXT("%d %s%s"), Entry.Count, *UFGItemDescriptor::GetItemName(Item).ToString(), Entry.Count > 1 ? TEXT("s") : TEXT("")));
		}
		Shell->ShowMessage(bFill ? LOCTEXT("FillMissingTitle", "Not enough materials to fill") : LOCTEXT("CopyMissingTitle", "Not enough materials to copy"),
			FText::FromString(TEXT("Missing: ") + FString::Join(Parts, TEXT(", "))));
		return;
	}
	case EAreaActionsResultCode::TooMany:
		Shell->ShowMessage(Failed, FText::Format(LOCTEXT("TooMany", "{Asked} buildings asked, at most {Limit} at once."),
			FFormatNamedArguments{ { TEXT("Asked"), Result.Buildings }, { TEXT("Limit"), Result.Items } }));
		return;
	default:
		Shell->ShowMessage(Failed, LOCTEXT("Unexpected", "The server gave an unexpected answer."));
	}
}

void UAreaActionPlaceCopies::ShowWindow()
{
	APlayerController* Player = Cast<APlayerController>(GetShell()->GetOwner());
	if (!Window || !Player)
	{
		return;
	}
	Window->SetVisibility(ESlateVisibility::Visible);
	UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx(Player, Window, EMouseLockMode::DoNotLock, false);
	Player->SetShowMouseCursor(true);
}

bool UAreaActionPlaceCopies::GetWindowFlag(const TCHAR* Name) const
{
	const FBoolProperty* Flag = Window ? FindFProperty<FBoolProperty>(Window->GetClass(), Name) : nullptr;
	return Flag && Flag->GetPropertyValue_InContainer(Window);
}

void UAreaActionPlaceCopies::SetWindowFlag(const TCHAR* Name, bool bValue)
{
	if (const FBoolProperty* Flag = Window ? FindFProperty<FBoolProperty>(Window->GetClass(), Name) : nullptr)
	{
		Flag->SetPropertyValue_InContainer(Window, bValue);
	}
}

bool UAreaActionPlaceCopies::CallWindow(FName Name, TFunctionRef<bool(UFunction*, uint8*)> SetParams, TFunctionRef<void(UFunction*, const uint8*)> ReadParams)
{
	UFunction* Function = Window ? Window->FindFunction(Name) : nullptr;
	if (!Function)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: the window %s has no %s"), *GetClass()->GetName(), *GetNameSafe(Window), *Name.ToString());
		return false;
	}
	uint8* Params = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(1, Function->ParmsSize)));
	FMemory::Memzero(Params, FMath::Max<int32>(1, Function->ParmsSize));
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Params);
	}
	const bool bSet = SetParams(Function, Params);
	if (bSet)
	{
		Window->ProcessEvent(Function, Params);
		ReadParams(Function, Params);
	}
	else
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: %s.%s has other parameters"), *GetClass()->GetName(), *GetNameSafe(Window), *Name.ToString());
	}
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Params);
	}
	return bSet;
}

AActor* UAreaActionPlaceCopies::GetSettings() const
{
	static const TCHAR* Path = TEXT("/AreaActionsReborn/Actions/ActionsSettings.ActionsSettings_C");
	UClass* Class = LoadClass<AActor>(nullptr, Path);
	return Class ? UGameplayStatics::GetActorOfClass(GetShell(), Class) : nullptr;
}

bool UAreaActionCopy::BindWindow()
{
	return AAreaActionBase::BindWindowEvent(Window, TEXT("OkClicked"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionCopy, Ok))
		&& AAreaActionBase::BindWindowEvent(Window, TEXT("Preview"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionCopy, PreviewClicked));
}

FAreaActionsPlacement UAreaActionCopy::MakePlacement(const FVector& Delta, const FRotator& DeltaRotation) const
{
	FAreaActionsPlacement Placement;
	Placement.Delta = Delta;
	Placement.Yaw = DeltaRotation.Yaw;
	Placement.RotationCenter = RotationCenter;
	return Placement;
}

void UAreaActionCopy::Ok(FVector Delta, FRotator DeltaRotation, bool UseCost)
{
	Send(MakePlacement(Delta, DeltaRotation), UseCost);
}

void UAreaActionCopy::PreviewClicked(FVector Delta, FRotator DeltaRotation)
{
	bPreviewing = true;
	PreviewDelta = Delta;
	PreviewRotation = DeltaRotation;
	ShowPreview(MakePlacement(Delta, DeltaRotation));
}

void UAreaActionCopy::OnClick(bool bRight)
{
	AAreaActionBase* Shell = GetActiveShell();
	AAreaActionsEquipment* Tool = Shell ? Shell->GetEquipment() : nullptr;
	if (!Tool)
	{
		return;
	}
	UWorld* World = Shell->GetWorld();
	FHitResult Hit;
	if (GetWindowFlag(TEXT("IsSelectingPasteTo")))
	{
		if (Tool->RaycastMouse(Hit))
		{
			AreaActionsBuildings::FSnapshot Target;
			const bool bBuilding = AreaActionsBuildings::FromHit(World, Hit, Target);
			FVector Point = Hit.Location;
			FBox Box;
			if (bBuilding && AreaActionsBuildings::GetLocalBox(Target, Box))
			{
				// The original's ActorZOffset: onto the top of the building hit
				Point.Z = Box.TransformBy(Target.Transform).Max.Z;
			}
			const FRotator Axes(0.0, Frame.Yaw, 0.0);
			const FVector Delta = Axes.UnrotateVector(Point - Frame.Position).GridSnap(1.0);
			FRotator Rotation = PreviewRotation;
			CallWindow(TEXT("GetData"), [](UFunction*, uint8*) { return true; }, [&Rotation](UFunction* Function, const uint8* Params)
			{
				if (const FRotator* Value = StructParam<FRotator>(Function, const_cast<uint8*>(Params), TEXT("DeltaRotation")))
				{
					Rotation = *Value;
				}
			});
			if (bBuilding)
			{
				Rotation = FRotator(0.0, FMath::RoundToDouble((Target.Transform.Rotator() - Axes).GetNormalized().Yaw * 100.0) / 100.0, 0.0);
			}
			CallWindow(TEXT("SetData"), [&Delta, &Rotation](UFunction* Function, uint8* Params)
			{
				FVector* DeltaParam = StructParam<FVector>(Function, Params, TEXT("Delta"));
				FRotator* RotationParam = StructParam<FRotator>(Function, Params, TEXT("DeltaRotation"));
				if (!DeltaParam || !RotationParam)
				{
					return false;
				}
				// The window shows metres
				*DeltaParam = Delta / 100.0;
				*RotationParam = Rotation;
				return true;
			}, [](UFunction*, const uint8*) {});
			PreviewClicked(Delta, Rotation);
		}
		ShowWindow();
		SetWindowFlag(TEXT("IsSelectingPasteTo"), false);
		return;
	}
	if (GetWindowFlag(TEXT("IsSelectingRotationPoint")))
	{
		AreaActionsBuildings::FSnapshot Target;
		if (Tool->RaycastMouse(Hit))
		{
			if (RotationMarker && Hit.GetActor() == RotationMarker)
			{
				RotationCenter = FVector::ZeroVector;
				RotationMarker->Destroy();
				RotationMarker = nullptr;
			}
			else if (AreaActionsBuildings::FromHit(World, Hit, Target)
				&& Buildings.ContainsByPredicate([&Target](const AreaActionsBuildings::FSnapshot& Each) { return SamePlace(Each, Target); }))
			{
				// The original's SnapToActor: the building's corner or middle nearest to the click
				TArray<FVector> Points;
				AreaActionsBuildings::GetCorners(Target, Points);
				Points.Add(Target.Transform.GetLocation());
				const FVector At = Hit.Location;
				Points.Sort([&At](const FVector& A, const FVector& B) { return FVector::DistSquared(A, At) < FVector::DistSquared(B, At); });
				RotationCenter = Points[0] - Frame.Position;
				if (RotationMarker)
				{
					RotationMarker->Destroy();
				}
				FActorSpawnParameters Parameters;
				Parameters.Owner = Shell;
				Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				UClass* MarkerClass = LoadClass<AActor>(nullptr, TEXT("/AreaActionsReborn/Actions/Copy/CopyRotationCenter.CopyRotationCenter_C"));
				RotationMarker = MarkerClass ? World->SpawnActor<AActor>(MarkerClass, FTransform(Points[0]), Parameters) : nullptr;
				if (bPreviewing)
				{
					PreviewClicked(PreviewDelta, PreviewRotation);
				}
			}
		}
		ShowWindow();
		SetWindowFlag(TEXT("IsSelectingRotationPoint"), false);
		return;
	}
	ShowWindow();
}

void UAreaActionCopy::End()
{
	Super::End();
	if (RotationMarker)
	{
		RotationMarker->Destroy();
		RotationMarker = nullptr;
	}
}

void UAreaActionFill::Start()
{
	Super::Start();
	if (Window)
	{
		UpdateRamp();
	}
}

bool UAreaActionFill::BindWindow()
{
	return AAreaActionBase::BindWindowEvent(Window, TEXT("OkClicked"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionFill, Ok))
		&& AAreaActionBase::BindWindowEvent(Window, TEXT("Preview"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionFill, PreviewClicked))
		&& AAreaActionBase::BindWindowEvent(Window, TEXT("UpdateIndicatorRamp"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionFill, UpdateRamp));
}

FAreaActionsPlacement UAreaActionFill::MakePlacement(const FVector& Border, const FIntVector& Amount, const FVector& Ramp)
{
	FAreaActionsPlacement Placement;
	Placement.bFill = true;
	Placement.Border = Border;
	Placement.Amount = Amount;
	Placement.Ramp = FVector2D(Ramp.X, Ramp.Y);
	return Placement;
}

void UAreaActionFill::Ok(FVector Border, FIntVector Amount, FVector Ramp, bool UseCost)
{
	Send(MakePlacement(Border, Amount, Ramp), UseCost);
}

void UAreaActionFill::PreviewClicked(FVector Border, FIntVector Amount, FVector Ramp)
{
	ShowPreview(MakePlacement(Border, Amount, Ramp));
}

bool UAreaActionFill::GetData(FVector& Border, FIntVector& Amount, FVector& Ramp)
{
	bool bRead = false;
	CallWindow(TEXT("GetData"), [](UFunction*, uint8*) { return true; }, [&](UFunction* Function, const uint8* Params)
	{
		uint8* Raw = const_cast<uint8*>(Params);
		const FVector* BorderValue = StructParam<FVector>(Function, Raw, TEXT("Border"));
		const FIntVector* AmountValue = StructParam<FIntVector>(Function, Raw, TEXT("Amount"));
		const FVector* RampValue = StructParam<FVector>(Function, Raw, TEXT("Ramp"));
		if (BorderValue && AmountValue && RampValue)
		{
			Border = *BorderValue;
			Amount = *AmountValue;
			Ramp = *RampValue;
			bRead = true;
		}
	});
	return bRead;
}

void UAreaActionFill::OnClick(bool bRight)
{
	AAreaActionBase* Shell = GetActiveShell();
	AAreaActionsEquipment* Tool = Shell ? Shell->GetEquipment() : nullptr;
	if (!Tool)
	{
		return;
	}
	FHitResult Hit;
	FVector Border, Ramp;
	FIntVector Amount;
	if (GetWindowFlag(TEXT("IsSelectingFillTo")) && Tool->RaycastMouse(Hit) && GetData(Border, Amount, Ramp))
	{
		// The cell of the pattern's grid the click is in, along the area's axes
		const FVector Local = FRotator(0.0, Frame.Yaw, 0.0).UnrotateVector(Hit.Location - Frame.Position);
		const FVector Pitch = Frame.PatternSize + Border;
		auto Cells = [](double Distance, double Step) { return FMath::Abs(Step) < 1.0 ? 0 : static_cast<int32>(FMath::Clamp(Distance / Step, -1.0e6, 1.0e6)); };
		const FIntVector Picked(Remap(Cells(Local.X, Pitch.X)), Remap(Cells(Local.Y, Pitch.Y)), Amount.Z);
		CallWindow(TEXT("SetData"), [&Picked](UFunction* Function, uint8* Params)
		{
			FIntVector* Value = StructParam<FIntVector>(Function, Params, TEXT("Amount"));
			if (Value)
			{
				*Value = Picked;
			}
			return Value != nullptr;
		}, [](UFunction*, const uint8*) {});
		PreviewClicked(Border, Picked, Ramp);
	}
	if (GetWindowFlag(TEXT("IsSelectingFillTo")))
	{
		SetWindowFlag(TEXT("IsSelectingFillTo"), false);
	}
	ShowWindow();
}

void UAreaActionFill::UpdateRamp()
{
	AAreaActionBase* Shell = GetActiveShell();
	FVector Border, Ramp;
	FIntVector Amount;
	if (!Shell || !GetData(Border, Amount, Ramp))
	{
		return;
	}
	// The original's UpdateRamp: the arrows lean by the ramp's angle over one pattern, towards the side the copies go
	const double AngleX = FMath::RadiansToDegrees(FMath::Atan(Ramp.X / FMath::Max(1.0, Frame.PatternSize.X)));
	const double AngleY = FMath::RadiansToDegrees(FMath::Atan(Ramp.Y / FMath::Max(1.0, Frame.PatternSize.Y)));
	TInlineComponentArray<USceneComponent*> Components(Shell);
	for (USceneComponent* Component : Components)
	{
		if (Component->GetFName() == TEXT("XArrow"))
		{
			Component->SetRelativeRotation(FRotator(Amount.X >= 0 ? AngleX - 90.0 : 90.0 - AngleX, 0.0, 0.0));
		}
		else if (Component->GetFName() == TEXT("YArrow"))
		{
			Component->SetRelativeRotation(FRotator(0.0, 0.0, Amount.Y >= 0 ? 90.0 - AngleY : AngleY - 90.0));
		}
	}
}

#undef LOCTEXT_NAMESPACE

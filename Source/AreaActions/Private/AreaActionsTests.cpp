#include "AreaActionsTests.h"

#if AA_WITH_TESTS

#include "AreaActionsEquipment.h"
#include "AreaActionsTypes.h"
#include "Blueprint/UserWidget.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Components/Button.h"
#include "EngineUtils.h"
#include "FGCrate.h"
#include "FGItemPickup_Spawnable.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Misc/App.h"
#include "AreaActionsInstanceModule.h"
#include "AreaActionsPopupBase.h"
#include "Buildables/FGBuildable.h"
#include "Components/ListView.h"
#include "Engine/GameViewportClient.h"
#include "FGBuildableSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponentEquipment.h"
#include "InputKeyEventArgs.h"
#include "Interfaces/IPluginManager.h"
#include "Resources/FGItemDescriptor.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "AreaActionsServerSubsystem.h"
#include "AreaActionsWorldModule.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDeviceRedirector.h"

namespace
{
	enum class EStep : uint8 { Next, Wait };

	struct FRun;
	using FStepFn = TFunction<EStep(FRun&)>;

	struct FRun
	{
		FString Name;
		TWeakObjectPtr<UWorld> World;
		TArray<FStepFn> Steps;
		int32 Index = 0;
		int32 Passed = 0;
		int32 Total = 0;
		double Started = 0.0;
		uint64 StartFrame = 0;
		double StepStarted = 0.0;
		// A step that waits longer than this fails the scenario
		double StepTimeoutSec = 30.0;
		bool bStarted = false;
		bool bEnded = false;
		// Scenario state shared between steps
		int32 PongToken = 0;
		bool bPongSeen = false;
		FDelegateHandle PongHandle;
		// Longest game frame while bMeasureFrames is on (W5)
		bool bMeasureFrames = false;
		double MaxFrameMs = 0.0;

		void Step(const FString& Text) const
		{
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step %s"), *Name, *Text);
		}

		bool Check(bool bOk, const FString& What)
		{
			++Total;
			Passed += bOk ? 1 : 0;
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s check %s %s"), *Name, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
			return bOk;
		}

		double Now() const { return FPlatformTime::Seconds(); }
		UWorld* GetWorld() const { return World.Get(); }
		APlayerController* GetPlayer() const { return World.IsValid() ? UGameplayStatics::GetPlayerController(World.Get(), 0) : nullptr; }
	};

	TUniquePtr<FRun> Run;

	void AddSmoke(FRun& R, bool bFailOnPurpose)
	{
		R.Steps.Add([](FRun& Self)
		{
			constexpr uint8 All = (1 << 0) | (1 << 1) | (1 << 2);
			Self.Check(UAreaActionsInstanceModule::PhasesSeen == All, FString::Printf(TEXT("instance module phases 0x%x"), UAreaActionsInstanceModule::PhasesSeen));
			Self.Check(UAreaActionsWorldModule::PhasesSeen == All, FString::Printf(TEXT("world module phases 0x%x"), UAreaActionsWorldModule::PhasesSeen));
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			// The RCO comes with the player controller, a moment after the world starts
			UAreaActionsRCO* Rco = UAreaActionsRCO::Of(Self.GetPlayer());
			if (!Rco)
			{
				return EStep::Wait;
			}
			Self.Check(true, TEXT("local player's RCO"));
			Self.PongToken = 1000 + FMath::RandRange(0, 8999);
			Self.PongHandle = Rco->OnPong.AddLambda([Token = Self.PongToken](int32 Got)
			{
				if (Run && Got == Token)
				{
					Run->bPongSeen = true;
				}
			});
			Self.Step(FString::Printf(TEXT("ServerPing %d"), Self.PongToken));
			Rco->ServerPing(Self.PongToken);
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			if (!Self.bPongSeen)
			{
				return EStep::Wait;
			}
			Self.Check(true, FString::Printf(TEXT("ClientPong %d"), Self.PongToken));
			if (UAreaActionsRCO* Rco = UAreaActionsRCO::Of(Self.GetPlayer()))
			{
				Rco->OnPong.Remove(Self.PongHandle);
			}
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			const bool bServer = World && World->GetNetMode() != NM_Client;
			Self.Check(!bServer || AAreaActionsServerSubsystem::Get(World) != nullptr, FString::Printf(TEXT("server subsystem (net mode %d)"), World ? static_cast<int32>(World->GetNetMode()) : -1));
			return EStep::Next;
		});
		if (bFailOnPurpose)
		{
			R.Steps.Add([](FRun& Self)
			{
				Self.Check(false, TEXT("deliberate failure"));
				return EStep::Next;
			});
		}
	}

	// --- The tool (AA-005): the Blueprint's own variables and events, read and called by name ---

	const TCHAR* ToolItem = TEXT("/AreaActions/Equipment/AreaActions/Desc_AreaActions.Desc_AreaActions_C");
	const TCHAR* StateEnum = TEXT("/AreaActions/Equipment/AreaActions/AreaBuildingStateEnum.AreaBuildingStateEnum");

	AFGCharacterPlayer* GetCharacter(const FRun& R)
	{
		APlayerController* Player = R.GetPlayer();
		return Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
	}

	AAreaActionsEquipment* GetTool(const FRun& R)
	{
		AFGCharacterPlayer* Character = GetCharacter(R);
		if (!Character)
		{
			return nullptr;
		}
		for (AFGEquipment* Equipment : Character->GetActiveEquipments())
		{
			if (AAreaActionsEquipment* Tool = Cast<AAreaActionsEquipment>(Equipment))
			{
				return Tool;
			}
		}
		return nullptr;
	}

	int32 ArrayNum(UObject* Object, const TCHAR* Name)
	{
		FArrayProperty* Property = Object ? FindFProperty<FArrayProperty>(Object->GetClass(), Name) : nullptr;
		return Property ? FScriptArrayHelper(Property, Property->ContainerPtrToValuePtr<void>(Object)).Num() : -1;
	}

	TArray<FVector> VectorArray(UObject* Object, const TCHAR* Name)
	{
		FArrayProperty* Property = Object ? FindFProperty<FArrayProperty>(Object->GetClass(), Name) : nullptr;
		return Property ? *Property->ContainerPtrToValuePtr<TArray<FVector>>(Object) : TArray<FVector>();
	}

	double DoubleValue(UObject* Object, const TCHAR* Name)
	{
		FDoubleProperty* Property = Object ? FindFProperty<FDoubleProperty>(Object->GetClass(), Name) : nullptr;
		return Property ? Property->GetPropertyValue_InContainer(Object) : NAN;
	}

	UObject* ObjectValue(UObject* Object, const TCHAR* Name)
	{
		FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
		return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
	}

	// The tool's state, as the menu's state list sets it: the Blueprint event SelectBuildStatus(BuildingState)
	bool SelectState(UObject* Tool, const TCHAR* Enumerator)
	{
		const UEnum* Enum = LoadObject<UEnum>(nullptr, StateEnum);
		UFunction* Function = Tool ? Tool->FindFunction(TEXT("SelectBuildStatus")) : nullptr;
		const int64 Value = Enum ? Enum->GetValueByNameString(Enumerator) : INDEX_NONE;
		if (!Function || Value == INDEX_NONE)
		{
			return false;
		}
		TArray<uint8> Params;
		Params.SetNumZeroed(Function->ParmsSize);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (FByteProperty* Byte = CastField<FByteProperty>(*It))
			{
				Byte->SetPropertyValue_InContainer(Params.GetData(), static_cast<uint8>(Value));
			}
			else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(*It))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Params.GetData()), Value);
			}
		}
		Tool->ProcessEvent(Function, Params.GetData());
		return true;
	}

	// A real key press of the local player, through the player controller's input as the keyboard and mouse send it
	void PressKey(const FRun& R, const FKey& Key)
	{
		APlayerController* Player = R.GetPlayer();
		FViewport* Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		Player->InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Key, IE_Pressed));
		Player->InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Key, IE_Released));
	}

	// Turns the camera to look at a point; the tool's raycast starts at the camera
	void LookAt(const FRun& R, const FVector& Target)
	{
		APlayerController* Player = R.GetPlayer();
		Player->SetControlRotation((Target - Player->PlayerCameraManager->GetCameraLocation()).Rotation());
	}

	// Waits a few frames (the camera manager and widgets update on the next ticks)
	FStepFn WaitFrames(int32 Frames)
	{
		return [Frames, Until = TSharedRef<uint64>(MakeShared<uint64>(0))](FRun& Self) mutable
		{
			if (*Until == 0)
			{
				*Until = GFrameCounter + Frames;
			}
			if (GFrameCounter < *Until)
			{
				return EStep::Wait;
			}
			*Until = 0;
			return EStep::Next;
		};
	}

	bool GroundBelow(UWorld* World, const FVector& Point, FVector& OutGround)
	{
		FHitResult Hit;
		FCollisionQueryParams Params(TEXT("AATest"), false);
		if (APlayerController* Player = UGameplayStatics::GetPlayerController(World, 0))
		{
			Params.AddIgnoredActor(Player->GetPawn());
		}
		if (!World->LineTraceSingleByChannel(Hit, Point + FVector(0, 0, 5000), Point - FVector(0, 0, 5000), ECC_WorldStatic, Params))
		{
			return false;
		}
		OutGround = Hit.Location;
		return true;
	}

	// The player takes the tool in hand: one tool item in the arms slot, made the active one
	void AddEquipTool(FRun& R)
	{
		R.Steps.Add([](FRun& Self)
		{
			AFGCharacterPlayer* Character = GetCharacter(Self);
			if (!Character)
			{
				return EStep::Wait;
			}
			UClass* Item = LoadClass<UFGItemDescriptor>(nullptr, ToolItem);
			UFGInventoryComponentEquipment* Arms = Character->GetEquipmentSlot(EEquipmentSlot::ES_ARMS);
			if (!Self.Check(Item && Arms, FString::Printf(TEXT("tool item %s, arms slot %s"), *GetNameSafe(Item), *GetNameSafe(Arms))))
			{
				return EStep::Next;
			}
			int32 Free = INDEX_NONE;
			for (int32 Index = 0; Index < Arms->GetSizeLinear() && Free == INDEX_NONE; ++Index)
			{
				Free = Arms->IsSomethingOnIndex(Index) ? INDEX_NONE : Index;
			}
			const int32 Added = Free == INDEX_NONE ? 0 : Arms->AddStackToIndex(Free, FInventoryStack(1, Item));
			Self.Step(FString::Printf(TEXT("tool item put in arms slot index %d: %d"), Free, Added));
			Arms->SetActiveIndex(Free);
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			if (!Tool)
			{
				return EStep::Wait;
			}
			Self.Check(true, FString::Printf(TEXT("tool in hand: %s"), *Tool->GetClass()->GetName()));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
	}

	void AddToolMenu(FRun& R)
	{
		AddEquipTool(R);
		R.Steps.Add([](FRun& Self)
		{
			Self.Step(TEXT("right mouse button"));
			PressKey(Self, EKeys::RightMouseButton);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			UAreaActionsPopupBase* Popup = Cast<UAreaActionsPopupBase>(ObjectValue(GetTool(Self), TEXT("PopupWidget")));
			if (!Self.Check(Popup && Popup->IsInViewport(), FString::Printf(TEXT("menu open: %s"), *GetNameSafe(Popup))))
			{
				return EStep::Next;
			}
			FString Version;
			if (const FPluginDescriptor* Descriptor = IPluginManager::Get().FindPlugin(TEXT("AreaActions")) ? &IPluginManager::Get().FindPlugin(TEXT("AreaActions"))->GetDescriptor() : nullptr)
			{
				Version = Descriptor->VersionName;
			}
			const FString Title = Popup->GetTitle().ToString();
			Self.Check(!Version.IsEmpty() && Title.Contains(Version), FString::Printf(TEXT("title '%s' has the version '%s'"), *Title, *Version));
			const UListView* Actions = Cast<UListView>(Popup->GetWidgetFromName(TEXT("Actions")));
			Self.Check(Actions && Actions->GetNumItems() == 6, FString::Printf(TEXT("action list items: %d"), Actions ? Actions->GetNumItems() : -1));
			return EStep::Next;
		});
	}

	// Three corners around a point in front of the player, as the player clicks them
	void AddCorners(FRun& R)
	{
		AddEquipTool(R);
		TSharedRef<TArray<FVector>> Targets = MakeShared<TArray<FVector>>();
		R.Steps.Add([Targets](FRun& Self)
		{
			APawn* Pawn = Self.GetPlayer()->GetPawn();
			const FVector Center = Pawn->GetActorLocation() + Pawn->GetActorForwardVector() * 1200.0;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const FVector Point = Center + FRotator(0, Index * 120.0, 0).RotateVector(FVector(500, 0, 0));
				FVector Ground;
				if (!GroundBelow(Self.GetWorld(), Point, Ground))
				{
					Self.Check(false, FString::Printf(TEXT("ground below %s"), *Point.ToCompactString()));
					return EStep::Next;
				}
				Targets->Add(Ground);
			}
			Self.Check(SelectState(GetTool(Self), TEXT("NewEnumerator1")), TEXT("corner mode set"));
			return EStep::Next;
		});
		for (int32 Index = 0; Index < 3; ++Index)
		{
			R.Steps.Add([Targets, Index](FRun& Self)
			{
				if (Targets->IsValidIndex(Index))
				{
					LookAt(Self, (*Targets)[Index]);
				}
				return EStep::Next;
			});
			R.Steps.Add(WaitFrames(3));
			R.Steps.Add([Targets, Index](FRun& Self)
			{
				Self.Step(FString::Printf(TEXT("left click %d at %s"), Index + 1, Targets->IsValidIndex(Index) ? *(*Targets)[Index].ToCompactString() : TEXT("-")));
				PressKey(Self, EKeys::LeftMouseButton);
				return EStep::Next;
			});
			R.Steps.Add(WaitFrames(3));
		}
		R.Steps.Add([](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			const int32 Corners = ArrayNum(Tool, TEXT("CornerLocations"));
			const int32 Walls = ArrayNum(Tool, TEXT("AreaWalls"));
			Self.Check(Corners == 3, FString::Printf(TEXT("corners after 3 clicks: %d"), Corners));
			Self.Check(Walls == 3, FString::Printf(TEXT("walls after 3 clicks: %d"), Walls));
			Self.Check(SelectState(Tool, TEXT("NewEnumerator10")), TEXT("Clear"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			const int32 Corners = ArrayNum(Tool, TEXT("CornerLocations"));
			const int32 Indicators = ArrayNum(Tool, TEXT("CornerIndicators")) + ArrayNum(Tool, TEXT("AreaWalls"))
				+ (ObjectValue(Tool, TEXT("MinZIndicator")) ? 1 : 0) + (ObjectValue(Tool, TEXT("MaxZIndicator")) ? 1 : 0);
			Self.Check(Corners == 0, FString::Printf(TEXT("corners after Clear: %d"), Corners));
			Self.Check(Indicators == 0, FString::Printf(TEXT("indicators after Clear: %d"), Indicators));
			return EStep::Next;
		});
	}

	// A Constructor in front of the player, clicked in the building selection mode
	void AddSelectBuilding(FRun& R)
	{
		AddEquipTool(R);
		TSharedRef<TWeakObjectPtr<AFGBuildable>> Built = MakeShared<TWeakObjectPtr<AFGBuildable>>();
		R.Steps.Add([Built](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			APawn* Pawn = Self.GetPlayer()->GetPawn();
			FVector Ground;
			UClass* Class = LoadClass<AFGBuildable>(nullptr, TEXT("/Game/FactoryGame/Buildable/Factory/ConstructorMk1/Build_ConstructorMk1.Build_ConstructorMk1_C"));
			AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
			if (!Self.Check(Class && Buildables && GroundBelow(World, Pawn->GetActorLocation() + Pawn->GetActorForwardVector() * 1500.0, Ground),
				TEXT("Constructor class, buildable subsystem, ground in front")))
			{
				return EStep::Next;
			}
			const FTransform Transform(FRotator(0, 37, 0), Ground + FVector(0, 0, 100));
			AFGBuildable* Constructor = Buildables->BeginSpawnBuildable(Class, Transform);
			if (Constructor)
			{
				Constructor->FinishSpawning(Transform);
			}
			*Built = Constructor;
			Self.Check(Constructor != nullptr, FString::Printf(TEXT("Constructor built at %s"), *Transform.GetLocation().ToCompactString()));
			Self.Check(SelectState(GetTool(Self), TEXT("NewEnumerator14")), TEXT("building selection mode set"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		R.Steps.Add([Built](FRun& Self)
		{
			if (AFGBuildable* Constructor = Built->Get())
			{
				FBox Box;
				AAreaActionsEquipment::GetLocalBox(Constructor, Box);
				LookAt(Self, Constructor->GetActorTransform().TransformPosition(Box.GetCenter()));
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([](FRun& Self)
		{
			Self.Step(TEXT("left click on the Constructor"));
			PressKey(Self, EKeys::LeftMouseButton);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([Built](FRun& Self)
		{
			AFGBuildable* Constructor = Built->Get();
			AAreaActionsEquipment* Tool = GetTool(Self);
			if (!Self.Check(Constructor && Tool, TEXT("Constructor and tool still there")))
			{
				return EStep::Next;
			}
			const FBox Box = Constructor->GetCombinedClearanceBox();
			const FTransform Transform = Constructor->GetActorTransform();
			TArray<FVector> Expected;
			double ExpectedMinZ = UE_BIG_NUMBER, ExpectedMaxZ = -UE_BIG_NUMBER;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const FVector Corner = Transform.TransformPosition(FVector(Index & 1 ? Box.Max.X : Box.Min.X, Index & 2 ? Box.Max.Y : Box.Min.Y, Index & 4 ? Box.Max.Z : Box.Min.Z));
				ExpectedMinZ = FMath::Min(ExpectedMinZ, Corner.Z);
				ExpectedMaxZ = FMath::Max(ExpectedMaxZ, Corner.Z);
				if (Index < 4)
				{
					Expected.Add(FVector(Corner.X, Corner.Y, 0));
				}
			}
			const TArray<FVector> Corners = VectorArray(Tool, TEXT("CornerLocations"));
			int32 Matched = 0;
			for (const FVector& Want : Expected)
			{
				Matched += Corners.ContainsByPredicate([&Want](const FVector& Got) { return FVector::Dist2D(Want, Got) <= 1.0; }) ? 1 : 0;
			}
			Self.Check(Box.IsValid != 0, FString::Printf(TEXT("clearance box %s"), *Box.ToString()));
			Self.Check(Corners.Num() == 4 && Matched == 4, FString::Printf(TEXT("corners %d, matching the clearance box within 1 cm: %d"), Corners.Num(), Matched));
			const double MinZ = DoubleValue(Tool, TEXT("MinZ"));
			const double MaxZ = DoubleValue(Tool, TEXT("MaxZ"));
			Self.Check(FMath::Abs(MinZ - ExpectedMinZ) <= 1.0 && FMath::Abs(MaxZ - ExpectedMaxZ) <= 1.0,
				FString::Printf(TEXT("MinZ %.1f (box %.1f), MaxZ %.1f (box %.1f)"), MinZ, ExpectedMinZ, MaxZ, ExpectedMaxZ));
			return EStep::Next;
		});
	}

	// --- The simple actions (AA-010) ---

	namespace Asset
	{
		const TCHAR* Constructor = TEXT("/Game/FactoryGame/Buildable/Factory/ConstructorMk1/Build_ConstructorMk1.Build_ConstructorMk1_C");
		const TCHAR* Assembler = TEXT("/Game/FactoryGame/Buildable/Factory/AssemblerMk1/Build_AssemblerMk1.Build_AssemblerMk1_C");
		const TCHAR* Storage = TEXT("/Game/FactoryGame/Buildable/Factory/StorageContainerMk1/Build_StorageContainerMk1.Build_StorageContainerMk1_C");
		const TCHAR* TruckStation = TEXT("/Game/FactoryGame/Buildable/Factory/TruckStation/Build_TruckStation.Build_TruckStation_C");
		const TCHAR* FreightPlatform = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Station/Build_TrainDockingStation.Build_TrainDockingStation_C");
		const TCHAR* IronPlate = TEXT("/Game/FactoryGame/Recipes/Constructor/Recipe_IronPlate.Recipe_IronPlate_C");
		const TCHAR* IronRod = TEXT("/Game/FactoryGame/Recipes/Constructor/Recipe_IronRod.Recipe_IronRod_C");
		const TCHAR* ReinforcedPlate = TEXT("/Game/FactoryGame/Recipes/Assembler/Recipe_IronPlateReinforced.Recipe_IronPlateReinforced_C");
		const TCHAR* IngotIron = TEXT("/Game/FactoryGame/Recipes/Smelter/Recipe_IngotIron.Recipe_IngotIron_C");
		const TCHAR* IronIngotItem = TEXT("/Game/FactoryGame/Resource/Parts/IronIngot/Desc_IronIngot.Desc_IronIngot_C");
		const TCHAR* IronPlateItem = TEXT("/Game/FactoryGame/Resource/Parts/IronPlate/Desc_IronPlate.Desc_IronPlate_C");
		const TCHAR* IronRodItem = TEXT("/Game/FactoryGame/Resource/Parts/IronRod/Desc_IronRod.Desc_IronRod_C");
		const TCHAR* PowerShard = TEXT("/Game/FactoryGame/Resource/Environment/Crystal/Desc_CrystalShard.Desc_CrystalShard_C");
	}

	TSubclassOf<UFGRecipe> Recipe(const TCHAR* Path) { return LoadClass<UFGRecipe>(nullptr, Path); }
	TSubclassOf<UFGItemDescriptor> Item(const TCHAR* Path) { return LoadClass<UFGItemDescriptor>(nullptr, Path); }

	// The test area: a square in front of the player, a height range, and the W4 control places
	struct FScene
	{
		FVector Center = FVector::ZeroVector;
		double HalfSize = 4000.0;
		FAreaActionsArea Area;
		TArray<FVector> Corners;
		TArray<TWeakObjectPtr<AFGBuildable>> Inside;
		TArray<TWeakObjectPtr<AFGBuildable>> Controls;
		TArray<TWeakObjectPtr<AFGCrate>> CratesBefore;
		TMap<int32, FAreaActionsResult> Results;
		FDelegateHandle ResultHandle;
		FString Message;
		int32 Expected = 0;
		int32 RejectsBefore = 0;
		int32 ArmsIndex = INDEX_NONE;

		FVector Slot(int32 Index) const { return Center + FVector(-HalfSize + 1000.0 + (Index % 4) * 1800.0, -HalfSize + 1000.0 + (Index / 4) * 1800.0, 0.0); }
		// Outside the polygon, in it below MinZ, in it above MaxZ
		TArray<FVector> ControlPlaces() const
		{
			return { Center + FVector(HalfSize + 2000.0, 0, 0), FVector(Center.X, Center.Y, Area.MinZ - 1500.0), FVector(Center.X, Center.Y, Area.MaxZ + 1500.0) };
		}
	};
	using FSceneRef = TSharedRef<FScene>;

	void PlaceScene(const FRun& R, FScene& S, double HalfSize, double Ahead)
	{
		const APawn* Pawn = R.GetPlayer()->GetPawn();
		const FVector Forward = Pawn->GetActorForwardVector().GetSafeNormal2D();
		S.HalfSize = HalfSize;
		S.Center = Pawn->GetActorLocation() + Forward * (HalfSize + Ahead) + FVector(0, 0, 300);
		S.Area.MinZ = Pawn->GetActorLocation().Z - 3000.0;
		S.Area.MaxZ = Pawn->GetActorLocation().Z + 3000.0;
		S.Corners.Reset();
		S.Area.Polygon.Reset();
		for (const FVector2D Sign : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(1, 1), FVector2D(-1, 1) })
		{
			const FVector Corner = S.Center + FVector(Sign.X * HalfSize, Sign.Y * HalfSize, 0);
			S.Corners.Add(Corner);
			S.Area.Polygon.Add(FVector2D(Corner));
		}
	}

	AFGBuildable* SpawnBuildable(const FRun& R, const TCHAR* Path, const FVector& At)
	{
		UClass* Class = LoadClass<AFGBuildable>(nullptr, Path);
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(R.GetWorld());
		if (!Class || !Buildables)
		{
			return nullptr;
		}
		const FTransform Transform(FRotator::ZeroRotator, At);
		AFGBuildable* Buildable = Buildables->BeginSpawnBuildable(Class, Transform);
		if (Buildable)
		{
			Buildable->FinishSpawning(Transform);
		}
		return Buildable;
	}

	// A machine with a recipe and its first ingredient in the input and first product in the output
	AFGBuildableManufacturer* SpawnMachine(const FRun& R, const TCHAR* Path, const FVector& At, const TCHAR* RecipePath, int32 InItems, int32 OutItems)
	{
		AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(SpawnBuildable(R, Path, At));
		const TSubclassOf<UFGRecipe> With = Recipe(RecipePath);
		if (!Machine || !With)
		{
			return Machine;
		}
		Machine->SetRecipe(With);
		const TArray<FItemAmount> Ingredients = UFGRecipe::GetIngredients(Machine, With);
		const TArray<FItemAmount> Products = UFGRecipe::GetProducts(With);
		if (InItems > 0 && Ingredients.Num() > 0)
		{
			Machine->GetInputInventory()->AddStack(FInventoryStack(InItems, Ingredients[0].ItemClass), true);
		}
		if (OutItems > 0 && Products.Num() > 0)
		{
			Machine->GetOutputInventory()->AddStack(FInventoryStack(OutItems, Products[0].ItemClass), true);
		}
		return Machine;
	}

	int32 MachineItems(const AFGBuildableManufacturer* Machine)
	{
		return Machine ? Machine->GetInputInventory()->GetNumItems(nullptr) + Machine->GetOutputInventory()->GetNumItems(nullptr) : -1;
	}

	void MakeAvailable(const FRun& R, const TCHAR* Path)
	{
		AFGRecipeManager* Recipes = AFGRecipeManager::Get(R.GetWorld());
		if (Recipes && !Recipes->IsRecipeAvailable(Recipe(Path)))
		{
			Recipes->AddAvailableRecipe(Recipe(Path));
			R.Step(FString::Printf(TEXT("recipe made available: %s"), Path));
		}
	}

	TArray<AFGCrate*> NewCrates(const FRun& R, const FScene& S)
	{
		TArray<AFGCrate*> Out;
		for (TActorIterator<AFGCrate> It(R.GetWorld()); It; ++It)
		{
			if (IsValid(*It) && !S.CratesBefore.Contains(*It))
			{
				Out.Add(*It);
			}
		}
		return Out;
	}

	void SnapshotCrates(const FRun& R, FScene& S)
	{
		S.CratesBefore.Reset();
		for (TActorIterator<AFGCrate> It(R.GetWorld()); It; ++It)
		{
			S.CratesBefore.Add(*It);
		}
	}

	// Results of the RCO, kept by request id
	void ListenResults(const FRun& R, const FSceneRef& S)
	{
		if (UAreaActionsRCO* Rco = UAreaActionsRCO::Of(R.GetPlayer()))
		{
			S->ResultHandle = Rco->OnResult.AddLambda([Weak = TWeakPtr<FScene>(S)](int32 Id, const FAreaActionsResult& Result)
			{
				if (TSharedPtr<FScene> Scene = Weak.Pin())
				{
					Scene->Results.Add(Id, Result);
				}
			});
		}
	}

	bool AllValid(const TArray<TWeakObjectPtr<AFGBuildable>>& Built)
	{
		return !Built.ContainsByPredicate([](const TWeakObjectPtr<AFGBuildable>& Entry) { return !Entry.IsValid(); });
	}

	// The selection in the tool, as clicks would leave it (CornerLocations, MinZ, MaxZ), and its walls drawn
	bool SetToolArea(const FRun& R, const FScene& S)
	{
		AAreaActionsEquipment* Tool = GetTool(R);
		FArrayProperty* Corners = Tool ? FindFProperty<FArrayProperty>(Tool->GetClass(), TEXT("CornerLocations")) : nullptr;
		FDoubleProperty* MinZ = Tool ? FindFProperty<FDoubleProperty>(Tool->GetClass(), TEXT("MinZ")) : nullptr;
		FDoubleProperty* MaxZ = Tool ? FindFProperty<FDoubleProperty>(Tool->GetClass(), TEXT("MaxZ")) : nullptr;
		if (!Corners || !MinZ || !MaxZ)
		{
			return false;
		}
		*Corners->ContainerPtrToValuePtr<TArray<FVector>>(Tool) = S.Corners;
		MinZ->SetPropertyValue_InContainer(Tool, S.Area.MinZ);
		MaxZ->SetPropertyValue_InContainer(Tool, S.Area.MaxZ);
		if (UFunction* Update = Tool->FindFunction(TEXT("UpdateIndicators")); Update && Update->ParmsSize == 0)
		{
			Tool->ProcessEvent(Update, nullptr);
		}
		return true;
	}

	// The newest of the tool's windows (its Widgets stack)
	UUserWidget* TopWindow(const FRun& R)
	{
		AAreaActionsEquipment* Tool = GetTool(R);
		FArrayProperty* Property = Tool ? FindFProperty<FArrayProperty>(Tool->GetClass(), TEXT("Widgets")) : nullptr;
		if (!Property)
		{
			return nullptr;
		}
		FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(Tool));
		return Array.Num() > 0 ? Cast<UUserWidget>(CastField<FObjectPropertyBase>(Property->Inner)->GetObjectPropertyValue(Array.GetRawPtr(Array.Num() - 1))) : nullptr;
	}

	bool IsWindow(const UUserWidget* Window, const TCHAR* Name)
	{
		return Window && Window->GetClass()->GetName().StartsWith(Name);
	}

	// The window's own button, as a mouse click fires it
	bool PressButton(UUserWidget* Window, const TCHAR* Name)
	{
		UButton* Button = Window ? Cast<UButton>(Window->GetWidgetFromName(Name)) : nullptr;
		if (!Button)
		{
			return false;
		}
		Button->OnClicked.Broadcast();
		return true;
	}

	UClass* ClassValue(UObject* Object, const TCHAR* Name)
	{
		FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
		return Property ? Cast<UClass>(Property->GetObjectPropertyValue_InContainer(Object)) : nullptr;
	}

	// A click on a list entry: the list's own "On Item Clicked" event, which the window's graph handles
	bool ClickListItem(UListView* List, TFunctionRef<bool(UObject*)> Pick)
	{
		FMulticastDelegateProperty* Property = List ? FindFProperty<FMulticastDelegateProperty>(List->GetClass(), TEXT("BP_OnItemClicked")) : nullptr;
		if (!Property)
		{
			return false;
		}
		for (UObject* Entry : List->GetListItems())
		{
			if (Pick(Entry))
			{
				struct { UObject* Item; } Params{ Entry };
				Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(List)->ProcessMulticastDelegate<UObject>(&Params);
				return true;
			}
		}
		return false;
	}

	UObject* ActiveAction(const FRun& R)
	{
		return ObjectValue(GetTool(R), TEXT("ActiveAction"));
	}

	// Right mouse button, then the action's entry in the menu
	void AddOpenAction(FRun& R, const TCHAR* Action)
	{
		R.Steps.Add([](FRun& Self)
		{
			if (ActiveAction(Self) || TopWindow(Self))
			{
				return EStep::Wait;
			}
			Self.Step(TEXT("right mouse button"));
			PressKey(Self, EKeys::RightMouseButton);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([Action](FRun& Self)
		{
			UAreaActionsPopupBase* Popup = Cast<UAreaActionsPopupBase>(ObjectValue(GetTool(Self), TEXT("PopupWidget")));
			UListView* List = Popup ? Cast<UListView>(Popup->GetWidgetFromName(TEXT("Actions"))) : nullptr;
			const bool bClicked = ClickListItem(List, [Action](UObject* Entry)
			{
				const UClass* Class = ClassValue(Entry, TEXT("Action"));
				return Class && Class->GetName().StartsWith(Action);
			});
			Self.Check(bClicked, FString::Printf(TEXT("menu entry %s clicked"), Action));
			return EStep::Next;
		});
	}

	// Waits for the window, then acts on it
	void AddOnWindow(FRun& R, const TCHAR* Name, TFunction<void(FRun&, UUserWidget*)> Act)
	{
		R.Steps.Add([Name, Act](FRun& Self)
		{
			UUserWidget* Window = TopWindow(Self);
			if (!IsWindow(Window, Name) || !Window->IsInViewport())
			{
				return EStep::Wait;
			}
			Self.Step(FString::Printf(TEXT("window %s"), *Window->GetClass()->GetName()));
			Act(Self, Window);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(2));
	}

	void AddPress(FRun& R, const TCHAR* Window, const TCHAR* Button)
	{
		AddOnWindow(R, Window, [Button](FRun& Self, UUserWidget* Open)
		{
			Self.Check(PressButton(Open, Button), FString::Printf(TEXT("button %s pressed"), Button));
		});
	}

	// The result window: its message is kept, OK ends the action
	void AddResultWindow(FRun& R, const FSceneRef& S)
	{
		AddOnWindow(R, TEXT("Widget_MessageOk"), [S](FRun& Self, UUserWidget* Window)
		{
			FStrProperty* Message = FindFProperty<FStrProperty>(Window->GetClass(), TEXT("Message"));
			S->Message = Message ? Message->GetPropertyValue_InContainer(Window) : FString();
			Self.Step(FString::Printf(TEXT("result message: %s"), *S->Message));
			Self.Check(PressButton(Window, TEXT("OkButton")), TEXT("button OkButton pressed"));
		});
		R.Steps.Add([](FRun& Self)
		{
			if (ActiveAction(Self) || TopWindow(Self))
			{
				return EStep::Wait;
			}
			Self.Check(true, TEXT("action ended, no window left"));
			return EStep::Next;
		});
	}

	void AddPickRecipe(FRun& R, const TCHAR* RecipePath)
	{
		AddOnWindow(R, TEXT("Widget_SetRecipe"), [RecipePath](FRun& Self, UUserWidget* Window)
		{
			const UClass* Want = Recipe(RecipePath);
			const bool bClicked = ClickListItem(Cast<UListView>(Window->GetWidgetFromName(TEXT("RecipeList"))), [Want](UObject* Entry)
			{
				return ClassValue(Entry, TEXT("Recipe")) == Want;
			});
			Self.Check(bClicked, FString::Printf(TEXT("recipe %s picked in the list"), *GetNameSafe(Want)));
		});
	}

	// Scene step: what Build puts in the area, then the tool's area, the crates before, the results listener
	void AddScene(FRun& R, const FSceneRef& S, double HalfSize, TFunction<void(FRun&, FScene&)> Build)
	{
		AddEquipTool(R);
		R.Steps.Add([S, HalfSize, Build](FRun& Self)
		{
			PlaceScene(Self, *S, HalfSize, 1000.0);
			Build(Self, *S);
			Self.Check(S->Inside.Num() > 0 && AllValid(S->Inside) && S->Controls.Num() == 3 && AllValid(S->Controls),
				FString::Printf(TEXT("scene: %d in the area, %d controls"), S->Inside.Num(), S->Controls.Num()));
			Self.Check(SetToolArea(Self, *S), TEXT("tool area set"));
			SnapshotCrates(Self, *S);
			ListenResults(Self, S);
			S->RejectsBefore = UAreaActionsRCO::RejectCount;
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
	}

	AFGBuildableManufacturer* Machine(const FScene& S, int32 Index)
	{
		return S.Inside.IsValidIndex(Index) ? Cast<AFGBuildableManufacturer>(S.Inside[Index].Get()) : nullptr;
	}

	// W4 for machines: each control keeps its recipe and items
	void CheckControlMachines(FRun& R, const FScene& S, const TCHAR* RecipePath, int32 Items)
	{
		for (int32 Index = 0; Index < S.Controls.Num(); ++Index)
		{
			const AFGBuildableManufacturer* Control = Cast<AFGBuildableManufacturer>(S.Controls[Index].Get());
			R.Check(Control && Control->GetCurrentRecipe() == Recipe(RecipePath) && MachineItems(Control) == Items,
				FString::Printf(TEXT("control %d: recipe %s, items %d"), Index, Control ? *GetNameSafe(Control->GetCurrentRecipe()) : TEXT("-"), MachineItems(Control)));
		}
	}

	void AddControlMachines(const FRun& R, FScene& S, const TCHAR* RecipePath, int32 In, int32 Out)
	{
		for (const FVector& Place : S.ControlPlaces())
		{
			S.Controls.Add(SpawnMachine(R, Asset::Constructor, Place, RecipePath, In, Out));
		}
	}

	void AddSetRecipe(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
		{
			MakeAvailable(Self, Asset::IronRod);
			const int32 In[] = { 10, 20, 7 };
			const int32 Out[] = { 5, 3, 1 };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				Scene.Inside.Add(SpawnMachine(Self, Asset::Constructor, Scene.Slot(Index), Asset::IronPlate, In[Index], Out[Index]));
			}
			Scene.Inside.Add(SpawnMachine(Self, Asset::Assembler, Scene.Slot(5), Asset::ReinforcedPlate, 0, 0));
			AddControlMachines(Self, Scene, Asset::IronPlate, 4, 2);
		});
		AddOpenAction(R, TEXT("SetRecipe"));
		AddPickRecipe(R, Asset::IronRod);
		AddResultWindow(R, S);
		R.Steps.Add([S](FRun& Self)
		{
			Self.Check(S->Message.Contains(TEXT("3 Constructor")), FString::Printf(TEXT("result names 3 Constructors: '%s'"), *S->Message));
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const AFGBuildableManufacturer* Constructor = Machine(*S, Index);
				Self.Check(Constructor && Constructor->GetCurrentRecipe() == Recipe(Asset::IronRod) && MachineItems(Constructor) == 0,
					FString::Printf(TEXT("Constructor %d: recipe %s, items %d"), Index, Constructor ? *GetNameSafe(Constructor->GetCurrentRecipe()) : TEXT("-"), MachineItems(Constructor)));
			}
			const AFGBuildableManufacturer* Assembler = Machine(*S, 3);
			Self.Check(Assembler && Assembler->GetCurrentRecipe() == Recipe(Asset::ReinforcedPlate), TEXT("Assembler keeps its recipe"));
			CheckControlMachines(Self, *S, Asset::IronPlate, 6);
			// D8: one crate with exactly the machines' items
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			const UFGInventoryComponent* Inventory = Crates.Num() == 1 ? Crates[0]->GetInventory() : nullptr;
			const int32 Ingots = Inventory ? Inventory->GetNumItems(Item(Asset::IronIngotItem)) : -1;
			const int32 Plates = Inventory ? Inventory->GetNumItems(Item(Asset::IronPlateItem)) : -1;
			const int32 All = Inventory ? Inventory->GetNumItems(nullptr) : -1;
			Self.Check(Crates.Num() == 1 && Ingots == 37 && Plates == 9 && All == 46,
				FString::Printf(TEXT("new crates %d; ingots %d of 37, plates %d of 9, all %d of 46"), Crates.Num(), Ingots, Plates, All));
			const APawn* Pawn = Self.GetPlayer()->GetPawn();
			Self.Check(Crates.Num() == 1 && FVector::Dist(Crates[0]->GetActorLocation(), Pawn->GetActorLocation()) < 1500.0,
				FString::Printf(TEXT("crate next to the player: %.0f cm"), Crates.Num() == 1 ? FVector::Dist(Crates[0]->GetActorLocation(), Pawn->GetActorLocation()) : -1.0));
			return EStep::Next;
		});
	}

	// Machines that cannot take the recipe, or (D12) already use it: nothing changes and no crate appears
	void AddSetRecipeUnchanged(FRun& R, const TCHAR* MachinesRecipe, const TCHAR* PickRecipe, const TCHAR* Expected)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [MachinesRecipe, PickRecipe](FRun& Self, FScene& Scene)
		{
			MakeAvailable(Self, PickRecipe);
			MakeAvailable(Self, MachinesRecipe);
			for (int32 Index = 0; Index < 2; ++Index)
			{
				Scene.Inside.Add(SpawnMachine(Self, Asset::Constructor, Scene.Slot(Index), MachinesRecipe, 8, 2));
			}
			AddControlMachines(Self, Scene, Asset::IronPlate, 4, 2);
		});
		AddOpenAction(R, TEXT("SetRecipe"));
		AddPickRecipe(R, PickRecipe);
		AddResultWindow(R, S);
		R.Steps.Add([S, MachinesRecipe, Expected](FRun& Self)
		{
			Self.Check(S->Message.Contains(Expected), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, Expected));
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const AFGBuildableManufacturer* Constructor = Machine(*S, Index);
				Self.Check(Constructor && Constructor->GetCurrentRecipe() == Recipe(MachinesRecipe) && MachineItems(Constructor) == 10,
					FString::Printf(TEXT("Constructor %d unchanged: recipe %s, items %d"), Index, Constructor ? *GetNameSafe(Constructor->GetCurrentRecipe()) : TEXT("-"), MachineItems(Constructor)));
			}
			CheckControlMachines(Self, *S, Asset::IronPlate, 6);
			Self.Check(NewCrates(Self, *S).IsEmpty(), FString::Printf(TEXT("no new crate: %d"), NewCrates(Self, *S).Num()));
			return EStep::Next;
		});
	}

	int32 StorageItems(const AFGBuildable* Buildable)
	{
		const AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Buildable);
		return Storage ? const_cast<AFGBuildableStorage*>(Storage)->GetStorageInventory()->GetNumItems(nullptr) : -1;
	}

	AFGBuildableStorage* SpawnStorage(const FRun& R, const FVector& At, int32 Plates, int32 Ingots)
	{
		AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(SpawnBuildable(R, Asset::Storage, At));
		if (Storage)
		{
			if (Plates > 0) Storage->GetStorageInventory()->AddStack(FInventoryStack(Plates, Item(Asset::IronPlateItem)), true);
			if (Ingots > 0) Storage->GetStorageInventory()->AddStack(FInventoryStack(Ingots, Item(Asset::IronIngotItem)), true);
		}
		return Storage;
	}

	void AddClearInventories(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<TWeakObjectPtr<AFGCrate>> Crate = MakeShared<TWeakObjectPtr<AFGCrate>>();
		AddScene(R, S, 4000.0, [Crate](FRun& Self, FScene& Scene)
		{
			Scene.Inside.Add(SpawnStorage(Self, Scene.Slot(0), 50, 30));
			Scene.Inside.Add(SpawnStorage(Self, Scene.Slot(1), 20, 0));
			AFGBuildableManufacturer* Constructor = SpawnMachine(Self, Asset::Constructor, Scene.Slot(2), Asset::IronPlate, 10, 5);
			Scene.Inside.Add(Constructor);
			// D11: a power shard in the machine's upgrade slot stays
			// The slot filter refuses shards until research unlocks the slots, which a new game lacks
			UFGInventoryComponent* Upgrades = Constructor ? Constructor->GetPotentialInventory() : nullptr;
			int32 Shards = 0;
			if (Upgrades && Upgrades->GetSizeLinear() > 0)
			{
				const FItemFilter Filter = Upgrades->mItemFilter;
				Upgrades->mItemFilter.Unbind();
				Shards = Upgrades->AddStackToIndex(0, FInventoryStack(1, Item(Asset::PowerShard)), true);
				Upgrades->mItemFilter = Filter;
			}
			Self.Check(Shards == 1, FString::Printf(TEXT("power shard in the Constructor's upgrade slot: %d (slots %d)"), Shards, Upgrades ? Upgrades->GetSizeLinear() : -1));
			FSpawnInventoryCrateParameters Parameters;
			Parameters.bAllowCrateMerging = false;
			*Crate = AFGItemPickup_Spawnable::SpawnInventoryCrate(Self.GetWorld(), { FInventoryStack(25, Item(Asset::IronRodItem)) }, Scene.Slot(5), Parameters);
			Self.Check(Crate->IsValid() && Scene.Area.Contains((*Crate)->GetActorLocation()),
				FString::Printf(TEXT("crate in the area at %s"), Crate->IsValid() ? *(*Crate)->GetActorLocation().ToCompactString() : TEXT("-")));
			for (const FVector& Place : Scene.ControlPlaces())
			{
				Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
			Scene.Expected = 50 + 30 + 20 + 15 + 25;
		});
		AddOpenAction(R, TEXT("ClearInventories"));
		AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("YesButton"));
		AddResultWindow(R, S);
		R.Steps.Add([S, Crate](FRun& Self)
		{
			const FString Want = FString::Printf(TEXT("Cleared 4 buildings deleting a total of %d items"), S->Expected);
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			Self.Check(StorageItems(S->Inside[0].Get()) == 0 && StorageItems(S->Inside[1].Get()) == 0,
				FString::Printf(TEXT("containers empty: %d, %d"), StorageItems(S->Inside[0].Get()), StorageItems(S->Inside[1].Get())));
			const AFGBuildableManufacturer* Constructor = Machine(*S, 2);
			Self.Check(MachineItems(Constructor) == 0, FString::Printf(TEXT("Constructor empty: %d"), MachineItems(Constructor)));
			const int32 Shards = Constructor ? Constructor->GetPotentialInventory()->GetNumItems(nullptr) : -1;
			Self.Check(Shards == 1, FString::Printf(TEXT("power shard kept (D11): %d"), Shards));
			const int32 CrateItems = Crate->IsValid() ? (*Crate)->GetInventory()->GetNumItems(nullptr) : 0;
			Self.Check(CrateItems == 0, FString::Printf(TEXT("crate empty or gone: %d items, %s"), CrateItems, Crate->IsValid() ? TEXT("still there") : TEXT("gone")));
			for (int32 Index = 0; Index < S->Controls.Num(); ++Index)
			{
				Self.Check(StorageItems(S->Controls[Index].Get()) == 15, FString::Printf(TEXT("control container %d: %d items"), Index, StorageItems(S->Controls[Index].Get())));
			}
			return EStep::Next;
		});
	}

	bool LoadMode(const AFGBuildable* Buildable)
	{
		if (const AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(Buildable))
		{
			return Station->GetIsInLoadMode();
		}
		const AFGBuildableTrainPlatformCargo* Platform = Cast<AFGBuildableTrainPlatformCargo>(Buildable);
		return Platform && Platform->GetIsInLoadMode();
	}

	void SetLoadMode(AFGBuildable* Buildable, bool bLoad)
	{
		if (AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(Buildable))
		{
			Station->SetIsInLoadMode(bLoad);
		}
		else if (AFGBuildableTrainPlatformCargo* Platform = Cast<AFGBuildableTrainPlatformCargo>(Buildable))
		{
			Platform->SetIsInLoadMode(bLoad);
		}
	}

	void AddControlStations(const FRun& R, FScene& S)
	{
		for (const FVector& Place : S.ControlPlaces())
		{
			AFGBuildable* Station = SpawnBuildable(R, Asset::TruckStation, Place);
			SetLoadMode(Station, false);
			S.Controls.Add(Station);
		}
	}

	void CheckControlStations(FRun& R, const FScene& S)
	{
		for (int32 Index = 0; Index < S.Controls.Num(); ++Index)
		{
			const AFGBuildable* Control = S.Controls[Index].Get();
			R.Check(Control && !LoadMode(Control), FString::Printf(TEXT("control station %d still unloads"), Index));
		}
	}

	// One pass of SetStationLoadUnload through its yes/no window
	void AddStationPass(FRun& R, const FSceneRef& S, bool bLoad)
	{
		AddOpenAction(R, TEXT("SetStationLoadUnload"));
		AddPress(R, TEXT("Widget_MessageYesNo"), bLoad ? TEXT("YesButton") : TEXT("NoButton"));
		AddResultWindow(R, S);
		R.Steps.Add([S, bLoad](FRun& Self)
		{
			const TCHAR* Mode = bLoad ? TEXT("to Load") : TEXT("to Unload");
			Self.Check(S->Message.Contains(Mode) && S->Message.Contains(TEXT("1 Truck Station")) && S->Message.Contains(TEXT("1 Freight Platform")),
				FString::Printf(TEXT("result '%s' names 1 Truck Station, 1 Freight Platform, %s"), *S->Message, Mode));
			for (int32 Index = 0; Index < S->Inside.Num(); ++Index)
			{
				const AFGBuildable* Station = S->Inside[Index].Get();
				Self.Check(Station && LoadMode(Station) == bLoad, FString::Printf(TEXT("%s load mode %d"), *GetNameSafe(Station), Station ? LoadMode(Station) : -1));
			}
			CheckControlStations(Self, *S);
			return EStep::Next;
		});
	}

	void AddSetStation(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
		{
			Scene.Inside.Add(SpawnBuildable(Self, Asset::TruckStation, Scene.Slot(0)));
			Scene.Inside.Add(SpawnBuildable(Self, Asset::FreightPlatform, Scene.Slot(6)));
			for (const TWeakObjectPtr<AFGBuildable>& Station : Scene.Inside)
			{
				SetLoadMode(Station.Get(), true);
			}
			AddControlStations(Self, Scene);
		});
		AddStationPass(R, S, false);
		AddStationPass(R, S, true);
	}

	void AddSetStationEmpty(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
		{
			Scene.Inside.Add(SpawnMachine(Self, Asset::Constructor, Scene.Slot(0), Asset::IronPlate, 0, 0));
			AddControlStations(Self, Scene);
		});
		AddOpenAction(R, TEXT("SetStationLoadUnload"));
		AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("YesButton"));
		AddResultWindow(R, S);
		R.Steps.Add([S](FRun& Self)
		{
			Self.Check(S->Message.Contains(TEXT("No stations in the area.")), FString::Printf(TEXT("result '%s'"), *S->Message));
			CheckControlStations(Self, *S);
			return EStep::Next;
		});
	}

	// Requests sent straight through the RCO, as a modified client could send them
	void AddRejectBadRequests(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
		{
			MakeAvailable(Self, Asset::IronRod);
			Scene.Inside.Add(SpawnMachine(Self, Asset::Constructor, Scene.Slot(0), Asset::IronPlate, 8, 2));
			AddControlMachines(Self, Scene, Asset::IronPlate, 4, 2);
		});
		R.Steps.Add([S](FRun& Self)
		{
			UAreaActionsRCO* Rco = UAreaActionsRCO::Of(Self.GetPlayer());
			FAreaActionsArea TwoCorners = S->Area;
			TwoCorners.Polygon.SetNum(2);
			FAreaActionsArea WithNaN = S->Area;
			WithNaN.Polygon[1].X = NAN;
			Self.Step(TEXT("requests 101 (2 corners), 102 (NaN), 103 (not a recipe)"));
			Rco->ServerSetRecipe(101, TwoCorners, Recipe(Asset::IronRod));
			Rco->ServerSetRecipe(102, WithNaN, Recipe(Asset::IronRod));
			Rco->ServerSetRecipe(103, S->Area, AActor::StaticClass());
			return EStep::Next;
		});
		// No tool in hand: the tool's item leaves the arms slot
		R.Steps.Add([S](FRun& Self)
		{
			UFGInventoryComponentEquipment* Arms = GetCharacter(Self)->GetEquipmentSlot(EEquipmentSlot::ES_ARMS);
			S->ArmsIndex = Arms->GetActiveIndex();
			Arms->RemoveAllFromIndex(S->ArmsIndex);
			return EStep::Next;
		});
		R.Steps.Add([S](FRun& Self)
		{
			if (GetTool(Self))
			{
				return EStep::Wait;
			}
			Self.Step(TEXT("tool put away; request 104"));
			UAreaActionsRCO::Of(Self.GetPlayer())->ServerClearInventories(104, S->Area);
			return EStep::Next;
		});
		R.Steps.Add([S](FRun& Self)
		{
			UFGInventoryComponentEquipment* Arms = GetCharacter(Self)->GetEquipmentSlot(EEquipmentSlot::ES_ARMS);
			Arms->AddStackToIndex(S->ArmsIndex, FInventoryStack(1, LoadClass<UFGItemDescriptor>(nullptr, ToolItem)));
			Arms->SetActiveIndex(S->ArmsIndex);
			return EStep::Next;
		});
		R.Steps.Add([S](FRun& Self)
		{
			if (!GetTool(Self))
			{
				return EStep::Wait;
			}
			// Two requests in one frame: the first one's job runs from the next tick, so the second one finds it
			Self.Step(TEXT("tool back in hand; requests 105 and 106 in one frame"));
			UAreaActionsRCO* Rco = UAreaActionsRCO::Of(Self.GetPlayer());
			Rco->ServerSetStationMode(105, S->Area, true);
			Rco->ServerSetStationMode(106, S->Area, true);
			return EStep::Next;
		});
		R.Steps.Add([S](FRun& Self)
		{
			if (S->Results.Num() < 6)
			{
				return EStep::Wait;
			}
			auto Code = [&S](int32 Id) { const FAreaActionsResult* Result = S->Results.Find(Id); return Result ? static_cast<int32>(Result->Code) : -1; };
			const int32 Rejected = static_cast<int32>(EAreaActionsResultCode::Rejected);
			for (int32 Id = 101; Id <= 104; ++Id)
			{
				Self.Check(Code(Id) == Rejected, FString::Printf(TEXT("request %d: code %d (Rejected %d)"), Id, Code(Id), Rejected));
			}
			Self.Check(Code(105) == static_cast<int32>(EAreaActionsResultCode::NothingInArea), FString::Printf(TEXT("request 105 runs: code %d"), Code(105)));
			Self.Check(Code(106) == static_cast<int32>(EAreaActionsResultCode::Busy), FString::Printf(TEXT("request 106: code %d (Busy)"), Code(106)));
			const int32 Rejects = UAreaActionsRCO::RejectCount - S->RejectsBefore;
			Self.Check(Rejects == 5, FString::Printf(TEXT("reject lines: %d of 5"), Rejects));
			const AFGBuildableManufacturer* Constructor = Machine(*S, 0);
			Self.Check(Constructor && Constructor->GetCurrentRecipe() == Recipe(Asset::IronPlate) && MachineItems(Constructor) == 10, TEXT("Constructor in the area unchanged"));
			CheckControlMachines(Self, *S, Asset::IronPlate, 6);
			Self.Check(NewCrates(Self, *S).IsEmpty(), TEXT("no new crate"));
			return EStep::Next;
		});
	}

	// 2000 machines, built 100 a frame; the request goes straight through the RCO, the frames are measured until the answer
	void AddSetRecipeLarge(FRun& R)
	{
		constexpr int32 Columns = 50;
		constexpr int32 Rows = 40;
		constexpr double Spacing = 1000.0;
		FSceneRef S = MakeShared<FScene>();
		AddEquipTool(R);
		R.Steps.Add([S](FRun& Self)
		{
			MakeAvailable(Self, Asset::IronRod);
			PlaceScene(Self, *S, Columns * Spacing / 2.0, 2000.0);
			S->Center.Z += 2000.0;
			S->Area.MinZ = S->Center.Z - 500.0;
			S->Area.MaxZ = S->Center.Z + 500.0;
			AddControlMachines(Self, *S, Asset::IronPlate, 0, 0);
			ListenResults(Self, S);
			return EStep::Next;
		});
		for (int32 Batch = 0; Batch < Columns * Rows / 100; ++Batch)
		{
			R.Steps.Add([S, Batch](FRun& Self)
			{
				for (int32 Index = Batch * 100; Index < (Batch + 1) * 100; ++Index)
				{
					const FVector At = S->Center + FVector((Index % Columns - Columns / 2.0 + 0.5) * Spacing, (Index / Columns - Rows / 2.0 + 0.5) * Spacing, 0.0);
					S->Inside.Add(SpawnMachine(Self, Asset::Constructor, At, Asset::IronPlate, 0, 0));
				}
				return EStep::Next;
			});
		}
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([S](FRun& Self)
		{
			Self.Check(S->Inside.Num() == Columns * Rows && AllValid(S->Inside) && AllValid(S->Controls), FString::Printf(TEXT("%d machines built"), S->Inside.Num()));
			Self.MaxFrameMs = 0.0;
			Self.bMeasureFrames = true;
			Self.Step(TEXT("request 201: SetRecipe for 2000 machines"));
			UAreaActionsRCO::Of(Self.GetPlayer())->ServerSetRecipe(201, S->Area, Recipe(Asset::IronRod));
			return EStep::Next;
		});
		R.Steps.Add([S](FRun& Self)
		{
			const FAreaActionsResult* Result = S->Results.Find(201);
			if (!Result)
			{
				return EStep::Wait;
			}
			Self.bMeasureFrames = false;
			Self.Check(Result->Code == EAreaActionsResultCode::Ok && Result->Buildings == Columns * Rows, FString::Printf(TEXT("result: code %d, buildings %d"), static_cast<int32>(Result->Code), Result->Buildings));
			int32 Changed = 0;
			for (const TWeakObjectPtr<AFGBuildable>& Built : S->Inside)
			{
				const AFGBuildableManufacturer* Constructor = Cast<AFGBuildableManufacturer>(Built.Get());
				Changed += Constructor && Constructor->GetCurrentRecipe() == Recipe(Asset::IronRod) ? 1 : 0;
			}
			Self.Check(Changed == Columns * Rows, FString::Printf(TEXT("machines with the new recipe: %d"), Changed));
			CheckControlMachines(Self, *S, Asset::IronPlate, 0);
			Self.Check(Self.MaxFrameMs <= 200.0, FString::Printf(TEXT("longest frame during the job: %.1f ms (W5: 200)"), Self.MaxFrameMs));
			return EStep::Next;
		});
	}

	bool BuildScenario(FRun& R)
	{
		if (R.Name == TEXT("SetRecipe"))
		{
			AddSetRecipe(R);
			return true;
		}
		if (R.Name == TEXT("SetRecipeNoMatch"))
		{
			AddSetRecipeUnchanged(R, Asset::IronPlate, Asset::IngotIron, TEXT("No machine in the area accepts"));
			return true;
		}
		if (R.Name == TEXT("SetRecipeAlready"))
		{
			AddSetRecipeUnchanged(R, Asset::IronRod, Asset::IronRod, TEXT("All machines in the area already use"));
			return true;
		}
		if (R.Name == TEXT("ClearInventories"))
		{
			AddClearInventories(R);
			return true;
		}
		if (R.Name == TEXT("SetStation"))
		{
			AddSetStation(R);
			return true;
		}
		if (R.Name == TEXT("SetStationEmpty"))
		{
			AddSetStationEmpty(R);
			return true;
		}
		if (R.Name == TEXT("RejectBadRequests"))
		{
			AddRejectBadRequests(R);
			return true;
		}
		if (R.Name == TEXT("SetRecipeLarge"))
		{
			R.StepTimeoutSec = 120.0;
			AddSetRecipeLarge(R);
			return true;
		}
		if (R.Name == TEXT("ToolMenu"))
		{
			AddToolMenu(R);
			return true;
		}
		if (R.Name == TEXT("Corners"))
		{
			AddCorners(R);
			return true;
		}
		if (R.Name == TEXT("SelectBuilding"))
		{
			AddSelectBuilding(R);
			return true;
		}
		if (R.Name == TEXT("Smoke"))
		{
			AddSmoke(R, false);
			return true;
		}
		if (R.Name == TEXT("SmokeFail"))
		{
			AddSmoke(R, true);
			return true;
		}
		return false;
	}

	void Finish(FRun& R)
	{
		R.bEnded = true;
		const bool bPass = R.Total > 0 && R.Passed == R.Total;
		UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST RESULT %s %s %d/%d"), *R.Name, bPass ? TEXT("PASS") : TEXT("FAIL"), R.Passed, R.Total);
		GLog->Flush();
		if (FParse::Param(FCommandLine::Get(), TEXT("AATestStay")))
		{
			return;
		}
		// Exit from the next core tick, never from inside a world tick or timer
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			FPlatformMisc::RequestExit(false, TEXT("AreaActions test mode"));
			return false;
		}));
	}

	bool Tick(float)
	{
		if (!Run || Run->bEnded)
		{
			return false;
		}
		FRun& R = *Run;
		if (R.bMeasureFrames)
		{
			R.MaxFrameMs = FMath::Max(R.MaxFrameMs, FApp::GetDeltaTime() * 1000.0);
		}
		if (!R.World.IsValid())
		{
			R.Check(false, TEXT("the game world is gone"));
			Finish(R);
			return false;
		}
		// Let the load settle: 3 s and 10 frames
		if (!R.bStarted)
		{
			if (R.Now() - R.Started < 3.0 || GFrameCounter - R.StartFrame < 10)
			{
				return true;
			}
			R.bStarted = true;
			R.StepStarted = R.Now();
			R.Step(TEXT("start"));
		}
		while (R.Index < R.Steps.Num())
		{
			if (R.Steps[R.Index](R) == EStep::Wait)
			{
				if (R.Now() - R.StepStarted > R.StepTimeoutSec)
				{
					R.Check(false, FString::Printf(TEXT("step %d timed out after %.0f s"), R.Index + 1, R.StepTimeoutSec));
					Finish(R);
					return false;
				}
				return true;
			}
			++R.Index;
			R.StepStarted = R.Now();
		}
		Finish(R);
		return false;
	}
}

void FAreaActionsTests::StartFromCommandLine(UWorld* World)
{
	FString Name;
	if (Run || !World || !FParse::Value(FCommandLine::Get(), TEXT("AATest="), Name))
	{
		return;
	}
	Run = MakeUnique<FRun>();
	Run->Name = Name;
	Run->World = World;
	Run->Started = FPlatformTime::Seconds();
	Run->StartFrame = GFrameCounter;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step scenario in %s, net mode %d"), *Name, *World->GetMapName(), static_cast<int32>(World->GetNetMode()));
	if (!BuildScenario(*Run))
	{
		Run->Check(false, TEXT("unknown scenario"));
		Finish(*Run);
		return;
	}
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
}

#endif

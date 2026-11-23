#include "AreaActionsTests.h"

#if AA_WITH_TESTS

#include "AreaActionBase.h"
#include "Algo/Count.h"
#include "AreaActionsBuildings.h"
#include "AreaActionsCopy.h"
#include "Hologram/FGHologram.h"
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Equipment/FGChainsaw.h"
#include "AreaActionsJobs.h"
#include "Algo/AnyOf.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "Buildables/FGBuildableWire.h"
#include "FGPowerConnectionComponent.h"
#include "HAL/IConsoleManager.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "FGCentralStorageSubsystem.h"
#include "FGGameState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FGBlueprintFunctionLibrary.h"
#include "FGBuildableBeam.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsSimpleActions.h"
#include "AbstractInstanceManager.h"
#include "GameFramework/CharacterMovementComponent.h"
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
#include "Components/TextBlock.h"
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
		// Save, back to the menu and load in this process: the run waits for the next game world (StartFromCommandLine hands it over)
		bool bAwaitWorld = false;
		double AwaitStarted = 0.0;
		FString LoadSave;
		bool bLoadIssued = false;

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

	// AA-077: every scenario starts at the same place, whatever start a new game picks. A flat meadow where the tool's clicks and the
	// scenes worked in the runs of chats 04 and 05; the player looks along +X.
	const FVector2D TestPlace(-246000.0, -62600.0);

	bool TraceGround(UWorld* World, const FVector2D& At, FHitResult& Hit)
	{
		FCollisionQueryParams Params(TEXT("AATestPlace"), false);
		if (APlayerController* Player = UGameplayStatics::GetPlayerController(World, 0))
		{
			Params.AddIgnoredActor(Player->GetPawn());
		}
		return World->LineTraceSingleByChannel(Hit, FVector(At, 100000.0), FVector(At, -100000.0), ECC_WorldStatic, Params);
	}

	void AddPlace(FRun& R)
	{
		R.Steps.Add([](FRun& Self)
		{
			AFGCharacterPlayer* Character = GetCharacter(Self);
			if (!Character)
			{
				return EStep::Wait;
			}
			// Flying until the ground there has streamed in, so the player does not fall through it
			Self.Step(FString::Printf(TEXT("teleport to X=%.0f Y=%.0f from %s"), TestPlace.X, TestPlace.Y, *Character->GetActorLocation().ToCompactString()));
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Character->TeleportTo(FVector(TestPlace, 30000.0), FRotator::ZeroRotator);
			Self.GetPlayer()->SetControlRotation(FRotator::ZeroRotator);
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			FHitResult Hit;
			if (!TraceGround(Self.GetWorld(), TestPlace, Hit))
			{
				return EStep::Wait;
			}
			AFGCharacterPlayer* Character = GetCharacter(Self);
			const FVector At = Hit.Location + FVector(0, 0, Character->GetSimpleCollisionHalfHeight() + 20.0);
			Character->TeleportTo(At, FRotator::ZeroRotator);
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
			Self.GetPlayer()->SetControlRotation(FRotator::ZeroRotator);
			Self.Check(true, FString::Printf(TEXT("place X=%.0f Y=%.0f Z=%.0f, ground %s"), At.X, At.Y, At.Z, *GetNameSafe(Hit.GetActor())));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
	}

	// What the tool's own trace hits if it clicks now (Equip.RaycastMouseWithRange: from the camera, MaxDistance, TraceTypeQuery3)
	bool AimHit(const FRun& R, FHitResult& Hit)
	{
		APlayerController* Player = R.GetPlayer();
		AAreaActionsEquipment* Tool = GetTool(R);
		const double Range = Tool ? DoubleValue(Tool, TEXT("MaxDistance")) : 0.0;
		if (!Player || !Player->PlayerCameraManager || !(Range > 0.0))
		{
			return false;
		}
		const FVector From = Player->PlayerCameraManager->GetCameraLocation();
		const FVector To = From + Player->PlayerCameraManager->GetCameraRotation().Vector() * Range;
		FCollisionQueryParams Params(TEXT("AATestAim"), false, Player->GetPawn());
		Params.AddIgnoredActor(Tool);
		return R.GetWorld()->LineTraceSingleByChannel(Hit, From, To, UEngineTypes::ConvertToCollisionChannel(TraceTypeQuery3), Params);
	}

	// The actor the hit belongs to, also for meshes drawn as abstract instances
	AActor* HitOwner(UWorld* World, const FHitResult& Hit)
	{
		FInstanceHandle Handle;
		AAbstractInstanceManager* Manager = AAbstractInstanceManager::GetInstanceManager(World);
		if (Manager && Manager->ResolveHit(Hit, Handle))
		{
			return Handle.GetOwner();
		}
		return Hit.GetActor();
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
			if (GetTool(Self))
			{
				// A scenario with several scenes takes the tool once
				return EStep::Next;
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
				// AA-077: a click that cannot land fails with its cause
				FHitResult Hit;
				const bool bHit = AimHit(Self, Hit);
				const double Off = bHit && Targets->IsValidIndex(Index) ? FVector::Dist(Hit.Location, (*Targets)[Index]) : -1.0;
				Self.Check(bHit && Off <= 100.0, FString::Printf(TEXT("aim %d hits %s at %s, %.0f cm from the target"), Index + 1,
					bHit ? *GetNameSafe(Hit.GetActor()) : TEXT("nothing within the tool's range"), bHit ? *Hit.Location.ToCompactString() : TEXT("-"), Off));
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
		R.Steps.Add([Built](FRun& Self)
		{
			FHitResult Hit;
			const bool bHit = AimHit(Self, Hit);
			AActor* Owner = bHit ? HitOwner(Self.GetWorld(), Hit) : nullptr;
			Self.Check(Owner && Owner == Built->Get(), FString::Printf(TEXT("aim hits %s (owner %s), the Constructor is %s"),
				bHit ? *GetNameSafe(Hit.GetActor()) : TEXT("nothing within the tool's range"), *GetNameSafe(Owner), *GetNameSafe(Built->Get())));
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
		const TCHAR* Splitter = TEXT("/Game/FactoryGame/Buildable/Factory/CA_Splitter/Build_ConveyorAttachmentSplitter.Build_ConveyorAttachmentSplitter_C");
		const TCHAR* WaterItem = TEXT("/Game/FactoryGame/Resource/RawResources/Water/Desc_Water.Desc_Water_C");
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

	void PlaceScene(const FRun& R, FScene& S, double HalfSize, double Ahead, double Side = 0.0)
	{
		const APawn* Pawn = R.GetPlayer()->GetPawn();
		const FVector Forward = Pawn->GetActorForwardVector().GetSafeNormal2D();
		const FVector Right = FVector(-Forward.Y, Forward.X, 0.0);
		S.HalfSize = HalfSize;
		S.Center = Pawn->GetActorLocation() + Forward * (HalfSize + Ahead) + Right * Side + FVector(0, 0, 300);
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

	// What the player sees: the list takes a real part of the screen and its rows show recipe names
	void AddCheckRecipeWindowLayout(FRun& R)
	{
		AddOnWindow(R, TEXT("Widget_SetRecipe"), [](FRun& Self, UUserWidget* Window) {});
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			UUserWidget* Window = TopWindow(Self);
			UListView* List = Window ? Cast<UListView>(Window->GetWidgetFromName(TEXT("RecipeList"))) : nullptr;
			FVector2D Viewport = FVector2D::ZeroVector;
			if (GEngine && GEngine->GameViewport)
			{
				GEngine->GameViewport->GetViewportSize(Viewport);
			}
			const double Width = List ? List->GetCachedGeometry().GetAbsoluteSize().X : 0.0;
			Self.Check(Viewport.X > 0.0 && Width >= Viewport.X * 0.3,
				FString::Printf(TEXT("recipe list width %.0f of viewport %.0f (at least 30%%)"), Width, Viewport.X));
			const TArray<UUserWidget*> Rows = List ? List->GetDisplayedEntryWidgets() : TArray<UUserWidget*>();
			const UTextBlock* Text = Rows.IsEmpty() ? nullptr : Cast<UTextBlock>(Rows[0]->GetWidgetFromName(TEXT("ItemText")));
			const FString Name = Text ? Text->GetText().ToString() : FString();
			// The name is readable when its box is as wide as the text wants
			const double TextWidth = Text ? Text->GetCachedGeometry().GetLocalSize().X : 0.0;
			const double TextWants = Text ? Text->GetDesiredSize().X : 0.0;
			Self.Check(Rows.Num() > 0 && !Name.IsEmpty() && TextWants > 0.0 && TextWidth >= TextWants * 0.9,
				FString::Printf(TEXT("rows shown %d, first row '%s', text width %.0f of %.0f wanted"), Rows.Num(), *Name, TextWidth, TextWants));
			return EStep::Next;
		});
	}

	void AddPickRecipe(FRun& R, const TCHAR* RecipePath)
	{
		AddCheckRecipeWindowLayout(R);
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
	void AddScene(FRun& R, const FSceneRef& S, double HalfSize, TFunction<void(FRun&, FScene&)> Build, double Side = 0.0)
	{
		AddEquipTool(R);
		R.Steps.Add([S, HalfSize, Build, Side](FRun& Self)
		{
			PlaceScene(Self, *S, HalfSize, 1000.0, Side);
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

	// bToCrate: the second answer, the items go to one new crate at the player
	void AddClearInventories(FRun& R, bool bToCrate)
	{
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<TWeakObjectPtr<AFGCrate>> Crate = MakeShared<TWeakObjectPtr<AFGCrate>>();
		AddScene(R, S, 4000.0, [Crate](FRun& Self, FScene& Scene)
		{
			Scene.Inside.Add(SpawnStorage(Self, Scene.Slot(0), 50, 30));
			Scene.Inside.Add(SpawnStorage(Self, Scene.Slot(1), 20, 0));
			AFGBuildableManufacturer* Constructor = SpawnMachine(Self, Asset::Constructor, Scene.Slot(2), Asset::IronPlate, 0, 5);
			Scene.Inside.Add(Constructor);
			// AA-079: 2 m3 of water in the input (past its filters) is reported apart; an empty container and an empty splitter do not count
			UFGInventoryComponent* Input = Constructor ? Constructor->GetInputInventory() : nullptr;
			int32 Water = 0;
			if (Input && Input->GetSizeLinear() > 0)
			{
				const FItemFilter ItemFilter = Input->mItemFilter;
				const FFormFilter FormFilter = Input->mFormFilter;
				Input->mItemFilter.Unbind();
				Input->mFormFilter.Unbind();
				// The recipe also pins its ingredient to the slot
				const TSubclassOf<UFGItemDescriptor> Allowed = Input->GetAllowedItemOnIndex(0);
				Input->SetAllowedItemOnIndex(0, nullptr);
				Water = Input->AddStackToIndex(0, FInventoryStack(2000, Item(Asset::WaterItem)), true);
				Input->SetAllowedItemOnIndex(0, Allowed);
				Input->mItemFilter = ItemFilter;
				Input->mFormFilter = FormFilter;
			}
			Self.Check(Water == 2000, FString::Printf(TEXT("water in the Constructor's input: %d of 2000"), Water));
			Scene.Inside.Add(SpawnStorage(Self, Scene.Slot(3), 0, 0));
			Scene.Inside.Add(SpawnBuildable(Self, Asset::Splitter, Scene.Slot(4)));
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
			Scene.Expected = 50 + 30 + 20 + 5 + 25;
		});
		AddOpenAction(R, TEXT("ClearInventories"));
		AddPress(R, TEXT("Widget_MessageYesNo"), bToCrate ? TEXT("NoButton") : TEXT("YesButton"));
		AddResultWindow(R, S);
		R.Steps.Add([S, Crate, bToCrate](FRun& Self)
		{
			const FString Want = FString::Printf(TEXT("Cleared 4 buildings %s a total of %d items"), bToCrate ? TEXT("taking") : TEXT("deleting"), S->Expected);
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			Self.Check(S->Message.Contains(TEXT("2 m³ of fluids were deleted")), FString::Printf(TEXT("result '%s' names 2 m³ of fluids"), *S->Message));
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
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			if (!bToCrate)
			{
				Self.Check(Crates.IsEmpty(), FString::Printf(TEXT("no new crate: %d"), Crates.Num()));
				return EStep::Next;
			}
			// Plates 50 + 20 + 5, ingots 30, rods 25: every solid item of the area, no water, nothing more
			const UFGInventoryComponent* Inventory = Crates.Num() == 1 ? Crates[0]->GetInventory() : nullptr;
			const int32 Plates = Inventory ? Inventory->GetNumItems(Item(Asset::IronPlateItem)) : -1;
			const int32 Ingots = Inventory ? Inventory->GetNumItems(Item(Asset::IronIngotItem)) : -1;
			const int32 Rods = Inventory ? Inventory->GetNumItems(Item(Asset::IronRodItem)) : -1;
			const int32 All = Inventory ? Inventory->GetNumItems(nullptr) : -1;
			Self.Check(Crates.Num() == 1 && Plates == 75 && Ingots == 30 && Rods == 25 && All == S->Expected,
				FString::Printf(TEXT("new crates %d; plates %d of 75, ingots %d of 30, rods %d of 25, all %d of %d"), Crates.Num(), Plates, Ingots, Rods, All, S->Expected));
			const APawn* Pawn = Self.GetPlayer()->GetPawn();
			const double Distance = Crates.Num() == 1 ? FVector::Dist(Crates[0]->GetActorLocation(), Pawn->GetActorLocation()) : -1.0;
			Self.Check(Distance >= 0.0 && Distance < 1500.0, FString::Printf(TEXT("crate next to the player: %.0f cm"), Distance));
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
			// AA-081: one space after the comma
			const FString Want = FString::Printf(TEXT("Set 1 Truck Stations, 1 Freight Platforms to %s"), bLoad ? TEXT("Load") : TEXT("Unload"));
			Self.Check(S->Message.Equals(Want), FString::Printf(TEXT("result '%s' is '%s'"), *S->Message, *Want));
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

	// X on the question window: the action ends and nothing changes (before, closing meant Unload)
	void AddSetStationCancel(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
		{
			Scene.Inside.Add(SpawnBuildable(Self, Asset::TruckStation, Scene.Slot(0)));
			SetLoadMode(Scene.Inside[0].Get(), true);
			AddControlStations(Self, Scene);
		});
		AddOpenAction(R, TEXT("SetStationLoadUnload"));
		AddOnWindow(R, TEXT("Widget_MessageYesNo"), [](FRun& Self, UUserWidget* Window)
		{
			UUserWidget* Frame = Cast<UUserWidget>(Window->GetWidgetFromName(TEXT("WindowDark")));
			FMulticastDelegateProperty* Property = Frame ? FindFProperty<FMulticastDelegateProperty>(Frame->GetClass(), TEXT("OnClose")) : nullptr;
			if (Property)
			{
				Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Frame)->ProcessMulticastDelegate<UObject>(nullptr);
			}
			Self.Check(Property != nullptr, TEXT("window closed with X"));
		});
		R.Steps.Add([S](FRun& Self)
		{
			if (ActiveAction(Self) || TopWindow(Self))
			{
				return EStep::Wait;
			}
			Self.Check(true, TEXT("action ended, no window left"));
			const AFGBuildable* Station = S->Inside[0].Get();
			Self.Check(Station && LoadMode(Station), TEXT("station still loads"));
			CheckControlStations(Self, *S);
			return EStep::Next;
		});
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
			UAreaActionsRCO::Of(Self.GetPlayer())->ServerClearInventories(104, S->Area, false);
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

	// --- Lightweight buildings and the copy engine (AA-020) ---

	// A class of the game by its asset name, wherever the game keeps it; the path found is logged once
	UClass* GameClass(const FString& AssetName)
	{
		static TMap<FString, FString> Paths;
		if (Paths.IsEmpty())
		{
			TArray<FAssetData> Assets;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(TEXT("/Game/FactoryGame")), Assets, true);
			for (const FAssetData& Asset : Assets)
			{
				FString Short = Asset.AssetName.ToString();
				Short.RemoveFromEnd(TEXT("_C"));
				Paths.Add(Short, Asset.PackageName.ToString() + TEXT(".") + Short + TEXT("_C"));
			}
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST asset registry: %d assets under /Game/FactoryGame"), Assets.Num());
		}
		const FString* Path = Paths.Find(AssetName);
		UClass* Class = Path ? LoadClass<UObject>(nullptr, **Path) : nullptr;
		UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST class %s: %s"), *AssetName, Class ? **Path : TEXT("not found"));
		return Class;
	}

	TSubclassOf<UFGRecipe> RecipeOfClass(const FRun& R, UClass* Class, const TCHAR* RecipeName)
	{
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(R.GetWorld());
		TSubclassOf<UFGRecipe> Found = Lightweights && Class ? Lightweights->GetBuiltWithRecipeForBuildableClass(Class) : nullptr;
		return Found ? Found : TSubclassOf<UFGRecipe>(GameClass(RecipeName));
	}

	FFactoryCustomizationData TestColor()
	{
		FFactoryCustomizationData Color;
		Color.SwatchDesc = GameClass(TEXT("SwatchDesc_Custom"));
		Color.OverrideColorData = FFactoryCustomizationColorSlot(FLinearColor(0.8f, 0.1f, 0.1f), FLinearColor(0.1f, 0.1f, 0.8f));
		return Color;
	}

	const FRuntimeBuildableInstanceData* FindLightweight(UWorld* World, UClass* Class, const FVector& At, int32* OutIndex = nullptr)
	{
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
		const TArray<FRuntimeBuildableInstanceData>* Array = Lightweights ? Lightweights->GetAllLightweightBuildableInstances().Find(Class) : nullptr;
		for (int32 Index = 0; Array && Index < Array->Num(); ++Index)
		{
			if ((*Array)[Index].IsValid() && FVector::Dist((*Array)[Index].Transform.GetLocation(), At) <= 1.0)
			{
				if (OutIndex)
				{
					*OutIndex = Index;
				}
				return &(*Array)[Index];
			}
		}
		return nullptr;
	}

	AFGBuildable* FindActor(UWorld* World, UClass* Class, const FVector& At)
	{
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
		if (!Buildables)
		{
			return nullptr;
		}
		for (AFGBuildable* Actor : Buildables->GetAllBuildablesRef())
		{
			if (IsValid(Actor) && Actor->GetClass() == Class && FVector::Dist(Actor->GetActorLocation(), At) <= 1.0)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	float BeamLength(const FFGDynamicStruct& Data)
	{
		return Data.IsValid() && Data.GetStruct() == FBuildableBeamLightweightData::StaticStruct()
			? static_cast<const FBuildableBeamLightweightData*>(Data.GetStructValueRaw())->BeamLength : -1.0f;
	}

	// Save, back to the main menu, load the save: the steps after this run in the loaded world
	void AddSaveAndLoad(FRun& R, const FString& SaveName)
	{
		TSharedRef<int32> Saved = MakeShared<int32>(-1);
		R.Steps.Add([Saved, SaveName](FRun& Self)
		{
			UFGSaveSession* Session = UFGSaveSession::Get(Self.GetWorld());
			if (!Self.Check(Session != nullptr, TEXT("save session")))
			{
				return EStep::Next;
			}
			Self.Step(FString::Printf(TEXT("saving %s"), *SaveName));
			Session->SaveGame(SaveName, FOnSaveGameComplete::CreateLambda([Saved](bool bOk, const FText& Error, void*)
			{
				*Saved = bOk ? 1 : 0;
				UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST save done: %d %s"), bOk, *Error.ToString());
			}), nullptr);
			return EStep::Next;
		});
		R.Steps.Add([Saved, SaveName](FRun& Self)
		{
			if (*Saved < 0)
			{
				return EStep::Wait;
			}
			if (!Self.Check(*Saved == 1, FString::Printf(TEXT("saved %s"), *SaveName)))
			{
				return EStep::Next;
			}
			Self.bAwaitWorld = true;
			Self.bLoadIssued = false;
			Self.LoadSave = SaveName;
			Self.AwaitStarted = Self.Now();
			Self.Step(TEXT("to the main menu"));
			UFGBlueprintFunctionLibrary::TravelToMainMenu(Self.GetPlayer());
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
	}

	// The test's save leaves the player's save folder again
	void AddDeleteSave(FRun& R, const FString& SaveName)
	{
		R.Steps.Add([SaveName](FRun& Self)
		{
			if (UFGSaveSystem* Saves = UFGSaveSystem::Get(Self.GetWorld()))
			{
				Saves->DeleteSaveFiles({ SaveName }, FOnDeleteSaveGameComplete::CreateLambda([SaveName](bool bOk, void*)
				{
					UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST save %s deleted: %d"), *SaveName, bOk);
				}), nullptr);
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
	}

	// R6, R7: what the copy engine builds, lightweight and actor, survives a save and a load with its data
	void AddSpawnProbe(FRun& R)
	{
		struct FProbe
		{
			FVector Foundation, Beam, Machine;
			FTransform BeamTransform;
		};
		TSharedRef<FProbe> P = MakeShared<FProbe>();
		const FString SaveName = TEXT("AATest_SpawnProbe");
		auto Check = [P](FRun& Self, const TCHAR* When)
		{
			UWorld* World = Self.GetWorld();
			UClass* FoundationClass = GameClass(TEXT("Build_Foundation_8x1_01"));
			UClass* BeamClass = GameClass(TEXT("Build_Beam"));
			const FFactoryCustomizationData Color = TestColor();
			const FRuntimeBuildableInstanceData* Foundation = FindLightweight(World, FoundationClass, P->Foundation);
			Self.Check(Foundation && Foundation->CustomizationData == Color && Foundation->BuiltWithRecipe,
				FString::Printf(TEXT("%s: lightweight foundation at %s, color kept %d, recipe %s"), When, *P->Foundation.ToCompactString(),
					Foundation && Foundation->CustomizationData == Color, Foundation ? *GetNameSafe(Foundation->BuiltWithRecipe) : TEXT("-")));
			const FRuntimeBuildableInstanceData* Beam = FindLightweight(World, BeamClass, P->Beam);
			Self.Check(Beam && FMath::IsNearlyEqual(BeamLength(Beam->TypeSpecificData), 12.0f) && Beam->Transform.GetRotation().Equals(P->BeamTransform.GetRotation(), 1e-4),
				FString::Printf(TEXT("%s: lightweight beam, length %.2f (want 12)"), When, Beam ? BeamLength(Beam->TypeSpecificData) : -1.0f));
			const AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(FindActor(World, LoadClass<AFGBuildable>(nullptr, Asset::Constructor), P->Machine));
			Self.Check(Machine && Machine->GetCurrentRecipe() == Recipe(Asset::IronPlate) && Machine->GetCustomizationData_Native() == Color && Machine->GetBuiltWithRecipe(),
				FString::Printf(TEXT("%s: Constructor actor, recipe %s, color kept %d, built with %s"), When, Machine ? *GetNameSafe(Machine->GetCurrentRecipe()) : TEXT("-"),
					Machine && Machine->GetCustomizationData_Native() == Color, Machine ? *GetNameSafe(Machine->GetBuiltWithRecipe()) : TEXT("-")));
		};
		R.Steps.Add([P](FRun& Self)
		{
			using namespace AreaActionsBuildings;
			UWorld* World = Self.GetWorld();
			const FVector Base = Self.GetPlayer()->GetPawn()->GetActorLocation() + FVector(3000.0, 0.0, 0.0);
			const FFactoryCustomizationData Color = TestColor();
			FSnapshot Foundation;
			Foundation.Class = GameClass(TEXT("Build_Foundation_8x1_01"));
			Foundation.BuiltWith = RecipeOfClass(Self, Foundation.Class, TEXT("Recipe_Foundation_8x1_01"));
			Foundation.Customization = Color;
			P->Foundation = Base;
			Self.Check(IsLightweightClass(Foundation.Class) && Spawn(World, Foundation, FTransform(FRotator(0, 15, 0), P->Foundation)),
				FString::Printf(TEXT("foundation built through AddFromBuildableInstanceData (lightweight class %d)"), IsLightweightClass(Foundation.Class)));
			FSnapshot Beam;
			Beam.Class = GameClass(TEXT("Build_Beam"));
			Beam.BuiltWith = RecipeOfClass(Self, Beam.Class, TEXT("Recipe_Beam"));
			FBuildableBeamLightweightData Length;
			Length.BeamLength = 12.0f;
			Beam.TypeData.InitializeAs(Length);
			P->Beam = Base + FVector(0.0, 1500.0, 400.0);
			P->BeamTransform = FTransform(FRotator(0, 30, 0), P->Beam);
			Self.Check(IsLightweightClass(Beam.Class) && Spawn(World, Beam, P->BeamTransform),
				FString::Printf(TEXT("beam built through AddFromBuildableInstanceData (lightweight class %d)"), IsLightweightClass(Beam.Class)));
			FSnapshot Machine;
			Machine.Class = LoadClass<AFGBuildable>(nullptr, Asset::Constructor);
			Machine.BuiltWith = RecipeOfClass(Self, Machine.Class, TEXT("Recipe_ConstructorMk1"));
			Machine.Customization = Color;
			Machine.MachineRecipe = Recipe(Asset::IronPlate);
			P->Machine = Base + FVector(0.0, -1500.0, 100.0);
			Self.Check(!IsLightweightClass(Machine.Class) && Spawn(World, Machine, FTransform(FRotator(0, -20, 0), P->Machine)), TEXT("Constructor built through BeginSpawnBuildable"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([Check](FRun& Self) { Check(Self, TEXT("built")); return EStep::Next; });
		AddSaveAndLoad(R, SaveName);
		R.Steps.Add([Check](FRun& Self) { Check(Self, TEXT("loaded")); return EStep::Next; });
		AddDeleteSave(R, SaveName);
	}

	// --- Copy and Fill (AA-020, AA-071) ---

	// One building of a pattern, as the test placed it
	struct FPiece
	{
		UClass* Class = nullptr;
		FTransform Transform;
		bool bLightweight = false;
		bool bIgnored = false;
		float BeamLength = -1.0f;
		TSubclassOf<UFGRecipe> BuiltWith;
		// The classes by path: a load replaces the world, the pointers above may not survive it
		FString ClassPath, RecipePath;

		void Resolve()
		{
			Class = LoadClass<UObject>(nullptr, *ClassPath);
			BuiltWith = RecipePath.IsEmpty() ? nullptr : LoadClass<UFGRecipe>(nullptr, *RecipePath);
		}
	};

	FPiece MakePiece(UClass* Class, const FTransform& Transform, bool bLightweight, float Length, TSubclassOf<UFGRecipe> BuiltWith)
	{
		FPiece Piece{ Class, Transform, bLightweight, AreaActionsBuildings::ShouldIgnore(Class), Length, BuiltWith };
		Piece.ClassPath = Class->GetPathName();
		Piece.RecipePath = BuiltWith ? BuiltWith->GetPathName() : FString();
		return Piece;
	}

	struct FCopyState
	{
		TArray<FPiece> Pattern;
		int32 ActorsBefore = 0;
		int32 LightweightsBefore = 0;
		// Copy: the expected move and turn
		FVector Delta = FVector::ZeroVector;
		double Yaw = 0.0;
		FVector Pivot = FVector::ZeroVector;
		AreaActionsBuildings::FItemCounts Cost;
		AreaActionsBuildings::FItemCounts Inventory, Near, Far, Depot;
		TWeakObjectPtr<AFGBuildableStorage> NearBox, FarBox;
		FVector Base = FVector::ZeroVector;
		int32 JobsBefore = 0;
	};
	using FCopyRef = TSharedRef<FCopyState>;

	int32 LightweightCount(UWorld* World)
	{
		int32 Count = 0;
		if (AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World))
		{
			for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Entry : Lightweights->GetAllLightweightBuildableInstances())
			{
				for (const FRuntimeBuildableInstanceData& Data : Entry.Value)
				{
					Count += Data.IsValid() ? 1 : 0;
				}
			}
		}
		return Count;
	}

	int32 ActorCount(UWorld* World)
	{
		const AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
		return Buildables ? Buildables->GetAllBuildablesRef().Num() : 0;
	}

	void CountWorld(const FRun& R, FCopyState& C)
	{
		C.ActorsBefore = ActorCount(R.GetWorld());
		C.LightweightsBefore = LightweightCount(R.GetWorld());
	}

	FVector Ground(const FRun& R, const FVector& At)
	{
		FVector Hit;
		return GroundBelow(R.GetWorld(), At, Hit) ? Hit + FVector(0, 0, 50) : At;
	}

	void AddLightweight(const FRun& R, FCopyState& C, const TCHAR* ClassName, const TCHAR* RecipeName, const FTransform& Transform, float Length = -1.0f)
	{
		AreaActionsBuildings::FSnapshot Building;
		Building.Class = GameClass(ClassName);
		Building.BuiltWith = RecipeOfClass(R, Building.Class, RecipeName);
		if (Length > 0.0f)
		{
			FBuildableBeamLightweightData Data;
			Data.BeamLength = Length;
			Building.TypeData.InitializeAs(Data);
		}
		const bool bBuilt = Building.Class && AreaActionsBuildings::Spawn(R.GetWorld(), Building, Transform);
		UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST pattern %s at %s: %d"), ClassName, *Transform.GetLocation().ToCompactString(), bBuilt);
		if (bBuilt)
		{
			C.Pattern.Add(MakePiece(Building.Class, Transform, true, Length, Building.BuiltWith));
		}
	}

	AFGBuildable* AddActor(FRun& R, FScene& S, FCopyState& C, const TCHAR* ClassName, const TCHAR* RecipeName, const FTransform& Transform, TFunction<void(AFGBuildable*)> Before = nullptr)
	{
		UClass* Class = GameClass(ClassName);
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(R.GetWorld());
		AFGBuildable* Actor = Class && Buildables ? Buildables->BeginSpawnBuildable(Class, Transform) : nullptr;
		if (!Actor)
		{
			R.Check(false, FString::Printf(TEXT("pattern actor %s"), ClassName));
			return nullptr;
		}
		const TSubclassOf<UFGRecipe> With = RecipeOfClass(R, Class, RecipeName);
		Actor->SetBuiltWithRecipe(With);
		if (Before)
		{
			Before(Actor);
		}
		Actor->FinishSpawning(Transform);
		S.Inside.Add(Actor);
		C.Pattern.Add(MakePiece(Class, Transform, false, -1.0f, With));
		return Actor;
	}

	// The storage containers that must stay untouched (W4): outside the polygon, below MinZ, above MaxZ
	void AddControlStorages(FRun& R, FScene& S)
	{
		for (const FVector& Place : S.ControlPlaces())
		{
			S.Controls.Add(SpawnBuildable(R, Asset::Storage, Place));
		}
	}

	// CopyBasic's pattern: 4 foundations (2x2), a wall, a beam of 6 m, a Constructor with a recipe, a Smart Splitter with a rule, a painted
	// container and a belt between two buildings
	void BuildBasic(FRun& R, FScene& S, FCopyState& C)
	{
		C.Base = Ground(R, S.Center);
		for (const FVector2D Offset : { FVector2D(-400, -400), FVector2D(400, -400), FVector2D(-400, 400), FVector2D(400, 400) })
		{
			AddLightweight(R, C, TEXT("Build_Foundation_8x1_01"), TEXT("Recipe_Foundation_8x1_01"), FTransform(C.Base + FVector(Offset, 0)));
		}
		AddLightweight(R, C, TEXT("Build_Wall_8x4_01"), TEXT("Recipe_Wall_8x4_01"), FTransform(C.Base + FVector(0, -800, 100)));
		AddLightweight(R, C, TEXT("Build_Beam"), TEXT("Recipe_Beam"), FTransform(C.Base + FVector(0, 0, 600)), 600.0f);
		AFGBuildable* Machine = AddActor(R, S, C, TEXT("Build_ConstructorMk1"), TEXT("Recipe_ConstructorMk1"), FTransform(C.Base + FVector(-2500, -1500, 50)));
		if (AFGBuildableManufacturer* Constructor = Cast<AFGBuildableManufacturer>(Machine))
		{
			Constructor->SetRecipe(Recipe(Asset::IronPlate));
		}
		AFGBuildable* Splitter = AddActor(R, S, C, TEXT("Build_ConveyorAttachmentSplitterSmart"), TEXT("Recipe_ConveyorAttachmentSplitterSmart"), FTransform(C.Base + FVector(-2500, 1500, 50)));
		if (AFGBuildableSplitterSmart* Smart = Cast<AFGBuildableSplitterSmart>(Splitter))
		{
			Smart->SetSortRules({ FSplitterSortRule(Item(Asset::IronPlateItem), 1) });
		}
		AddActor(R, S, C, TEXT("Build_StorageContainerMk1"), TEXT("Recipe_StorageContainerMk1"), FTransform(C.Base + FVector(-2500, 0, 50)), [](AFGBuildable* Actor)
		{
			Actor->SetCustomizationData_Native(TestColor());
		});
		AddActor(R, S, C, TEXT("Build_ConveyorBeltMk1"), TEXT("Recipe_ConveyorBeltMk1"), FTransform(C.Base + FVector(-2500, -1100, 150)), [](AFGBuildable* Actor)
		{
			if (TArray<FSplinePointData>* Spline = Cast<AFGBuildableConveyorBelt>(Actor)->GetMutableSplinePointData())
			{
				Spline->Reset();
				Spline->Emplace(FVector::ZeroVector, FVector(0, 600, 0));
				Spline->Emplace(FVector(0, 600, 0), FVector(0, 600, 0));
			}
		});
		AddControlStorages(R, S);
	}

	// FillGrid's pattern: one foundation; AddScene wants an actor in the area, the controls' outside container stands in for it
	void BuildSingle(FRun& R, FScene& S, FCopyState& C)
	{
		C.Base = Ground(R, S.Center);
		AddLightweight(R, C, TEXT("Build_Foundation_8x1_01"), TEXT("Recipe_Foundation_8x1_01"), FTransform(C.Base));
		AddControlStorages(R, S);
		S.Inside.Add(S.Controls[0]);
	}

	// The middle of the pattern's places in XY: the copy's default rotation centre (the original's AreaCenter)
	FVector PlacesCenter(const FCopyState& C)
	{
		FBox2D Box(ForceInit);
		for (const FPiece& Piece : C.Pattern)
		{
			Box += FVector2D(Piece.Transform.GetLocation());
		}
		return FVector(Box.GetCenter(), 0.0);
	}

	FTransform Expected(const FCopyState& C, const FPiece& Piece)
	{
		const FQuat Turn(FRotator(0, C.Yaw, 0));
		FTransform Out = Piece.Transform;
		Out.SetLocation(C.Pivot + C.Delta + Turn.RotateVector(Piece.Transform.GetLocation() - C.Pivot));
		Out.SetRotation(Turn * Piece.Transform.GetRotation());
		return Out;
	}

	bool SameRotation(const FQuat& A, const FQuat& B)
	{
		return FMath::RadiansToDegrees(A.AngularDistance(B)) <= 0.1;
	}

	// The copy of every piece the copy takes stands where expected (1 cm, 0.1 degree) with its data, the belt has none; with bPresent
	// false none of them stands there
	void CheckCopies(FRun& R, FCopyState& C, bool bPresent, const TCHAR* When)
	{
		UWorld* World = R.GetWorld();
		for (FPiece& Piece : C.Pattern)
		{
			Piece.Resolve();
		}
		int32 Found = 0, Taken = 0;
		for (const FPiece& Piece : C.Pattern)
		{
			const FTransform Want = Expected(C, Piece);
			if (Piece.bIgnored)
			{
				R.Check(!FindActor(World, Piece.Class, Want.GetLocation()), FString::Printf(TEXT("%s: no copy of %s"), When, *GetNameSafe(Piece.Class)));
				continue;
			}
			++Taken;
			bool bOk = false;
			FString Got = TEXT("none");
			if (Piece.bLightweight)
			{
				const FRuntimeBuildableInstanceData* Copy = FindLightweight(World, Piece.Class, Want.GetLocation());
				bOk = Copy && SameRotation(Copy->Transform.GetRotation(), Want.GetRotation()) && (Piece.BeamLength < 0.0f || FMath::IsNearlyEqual(BeamLength(Copy->TypeSpecificData), Piece.BeamLength));
				Got = Copy ? FString::Printf(TEXT("yaw %.2f, length %.1f"), Copy->Transform.Rotator().Yaw, BeamLength(Copy->TypeSpecificData)) : Got;
			}
			else if (const AFGBuildable* Copy = FindActor(World, Piece.Class, Want.GetLocation()))
			{
				bOk = SameRotation(Copy->GetActorQuat(), Want.GetRotation()) && Copy->GetBuiltWithRecipe() == Piece.BuiltWith;
				if (const AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Copy))
				{
					bOk &= Machine->GetCurrentRecipe() == Recipe(Asset::IronPlate);
				}
				if (const AFGBuildableSplitterSmart* Smart = Cast<AFGBuildableSplitterSmart>(Copy))
				{
					const TArray<FSplitterSortRule> Rules = Smart->GetSortRules();
					bOk &= Rules.Num() == 1 && Rules[0].ItemClass == Item(Asset::IronPlateItem) && Rules[0].OutputIndex == 1;
				}
				if (Copy->IsA<AFGBuildableStorage>())
				{
					bOk &= Copy->GetCustomizationData_Native() == TestColor();
				}
				Got = FString::Printf(TEXT("yaw %.2f, data kept %d"), Copy->GetActorRotation().Yaw, bOk);
			}
			Found += bOk ? 1 : 0;
			if (bPresent != bOk)
			{
				R.Check(false, FString::Printf(TEXT("%s: %s at %s yaw %.2f: %s"), When, *GetNameSafe(Piece.Class), *Want.GetLocation().ToCompactString(), Want.Rotator().Yaw, *Got));
			}
		}
		R.Check(bPresent ? Found == Taken : Found == 0, FString::Printf(TEXT("%s: copies in place %d of %d (want %d)"), When, Found, Taken, bPresent ? Taken : 0));
	}

	// The pattern itself and the controls are untouched
	void CheckPattern(FRun& R, const FScene& S, const FCopyState& C, const TCHAR* When)
	{
		int32 Kept = 0;
		for (const FPiece& Piece : C.Pattern)
		{
			Kept += (Piece.bLightweight ? FindLightweight(R.GetWorld(), Piece.Class, Piece.Transform.GetLocation()) != nullptr
				: FindActor(R.GetWorld(), Piece.Class, Piece.Transform.GetLocation()) != nullptr) ? 1 : 0;
		}
		int32 ControlCopies = 0;
		for (const TWeakObjectPtr<AFGBuildable>& Control : S.Controls)
		{
			ControlCopies += Control.IsValid() && FindActor(R.GetWorld(), Control->GetClass(), Control->GetActorLocation() + C.Delta) ? 1 : 0;
		}
		R.Check(Kept == C.Pattern.Num() && AllValid(S.Controls) && ControlCopies == 0,
			FString::Printf(TEXT("%s: pattern kept %d of %d, controls %d, copies of controls %d (want 0)"), When, Kept, C.Pattern.Num(), S.Controls.Num(), ControlCopies));
	}

	// A number typed into one of the windows' number boxes
	void SetBox(UUserWidget* Window, const TCHAR* Name, double Value)
	{
		UUserWidget* Box = Window ? Cast<UUserWidget>(Window->GetWidgetFromName(Name)) : nullptr;
		UEditableTextBox* Text = Box ? Cast<UEditableTextBox>(Box->GetWidgetFromName(TEXT("TextBox"))) : nullptr;
		if (Text)
		{
			Text->SetText(FText::FromString(FString::SanitizeFloat(Value, 0)));
		}
		else
		{
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST no number box %s in %s"), Name, *GetNameSafe(Window));
		}
	}

	void SetCost(UUserWidget* Window, bool bUseCost)
	{
		if (UCheckBox* Box = Window ? Cast<UCheckBox>(Window->GetWidgetFromName(TEXT("UseBuildCost"))) : nullptr)
		{
			Box->SetIsChecked(bUseCost);
		}
	}

	// The Copy window: move (m) and turn (degrees) typed in, "Use build cost" set, then Button
	void AddCopyWindow(FRun& R, const FVector& DeltaMetres, double Yaw, bool bUseCost, const TCHAR* Button = TEXT("OkButton"))
	{
		AddOnWindow(R, TEXT("Widget_Copy"), [=](FRun& Self, UUserWidget* Window)
		{
			SetBox(Window, TEXT("XNumberBox"), DeltaMetres.X);
			SetBox(Window, TEXT("YNumberBox"), DeltaMetres.Y);
			SetBox(Window, TEXT("ZNumberBox"), DeltaMetres.Z);
			SetBox(Window, TEXT("RotationZ"), Yaw);
			SetCost(Window, bUseCost);
			Self.Check(PressButton(Window, Button), FString::Printf(TEXT("Copy: %s with %s m, %.0f degrees, cost %d"), Button, *DeltaMetres.ToCompactString(), Yaw, bUseCost));
		});
	}

	void AddFillWindow(FRun& R, const FVector& BorderMetres, const FIntVector& Amount, const FVector2D& RampMetres, bool bUseCost)
	{
		AddOnWindow(R, TEXT("Widget_Fill"), [=](FRun& Self, UUserWidget* Window)
		{
			SetBox(Window, TEXT("XBorder"), BorderMetres.X);
			SetBox(Window, TEXT("YBorder"), BorderMetres.Y);
			SetBox(Window, TEXT("ZBorder"), BorderMetres.Z);
			SetBox(Window, TEXT("XAmount"), Amount.X);
			SetBox(Window, TEXT("YAmount"), Amount.Y);
			SetBox(Window, TEXT("ZAmount"), Amount.Z);
			SetBox(Window, TEXT("XRamp"), RampMetres.X);
			SetBox(Window, TEXT("YRamp"), RampMetres.Y);
			SetCost(Window, bUseCost);
			Self.Check(PressButton(Window, TEXT("OkButton")), FString::Printf(TEXT("Fill: OK with gap %s m, amount %s, ramp %s m, cost %d"), *BorderMetres.ToCompactString(), *Amount.ToString(),
				*RampMetres.ToString(), bUseCost));
		});
	}

	void AddCheckMessage(FRun& R, const FSceneRef& S, const FString& Want)
	{
		R.Steps.Add([S, Want](FRun& Self)
		{
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			return EStep::Next;
		});
	}

	// The cost of one copy of the pattern, from the game's recipes (a beam's times its length)
	AreaActionsBuildings::FItemCounts PatternCost(const FRun& R, const FCopyState& C)
	{
		AreaActionsBuildings::FItemCounts Cost;
		for (const FPiece& Piece : C.Pattern)
		{
			if (Piece.bIgnored || !Piece.BuiltWith)
			{
				continue;
			}
			int32 Times = 1;
			if (Piece.BeamLength > 0.0f)
			{
				Times = FMath::Max(1, Piece.Class->GetDefaultObject<AFGBuildableBeam>()->GetDismantleReturnsMultiplierForBeam(Piece.BeamLength));
			}
			for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(R.GetWorld(), Piece.BuiltWith))
			{
				Cost.FindOrAdd(Ingredient.ItemClass) += Ingredient.Amount * Times;
			}
		}
		return Cost;
	}

	FString ItemsText(const AreaActionsBuildings::FItemCounts& Counts)
	{
		TArray<FString> Parts;
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Counts)
		{
			Parts.Add(FString::Printf(TEXT("%s %d"), *GetNameSafe(Entry.Key), Entry.Value));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	// The depot keeps its contents in a private list; the test sets one item's amount there
	void SetDepot(UWorld* World, TSubclassOf<UFGItemDescriptor> ItemClass, int32 Amount)
	{
		AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
		FArrayProperty* Property = Depot ? FindFProperty<FArrayProperty>(Depot->GetClass(), TEXT("mStoredItems")) : nullptr;
		TArray<FItemAmount>* Items = Property ? Property->ContainerPtrToValuePtr<TArray<FItemAmount>>(Depot) : nullptr;
		if (!Items)
		{
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST no depot list"));
			return;
		}
		Items->RemoveAll([ItemClass](const FItemAmount& Entry) { return Entry.ItemClass == ItemClass; });
		if (Amount > 0)
		{
			Items->Emplace(ItemClass, Amount);
		}
	}

	int32 DepotCount(UWorld* World, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		const AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
		return Depot ? Depot->GetNumItemsFromCentralStorage(ItemClass) : 0;
	}

	UFGInventoryComponent* PlayerInventory(const FRun& R)
	{
		AFGCharacterPlayer* Character = GetCharacter(R);
		return Character ? Character->GetInventory() : nullptr;
	}

	int32 StorageCount(const TWeakObjectPtr<AFGBuildableStorage>& Box, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		return Box.IsValid() ? Box->GetStorageInventory()->GetNumItems(ItemClass) : 0;
	}

	// The cost's sources, per item: Own in the player's inventory (their own items of the kind go first), Near in a container 15 m behind
	// the player, Far in one 600 m away, Depot in the depot; each a function of the item's cost
	void SetCostSources(FRun& R, FCopyState& C, TFunction<int32(int32)> Own, TFunction<int32(int32)> Near, TFunction<int32(int32)> Far, TFunction<int32(int32)> Depot)
	{
		UWorld* World = R.GetWorld();
		C.Cost = PatternCost(R, C);
		UFGInventoryComponent* Inventory = PlayerInventory(R);
		const APawn* Pawn = R.GetPlayer()->GetPawn();
		if (!C.NearBox.IsValid())
		{
			C.NearBox = SpawnStorage(R, Ground(R, Pawn->GetActorLocation() - Pawn->GetActorForwardVector() * 1500.0), 0, 0);
			C.FarBox = SpawnStorage(R, Ground(R, Pawn->GetActorLocation() + FVector(0, 60000, 0)), 0, 0);
		}
		C.Inventory.Reset();
		C.Near.Reset();
		C.Far.Reset();
		C.Depot.Reset();
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C.Cost)
		{
			auto Fill = [&Entry](UFGInventoryComponent* Into, int32 Amount)
			{
				if (Into)
				{
					Into->Remove(Entry.Key, Into->GetNumItems(Entry.Key));
					if (Amount > 0)
					{
						Into->AddStack(FInventoryStack(Amount, Entry.Key), true);
					}
				}
			};
			Fill(Inventory, Own(Entry.Value));
			Fill(C.NearBox.IsValid() ? C.NearBox->GetStorageInventory() : nullptr, Near(Entry.Value));
			Fill(C.FarBox.IsValid() ? C.FarBox->GetStorageInventory() : nullptr, Far(Entry.Value));
			SetDepot(World, Entry.Key, Depot(Entry.Value));
			C.Inventory.Add(Entry.Key, Inventory ? Inventory->GetNumItems(Entry.Key) : 0);
			C.Near.Add(Entry.Key, StorageCount(C.NearBox, Entry.Key));
			C.Far.Add(Entry.Key, StorageCount(C.FarBox, Entry.Key));
			C.Depot.Add(Entry.Key, DepotCount(World, Entry.Key));
		}
		// The containers are buildings too
		CountWorld(R, C);
		R.Step(FString::Printf(TEXT("cost %s; inventory %s; near %s; far %s; depot %s"), *ItemsText(C.Cost), *ItemsText(C.Inventory), *ItemsText(C.Near), *ItemsText(C.Far), *ItemsText(C.Depot)));
	}

	int32 CostNone(int32) { return 0; }
	int32 CostOne(int32) { return 1; }
	int32 CostAllButOne(int32 Cost) { return Cost - 1; }
	int32 CostAll(int32 Cost) { return Cost; }

	// Each source lost exactly what Taken says (Own, Near, Depot), the far container nothing
	void CheckSources(FRun& R, const FCopyState& C, TFunction<int32(int32)> FromOwn, TFunction<int32(int32)> FromNear, TFunction<int32(int32)> FromDepot, const TCHAR* When)
	{
		UFGInventoryComponent* Inventory = PlayerInventory(R);
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C.Cost)
		{
			const int32 Own = Inventory ? Inventory->GetNumItems(Entry.Key) : -1;
			const int32 Near = StorageCount(C.NearBox, Entry.Key);
			const int32 Far = StorageCount(C.FarBox, Entry.Key);
			const int32 Depot = DepotCount(R.GetWorld(), Entry.Key);
			const int32 WantOwn = C.Inventory.FindRef(Entry.Key) - FromOwn(Entry.Value);
			const int32 WantNear = C.Near.FindRef(Entry.Key) - FromNear(Entry.Value);
			const int32 WantDepot = C.Depot.FindRef(Entry.Key) - FromDepot(Entry.Value);
			R.Check(Own == WantOwn && Near == WantNear && Far == C.Far.FindRef(Entry.Key) && Depot == WantDepot,
				FString::Printf(TEXT("%s: %s inventory %d (want %d), near %d (want %d), far %d (want %d), depot %d (want %d)"), When, *GetNameSafe(Entry.Key), Own, WantOwn,
					Near, WantNear, Far, C.Far.FindRef(Entry.Key), Depot, WantDepot));
		}
	}

	void CheckNothingBuilt(FRun& R, const FCopyState& C, const TCHAR* When)
	{
		const int32 Actors = ActorCount(R.GetWorld()), Lightweights = LightweightCount(R.GetWorld());
		R.Check(Actors == C.ActorsBefore && Lightweights == C.LightweightsBefore,
			FString::Printf(TEXT("%s: actors %d (were %d), lightweights %d (were %d)"), When, Actors, C.ActorsBefore, Lightweights, C.LightweightsBefore));
	}

	// A copy scene: the pattern and the area; the world's counts after it
	void AddCopyScene(FRun& R, const FSceneRef& S, const FCopyRef& C, TFunction<void(FRun&, FScene&, FCopyState&)> Build)
	{
		AddScene(R, S, 4000.0, [C, Build](FRun& Self, FScene& Scene)
		{
			Build(Self, Scene, *C);
			C->Pivot = PlacesCenter(*C);
			Self.Step(FString::Printf(TEXT("pattern: %d pieces at %s, places centre %s"), C->Pattern.Num(), *C->Base.ToCompactString(), *C->Pivot.ToCompactString()));
		});
		R.Steps.Add([C](FRun& Self)
		{
			CountWorld(Self, *C);
			C->JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			return EStep::Next;
		});
	}

	// One Copy through the window and its result window
	void AddCopy(FRun& R, const FSceneRef& S, const FCopyRef& C, const FVector& DeltaMetres, double Yaw, bool bUseCost)
	{
		R.Steps.Add([C, DeltaMetres, Yaw](FRun& Self)
		{
			C->Delta = DeltaMetres * 100.0;
			C->Yaw = Yaw;
			return EStep::Next;
		});
		AddOpenAction(R, TEXT("Copy"));
		AddCopyWindow(R, DeltaMetres, Yaw, bUseCost);
		AddResultWindow(R, S);
	}

	void AddCopyBasic(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, false);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([S, C](FRun& Self)
		{
			CheckCopies(Self, *C, true, TEXT("copied"));
			CheckPattern(Self, *S, *C, TEXT("copied"));
			Self.Check(AAreaActionsServerSubsystem::FinishedJobs - C->JobsBefore == 1, TEXT("one AAJOB line"));
			return EStep::Next;
		});
	}

	void AddCopyRotate(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		// Round the middle of the area
		AddCopy(R, S, C, FVector(0, 100, 0), 90.0, false);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self) { CheckCopies(Self, *C, true, TEXT("turned round the middle")); return EStep::Next; });
		// Round a centre picked on the Constructor
		R.Steps.Add([C](FRun& Self) { C->Delta = FVector(0, -10000, 0); C->Yaw = 90.0; return EStep::Next; });
		AddOpenAction(R, TEXT("Copy"));
		AddPress(R, TEXT("Widget_Copy"), TEXT("SetRotationCenter"));
		R.Steps.Add([C](FRun& Self)
		{
			if (const FPiece* Machine = C->Pattern.FindByPredicate([](const FPiece& Piece) { return Piece.Class->IsChildOf(AFGBuildableManufacturer::StaticClass()); }))
			{
				LookAt(Self, Machine->Transform.GetLocation() + FVector(0, 0, 300));
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([](FRun& Self)
		{
			FHitResult Hit;
			const bool bHit = AimHit(Self, Hit);
			Self.Check(bHit && Cast<AFGBuildableManufacturer>(HitOwner(Self.GetWorld(), Hit)), FString::Printf(TEXT("aim hits the Constructor (%s)"), bHit ? *GetNameSafe(Hit.GetActor()) : TEXT("nothing")));
			Self.Step(TEXT("left click picks the rotation centre"));
			PressKey(Self, EKeys::LeftMouseButton);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([C](FRun& Self)
		{
			UClass* MarkerClass = LoadClass<AActor>(nullptr, TEXT("/AreaActions/Actions/Copy/CopyRotationCenter.CopyRotationCenter_C"));
			AActor* Marker = MarkerClass ? UGameplayStatics::GetActorOfClass(Self.GetWorld(), MarkerClass) : nullptr;
			if (Self.Check(Marker != nullptr, FString::Printf(TEXT("rotation centre marker %s"), Marker ? *Marker->GetActorLocation().ToCompactString() : TEXT("none"))))
			{
				C->Pivot = FVector(Marker->GetActorLocation().X, Marker->GetActorLocation().Y, 0.0);
			}
			return EStep::Next;
		});
		AddCopyWindow(R, FVector(0, -100, 0), 90.0, false);
		AddResultWindow(R, S);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self) { CheckCopies(Self, *C, true, TEXT("turned round the picked centre")); return EStep::Next; });
	}

	// The copies land outside the area (80 m wide), so the second request finds only the pattern again
	void AddCopyTwice(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		R.Steps.Add([C](FRun& Self) { SetCostSources(Self, *C, [](int32 Cost) { return 2 * Cost; }, CostNone, CostNone, CostNone); return EStep::Next; });
		AddCopy(R, S, C, FVector(100, 0, 0), 0.0, true);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self)
		{
			CheckSources(Self, *C, CostAll, CostNone, CostNone, TEXT("first copy"));
			for (TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C->Inventory)
			{
				Entry.Value -= C->Cost.FindRef(Entry.Key);
			}
			CountWorld(Self, *C);
			return EStep::Next;
		});
		AddCopy(R, S, C, FVector(100, 0, 0), 0.0, true);
		AddCheckMessage(R, S, TEXT("Copied 0 buildings."));
		R.Steps.Add([C](FRun& Self)
		{
			CheckSources(Self, *C, CostNone, CostNone, CostNone, TEXT("second copy"));
			CheckNothingBuilt(Self, *C, TEXT("second copy"));
			return EStep::Next;
		});
	}

	void AddCopySaveLoad(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, false);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self) { CheckCopies(Self, *C, true, TEXT("copied")); return EStep::Next; });
		AddSaveAndLoad(R, TEXT("AATest_CopySaveLoad"));
		R.Steps.Add([C](FRun& Self) { CheckCopies(Self, *C, true, TEXT("loaded")); return EStep::Next; });
		AddDeleteSave(R, TEXT("AATest_CopySaveLoad"));
	}

	// One of each item in the inventory, the rest in the container in range; the one out of range holds as much as the cost
	void AddCopyCost(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		R.Steps.Add([C](FRun& Self) { SetCostSources(Self, *C, CostOne, CostAllButOne, CostAll, CostNone); return EStep::Next; });
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, true);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self)
		{
			CheckSources(Self, *C, CostOne, CostAllButOne, CostNone, TEXT("paid"));
			CheckCopies(Self, *C, true, TEXT("paid"));
			return EStep::Next;
		});
	}

	void AddCopyCostMissing(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		R.Steps.Add([C](FRun& Self) { SetCostSources(Self, *C, CostOne, CostNone, CostAll, CostNone); return EStep::Next; });
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, true);
		R.Steps.Add([S, C](FRun& Self)
		{
			Self.Check(S->Message.StartsWith(TEXT("Missing: ")), FString::Printf(TEXT("result '%s' lists what is missing"), *S->Message));
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C->Cost)
			{
				const FString Want = FString::Printf(TEXT("%d %s"), Entry.Value - 1, *UFGItemDescriptor::GetItemName(Entry.Key).ToString());
				Self.Check(Entry.Value == 1 ? !S->Message.Contains(UFGItemDescriptor::GetItemName(Entry.Key).ToString()) : S->Message.Contains(Want),
					FString::Printf(TEXT("missing has '%s'"), *Want));
			}
			CheckSources(Self, *C, CostNone, CostNone, CostNone, TEXT("nothing taken"));
			CheckNothingBuilt(Self, *C, TEXT("nothing built"));
			return EStep::Next;
		});
	}

	void AddCopyNoCost(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		R.Steps.Add([C](FRun& Self)
		{
			SetCostSources(Self, *C, CostOne, CostNone, CostNone, CostNone);
			AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>();
			if (State)
			{
				State->SetCheatNoCost(true);
			}
			Self.Check(State && PlayerInventory(Self)->GetNoBuildCost(), TEXT("No Build Cost on"));
			return EStep::Next;
		});
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, true);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self)
		{
			CheckCopies(Self, *C, true, TEXT("no cost"));
			CheckSources(Self, *C, CostNone, CostNone, CostNone, TEXT("no cost"));
			if (AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>())
			{
				State->SetCheatNoCost(false);
			}
			return EStep::Next;
		});
	}

	// AA-071: one of each item in the inventory, the rest only in the depot; first the depot lacks one of the first item (only that one
	// is missing), then it has enough
	void AddCopyCostDepot(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		TSharedRef<TSubclassOf<UFGItemDescriptor>> Short = MakeShared<TSubclassOf<UFGItemDescriptor>>();
		R.Steps.Add([C, Short](FRun& Self)
		{
			SetCostSources(Self, *C, CostOne, CostNone, CostNone, CostAllButOne);
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C->Cost)
			{
				if (!*Short && Entry.Value > 1)
				{
					*Short = Entry.Key;
					SetDepot(Self.GetWorld(), Entry.Key, Entry.Value - 2);
					C->Depot[Entry.Key] = DepotCount(Self.GetWorld(), Entry.Key);
				}
			}
			Self.Step(FString::Printf(TEXT("depot short of one %s"), *GetNameSafe(Short->Get())));
			return EStep::Next;
		});
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, true);
		R.Steps.Add([S, C, Short](FRun& Self)
		{
			const FString Want = FString::Printf(TEXT("Missing: 1 %s"), *UFGItemDescriptor::GetItemName(*Short).ToString());
			Self.Check(S->Message == Want, FString::Printf(TEXT("result '%s' is '%s'"), *S->Message, *Want));
			CheckSources(Self, *C, CostNone, CostNone, CostNone, TEXT("short"));
			CheckNothingBuilt(Self, *C, TEXT("short"));
			return EStep::Next;
		});
		R.Steps.Add([C](FRun& Self) { SetCostSources(Self, *C, CostOne, CostNone, CostNone, CostAllButOne); return EStep::Next; });
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, true);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		R.Steps.Add([C](FRun& Self)
		{
			CheckSources(Self, *C, CostOne, CostNone, CostAllButOne, TEXT("paid from the depot"));
			CheckCopies(Self, *C, true, TEXT("paid from the depot"));
			return EStep::Next;
		});
	}

	// Fill's copies of the single foundation: Steps along X and Y, the given pitch, Z per step along X
	void CheckGrid(FRun& R, const FCopyState& C, const TArray<FIntPoint>& Steps, double Pitch, double RampX, const TCHAR* When)
	{
		const FPiece& Foundation = C.Pattern[0];
		int32 Found = 0;
		for (const FIntPoint& Step : Steps)
		{
			const FVector Want = Foundation.Transform.GetLocation() + FVector(Step.X * Pitch, Step.Y * Pitch, FMath::Abs(Step.X) * RampX);
			const bool bHere = FindLightweight(R.GetWorld(), Foundation.Class, Want) != nullptr;
			Found += bHere ? 1 : 0;
			if (!bHere)
			{
				R.Check(false, FString::Printf(TEXT("%s: no foundation at step %s, %s"), When, *Step.ToString(), *Want.ToCompactString()));
			}
		}
		R.Check(Found == Steps.Num() && LightweightCount(R.GetWorld()) == C.LightweightsBefore + Steps.Num(),
			FString::Printf(TEXT("%s: foundations in the grid %d of %d, lightweights %d (want %d)"), When, Found, Steps.Num(), LightweightCount(R.GetWorld()), C.LightweightsBefore + Steps.Num()));
	}

	// Amount (3, 2, 1) with a gap of 1 m and a drop of 1 m per step along X; negative: (-3, -2, 1) in the test place's negative X and Y
	void AddFillGrid(FRun& R, bool bNegative)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildSingle);
		R.Steps.Add([C, bNegative](FRun& Self)
		{
			Self.Check(!bNegative || (C->Base.X < 0.0 && C->Base.Y < 0.0), FString::Printf(TEXT("pattern at %s"), *C->Base.ToCompactString()));
			return EStep::Next;
		});
		AddOpenAction(R, TEXT("Fill"));
		const double Ramp = bNegative ? 0.0 : -1.0;
		AddFillWindow(R, FVector(1, 1, 0), bNegative ? FIntVector(-3, -2, 1) : FIntVector(3, 2, 1), FVector2D(Ramp, 0), false);
		AddResultWindow(R, S);
		AddCheckMessage(R, S, TEXT("Filled with 5 buildings."));
		R.Steps.Add([C, bNegative, Ramp](FRun& Self)
		{
			TArray<FIntPoint> Steps;
			for (int32 X = 0; X < 3; ++X)
			{
				for (int32 Y = 0; Y < 2; ++Y)
				{
					if (X || Y)
					{
						Steps.Add(bNegative ? FIntPoint(-X, -Y) : FIntPoint(X, Y));
					}
				}
			}
			// The foundation is 8 m: the step is 8 m and the gap
			CheckGrid(Self, *C, Steps, 900.0, Ramp * 100.0, bNegative ? TEXT("negative fill") : TEXT("fill"));
			return EStep::Next;
		});
	}

	// The game's holograms the preview spawned and has not destroyed
	int32 PreviewHolograms(FRun& Self)
	{
		int32 Holograms = 0;
		for (TActorIterator<AFGHologram> It(Self.GetWorld()); It; ++It)
		{
			Holograms += It->GetOwner() && It->GetOwner()->IsA<AAreaActionsPreview>() ? 1 : 0;
		}
		return Holograms;
	}

	void AddCopyPreview(FRun& R, const FCopyRef& C)
	{
		R.Steps.Add([C](FRun& Self) { C->Delta = FVector(2000, 0, 0); C->Yaw = 0.0; return EStep::Next; });
		AddOpenAction(R, TEXT("Copy"));
		AddCopyWindow(R, FVector(20, 0, 0), 0.0, false, TEXT("PreviewButton"));
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([C](FRun& Self)
		{
			Self.Check(AAreaActionsPreview::LiveGhosts == 9, FString::Printf(TEXT("ghosts %d (want 9)"), AAreaActionsPreview::LiveGhosts));
			// D20: the game's own holograms, except the beam (its length)
			int32 Holograms = 0;
			// The build gun's colour comes from the custom depth stencil: each hologram's building meshes have the valid one (its guidelines,
			// ports and clearance keep their own, as in the build gun), and so has every instanced mesh of the preview
			const uint8 Stencil = AAreaActionsPreview::GetValidStencil();
			auto WithStencil = [Stencil](const AActor* Actor, int32& Meshes)
			{
				TInlineComponentArray<UMeshComponent*> Components(Actor);
				Meshes = Components.Num();
				return Algo::CountIf(Components, [Stencil](const UMeshComponent* Mesh) { return Mesh->bRenderCustomDepth && Mesh->CustomDepthStencilValue == Stencil; });
			};
			int32 Uncoloured = 0, Instanced = 0, InstancedColoured = 0;
			for (TActorIterator<AFGHologram> It(Self.GetWorld()); It; ++It)
			{
				if (It->GetOwner() && It->GetOwner()->IsA<AAreaActionsPreview>())
				{
					int32 Meshes = 0;
					++Holograms;
					Uncoloured += WithStencil(*It, Meshes) > 0 ? 0 : 1;
				}
			}
			for (TActorIterator<AAreaActionsPreview> It(Self.GetWorld()); It; ++It)
			{
				int32 Meshes = 0;
				InstancedColoured += WithStencil(*It, Meshes);
				Instanced += Meshes;
			}
			Self.Check(Holograms == 8, FString::Printf(TEXT("game holograms in the preview %d (want 8)"), Holograms));
			Self.Check(Stencil != 0 && Uncoloured == 0 && Instanced > 0 && InstancedColoured == Instanced, FString::Printf(
				TEXT("ghost stencil %d: holograms without it %d, instanced meshes with it %d of %d"), Stencil, Uncoloured, InstancedColoured, Instanced));
			CheckNothingBuilt(Self, *C, TEXT("preview"));
			return EStep::Next;
		});
	}

	// D7: the preview shows ghosts, not buildings; Cancel and the tool put away leave none; a save made meanwhile has no copies
	void AddPreview(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		AddCopyPreview(R, C);
		AddOnWindow(R, TEXT("Widget_Copy"), [](FRun& Self, UUserWidget* Window)
		{
			UUserWidget* Frame = Cast<UUserWidget>(Window->GetWidgetFromName(TEXT("WindowDark")));
			FMulticastDelegateProperty* Property = Frame ? FindFProperty<FMulticastDelegateProperty>(Frame->GetClass(), TEXT("OnClose")) : nullptr;
			if (Property)
			{
				Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Frame)->ProcessMulticastDelegate<UObject>(nullptr);
			}
			Self.Check(Property != nullptr, TEXT("Copy window closed with X"));
		});
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(AAreaActionsPreview::LiveGhosts == 0 && PreviewHolograms(Self) == 0 && !ActiveAction(Self), FString::Printf(TEXT("after Cancel: ghosts %d, holograms %d, action %s"),
				AAreaActionsPreview::LiveGhosts, PreviewHolograms(Self), *GetNameSafe(ActiveAction(Self))));
			return EStep::Next;
		});
		AddCopyPreview(R, C);
		R.Steps.Add([](FRun& Self)
		{
			AFGCharacterPlayer* Character = GetCharacter(Self);
			if (AAreaActionsEquipment* Tool = GetTool(Self); Character && Tool)
			{
				Character->UnequipEquipment(Tool);
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(AAreaActionsPreview::LiveGhosts == 0 && PreviewHolograms(Self) == 0 && !GetTool(Self), FString::Printf(TEXT("tool put away: ghosts %d, holograms %d, tool %s"),
				AAreaActionsPreview::LiveGhosts, PreviewHolograms(Self), *GetNameSafe(GetTool(Self))));
			return EStep::Next;
		});
		AddEquipTool(R);
		R.Steps.Add([S](FRun& Self) { Self.Check(SetToolArea(Self, *S), TEXT("tool area set again")); return EStep::Next; });
		AddCopyPreview(R, C);
		AddSaveAndLoad(R, TEXT("AATest_Preview"));
		R.Steps.Add([C](FRun& Self)
		{
			CheckCopies(Self, *C, false, TEXT("loaded after a preview"));
			Self.Check(AAreaActionsPreview::LiveGhosts == 0, FString::Printf(TEXT("ghosts after the load %d"), AAreaActionsPreview::LiveGhosts));
			return EStep::Next;
		});
		AddDeleteSave(R, TEXT("AATest_Preview"));
	}

	// The building selection on a foundation (lightweight): its box
	void AddSelectLightweight(FRun& R)
	{
		AddEquipTool(R);
		TSharedRef<FTransform> Placed = MakeShared<FTransform>();
		R.Steps.Add([Placed](FRun& Self)
		{
			const APawn* Pawn = Self.GetPlayer()->GetPawn();
			FCopyState Built;
			*Placed = FTransform(FRotator(0, 23, 0), Ground(Self, Pawn->GetActorLocation() + Pawn->GetActorForwardVector() * 2000.0));
			AddLightweight(Self, Built, TEXT("Build_Foundation_8x1_01"), TEXT("Recipe_Foundation_8x1_01"), *Placed);
			Self.Check(Built.Pattern.Num() == 1, FString::Printf(TEXT("foundation built at %s"), *Placed->GetLocation().ToCompactString()));
			Self.Check(SelectState(GetTool(Self), TEXT("NewEnumerator14")), TEXT("building selection mode set"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		R.Steps.Add([Placed](FRun& Self) { LookAt(Self, Placed->GetLocation() + FVector(0, 0, 50)); return EStep::Next; });
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([](FRun& Self)
		{
			FHitResult Hit;
			const bool bHit = AimHit(Self, Hit);
			AreaActionsBuildings::FSnapshot Target;
			Self.Check(bHit && AreaActionsBuildings::FromHit(Self.GetWorld(), Hit, Target) && Target.Class == GameClass(TEXT("Build_Foundation_8x1_01")),
				FString::Printf(TEXT("aim hits the foundation (%s)"), bHit ? *GetNameSafe(Hit.GetActor()) : TEXT("nothing")));
			PressKey(Self, EKeys::LeftMouseButton);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([Placed](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			const FBox Box = GameClass(TEXT("Build_Foundation_8x1_01"))->GetDefaultObject<AFGBuildable>()->GetCombinedClearanceBox();
			double WantMinZ = UE_BIG_NUMBER, WantMaxZ = -UE_BIG_NUMBER;
			TArray<FVector> Want;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const FVector Corner = Placed->TransformPosition(FVector(Index & 1 ? Box.Max.X : Box.Min.X, Index & 2 ? Box.Max.Y : Box.Min.Y, Index & 4 ? Box.Max.Z : Box.Min.Z));
				WantMinZ = FMath::Min(WantMinZ, Corner.Z);
				WantMaxZ = FMath::Max(WantMaxZ, Corner.Z);
				if (Index < 4)
				{
					Want.Add(Corner);
				}
			}
			const TArray<FVector> Corners = VectorArray(Tool, TEXT("CornerLocations"));
			int32 Matched = 0;
			for (const FVector& Corner : Want)
			{
				Matched += Corners.ContainsByPredicate([&Corner](const FVector& Got) { return FVector::Dist2D(Corner, Got) <= 1.0; }) ? 1 : 0;
			}
			Self.Check(Box.IsValid && Corners.Num() == 4 && Matched == 4, FString::Printf(TEXT("corners %d, matching the foundation's box within 1 cm: %d"), Corners.Num(), Matched));
			const double MinZ = DoubleValue(Tool, TEXT("MinZ")), MaxZ = DoubleValue(Tool, TEXT("MaxZ"));
			Self.Check(FMath::Abs(MinZ - WantMinZ) <= 1.0 && FMath::Abs(MaxZ - WantMaxZ) <= 1.0, FString::Printf(TEXT("MinZ %.1f (box %.1f), MaxZ %.1f (box %.1f)"), MinZ, WantMinZ, MaxZ, WantMaxZ));
			return EStep::Next;
		});
	}

	// W5 on a Fill of 5049 foundations, then a request over AreaActions.MaxSpawnPerRequest refused before anything changes
	void AddFillLarge(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildSingle);
		R.Steps.Add([](FRun& Self)
		{
			Self.MaxFrameMs = 0.0;
			Self.bMeasureFrames = true;
			Self.StepTimeoutSec = 180.0;
			return EStep::Next;
		});
		AddOpenAction(R, TEXT("Fill"));
		AddFillWindow(R, FVector(1, 1, 0), FIntVector(101, 50, 1), FVector2D(0, 0), false);
		AddResultWindow(R, S);
		AddCheckMessage(R, S, TEXT("Filled with 5,049 buildings."));
		R.Steps.Add([C](FRun& Self)
		{
			Self.bMeasureFrames = false;
			Self.Check(Self.MaxFrameMs <= 200.0, FString::Printf(TEXT("longest frame during the job: %.1f ms (W5: 200)"), Self.MaxFrameMs));
			Self.Check(LightweightCount(Self.GetWorld()) == C->LightweightsBefore + 5049, FString::Printf(TEXT("lightweights %d (want %d)"), LightweightCount(Self.GetWorld()), C->LightweightsBefore + 5049));
			CountWorld(Self, *C);
			return EStep::Next;
		});
		AddOpenAction(R, TEXT("Fill"));
		AddFillWindow(R, FVector(1, 1, 0), FIntVector(200, 101, 1), FVector2D(0, 0), false);
		AddResultWindow(R, S);
		AddCheckMessage(R, S, TEXT("at most 10,000 at once"));
		R.Steps.Add([C](FRun& Self)
		{
			CheckNothingBuilt(Self, *C, TEXT("refused request"));
			Self.StepTimeoutSec = 30.0;
			return EStep::Next;
		});
	}

	// --- Action windows (AA-081) and the tool's menu (AA-075) ---

	// Counts the Error lines of the mod's C++ category while it lives
	struct FErrorCounter : public FOutputDevice
	{
		int32 Errors = 0;
		FErrorCounter() { GLog->AddOutputDevice(this); }
		virtual ~FErrorCounter() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			Errors += Category == LogAreaActionsCpp.GetCategoryName() && Verbosity == ELogVerbosity::Error ? 1 : 0;
		}
	};

	bool ClickRecipe(UUserWidget* Window, const TCHAR* RecipePath)
	{
		const UClass* Want = Recipe(RecipePath);
		return ClickListItem(Window ? Cast<UListView>(Window->GetWidgetFromName(TEXT("RecipeList"))) : nullptr, [Want](UObject* Entry)
		{
			return ClassValue(Entry, TEXT("Recipe")) == Want;
		});
	}

	// The same button twice, in one frame (GapFrames 0) or GapFrames apart: one request, one job, a result with success
	void AddDoubleClick(FRun& R, bool bRecipe, int32 GapFrames, double Side)
	{
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<int32> JobsBefore = MakeShared<int32>(0);
		TSharedRef<TWeakObjectPtr<UUserWidget>> Clicked = MakeShared<TWeakObjectPtr<UUserWidget>>();
		AddScene(R, S, 4000.0, [bRecipe](FRun& Self, FScene& Scene)
		{
			MakeAvailable(Self, Asset::IronRod);
			for (int32 Index = 0; Index < 2; ++Index)
			{
				Scene.Inside.Add(bRecipe ? static_cast<AFGBuildable*>(SpawnMachine(Self, Asset::Constructor, Scene.Slot(Index), Asset::IronPlate, 0, 0))
					: static_cast<AFGBuildable*>(SpawnStorage(Self, Scene.Slot(Index), 10, 0)));
			}
			for (const FVector& Place : Scene.ControlPlaces())
			{
				Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
		}, Side);
		AddOpenAction(R, bRecipe ? TEXT("SetRecipe") : TEXT("ClearInventories"));
		const TCHAR* WindowName = bRecipe ? TEXT("Widget_SetRecipe") : TEXT("Widget_MessageYesNo");
		auto Click = [bRecipe](FRun& Self, UUserWidget* Window, int32 Nth)
		{
			const bool bDone = bRecipe ? ClickRecipe(Window, Asset::IronRod) : PressButton(Window, TEXT("YesButton"));
			Self.Check(bDone, FString::Printf(TEXT("click %d on %s"), Nth, bRecipe ? TEXT("Iron Rod") : TEXT("Delete items")));
		};
		R.Steps.Add([=](FRun& Self)
		{
			UUserWidget* Window = TopWindow(Self);
			if (!IsWindow(Window, WindowName) || !Window->IsInViewport())
			{
				return EStep::Wait;
			}
			*JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			*Clicked = Window;
			Click(Self, Window, 1);
			if (GapFrames == 0)
			{
				Click(Self, Window, 2);
			}
			return EStep::Next;
		});
		if (GapFrames > 0)
		{
			R.Steps.Add(WaitFrames(GapFrames));
			R.Steps.Add([=](FRun& Self)
			{
				Click(Self, Clicked->Get(), 2);
				return EStep::Next;
			});
		}
		AddResultWindow(R, S);
		R.Steps.Add([S, JobsBefore, bRecipe, GapFrames](FRun& Self)
		{
			const int32 Jobs = AAreaActionsServerSubsystem::FinishedJobs - *JobsBefore;
			Self.Check(Jobs == 1 && S->Results.Num() == 1, FString::Printf(TEXT("%s, %d frames apart: jobs %d, results %d (want 1, 1)"),
				bRecipe ? TEXT("recipe") : TEXT("Delete items"), GapFrames, Jobs, S->Results.Num()));
			const TCHAR* Success = bRecipe ? TEXT("Set recipe to") : TEXT("Cleared 2 buildings");
			Self.Check(S->Message.Contains(Success) && !S->Message.Contains(TEXT("Another area action")), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, Success));
			return EStep::Next;
		});
	}

	// A handler whose parameters differ from the dispatcher's is refused with an error; a matching one binds
	void AddBindSignature(FRun& R)
	{
		R.Steps.Add([](FRun& Self)
		{
			const UAreaActionsWorldModule* Module = UAreaActionsWorldModule::Get(Self.GetWorld());
			// The message window of the first action that names one
			TSubclassOf<UUserWidget> MessageOk;
			for (const TSubclassOf<AAreaActionBase>& Action : Module ? Module->AvailableActions : TArray<TSubclassOf<AAreaActionBase>>())
			{
				MessageOk = MessageOk ? MessageOk : Action->GetDefaultObject<AAreaActionBase>()->MessageOkClass;
			}
			UUserWidget* Window = MessageOk ? CreateWidget<UUserWidget>(Self.GetPlayer(), MessageOk) : nullptr;
			if (!Self.Check(Window != nullptr, FString::Printf(TEXT("message window %s"), *GetNameSafe(Window))))
			{
				return EStep::Next;
			}
			UAreaActionSetRecipe* Target = NewObject<UAreaActionSetRecipe>(GetTransientPackage());
			FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(Window->GetClass(), TEXT("OkClicked"));
			const FMulticastScriptDelegate* Bound = Property ? Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Window) : nullptr;
			int32 Errors = 0;
			bool bWrong = true;
			{
				FErrorCounter Counter;
				// OkClicked() sends nothing; Selected takes a recipe
				bWrong = AAreaActionBase::BindWindowEvent(Window, TEXT("OkClicked"), Target, GET_FUNCTION_NAME_CHECKED(UAreaActionSetRecipe, Selected));
				// The redirector hands lines to this device later, on a flush
				GLog->Flush();
				Errors = Counter.Errors;
			}
			Self.Check(!bWrong && Errors == 1, FString::Printf(TEXT("OkClicked to Selected(recipe): bound %d, error lines %d (want 0, 1)"), bWrong, Errors));
			Self.Check(Bound && !Bound->Contains(Target, GET_FUNCTION_NAME_CHECKED(UAreaActionSetRecipe, Selected)), TEXT("OkClicked has no Selected"));
			const bool bRight = AAreaActionBase::BindWindowEvent(Window, TEXT("OkClicked"), Target, GET_FUNCTION_NAME_CHECKED(UAreaActionSetRecipe, Canceled));
			Self.Check(bRight && Bound && Bound->Contains(Target, GET_FUNCTION_NAME_CHECKED(UAreaActionSetRecipe, Canceled)), TEXT("OkClicked to Canceled() binds"));
			return EStep::Next;
		});
	}

	// AA-075: the menu closed with X leaves the game's own input: no cursor, the viewport takes the mouse
	void AddToolMenuClose(FRun& R)
	{
		AddToolMenu(R);
		R.Steps.Add([](FRun& Self)
		{
			UUserWidget* Popup = Cast<UUserWidget>(ObjectValue(GetTool(Self), TEXT("PopupWidget")));
			UUserWidget* Frame = Popup ? Cast<UUserWidget>(Popup->GetWidgetFromName(TEXT("WindowDark"))) : nullptr;
			FMulticastDelegateProperty* Property = Frame ? FindFProperty<FMulticastDelegateProperty>(Frame->GetClass(), TEXT("OnClose")) : nullptr;
			if (Property)
			{
				Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Frame)->ProcessMulticastDelegate<UObject>(nullptr);
			}
			Self.Check(Property != nullptr, TEXT("menu closed with X"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			APlayerController* Player = Self.GetPlayer();
			UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
			const UUserWidget* Popup = Cast<UUserWidget>(ObjectValue(GetTool(Self), TEXT("PopupWidget")));
			Self.Check(!Popup || !Popup->IsInViewport(), TEXT("menu gone"));
			Self.Check(TopWindow(Self) == nullptr, TEXT("no window of the tool left"));
			Self.Check(Player && !Player->bShowMouseCursor, FString::Printf(TEXT("mouse cursor hidden: %d"), Player ? !Player->bShowMouseCursor : -1));
			const bool bGameInput = Viewport && !Viewport->IgnoreInput() && Viewport->GetMouseCaptureMode() == EMouseCaptureMode::CapturePermanently;
			Self.Check(bGameInput, FString::Printf(TEXT("game input: ignore %d, capture mode %d (want 0, %d)"), Viewport ? Viewport->IgnoreInput() : -1,
				Viewport ? static_cast<int32>(Viewport->GetMouseCaptureMode()) : -1, static_cast<int32>(EMouseCaptureMode::CapturePermanently)));
			return EStep::Next;
		});
	}

	// --- MassDismantle (AA-030), the return crates (AA-078, AA-085) ---

	// "Desc_IronPlate_C 50, Desc_IronIngot_C 10", sorted, items with a state marked
	FString StacksText(const TArray<FInventoryStack>& Stacks)
	{
		TMap<FString, int32> Sum;
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems())
			{
				Sum.FindOrAdd(GetNameSafe(Stack.Item.GetItemClass()) + (Stack.Item.HasState() ? TEXT("(state)") : TEXT(""))) += Stack.NumItems;
			}
		}
		Sum.KeySort(TLess<FString>());
		TArray<FString> Parts;
		for (const TPair<FString, int32>& Entry : Sum)
		{
			Parts.Add(FString::Printf(TEXT("%s %d"), *Entry.Key, Entry.Value));
		}
		return Parts.IsEmpty() ? TEXT("nothing") : FString::Join(Parts, TEXT(", "));
	}

	int32 StacksCount(const TArray<FInventoryStack>& Stacks, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		int32 Count = 0;
		for (const FInventoryStack& Stack : Stacks)
		{
			Count += Stack.HasItems() && (!ItemClass || Stack.Item.GetItemClass() == ItemClass) ? Stack.NumItems : 0;
		}
		return Count;
	}

	// Every item in the actor's inventories, read straight from them
	TArray<FInventoryStack> ContentsOf(const AActor* Actor)
	{
		TArray<FInventoryStack> Out;
		TInlineComponentArray<UFGInventoryComponent*> Inventories(Actor);
		for (const UFGInventoryComponent* Inventory : Inventories)
		{
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks);
			Out.Append(Stacks);
		}
		return Out;
	}

	// A chainsaw with energy stored: an item with a state (FInventoryItem::HasState)
	FInventoryStack StateItem(float Energy)
	{
		FInventoryItem Saw(TSubclassOf<UFGItemDescriptor>(GameClass(TEXT("Desc_Chainsaw"))));
		FFGChainsawItemState State;
		State.EnergyStored = Energy;
		FFGDynamicStruct Dynamic;
		Dynamic.InitializeAs(State);
		Saw.SetItemState(Dynamic);
		FInventoryStack Stack(Saw);
		Stack.NumItems = 1;
		return Stack;
	}

	float StateEnergy(const FInventoryStack& Stack)
	{
		const FFGDynamicStruct& State = Stack.Item.GetItemState();
		return State.IsValid() && State.GetStruct() == FFGChainsawItemState::StaticStruct() ? static_cast<const FFGChainsawItemState*>(State.GetStructValueRaw())->EnergyStored : -1.0f;
	}

	// A belt of 6 m between two points, as BuildBasic's
	AFGBuildable* SpawnBelt(const FRun& R, const FVector& At)
	{
		UClass* Class = GameClass(TEXT("Build_ConveyorBeltMk1"));
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(R.GetWorld());
		const FTransform Transform(At);
		AFGBuildable* Belt = Class && Buildables ? Buildables->BeginSpawnBuildable(Class, Transform) : nullptr;
		if (!Belt)
		{
			return nullptr;
		}
		Belt->SetBuiltWithRecipe(RecipeOfClass(R, Class, TEXT("Recipe_ConveyorBeltMk1")));
		if (TArray<FSplinePointData>* Spline = Cast<AFGBuildableConveyorBelt>(Belt)->GetMutableSplinePointData())
		{
			Spline->Reset();
			Spline->Emplace(FVector::ZeroVector, FVector(0, 600, 0));
			Spline->Emplace(FVector(0, 600, 0), FVector(0, 600, 0));
		}
		Belt->FinishSpawning(Transform);
		return Belt;
	}

	AFGBuildable* SpawnGameBuildable(const FRun& R, const TCHAR* ClassName, const TCHAR* RecipeName, const FVector& At)
	{
		UClass* Class = GameClass(ClassName);
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(R.GetWorld());
		const FTransform Transform(At);
		AFGBuildable* Actor = Class && Buildables ? Buildables->BeginSpawnBuildable(Class, Transform) : nullptr;
		if (Actor)
		{
			Actor->SetBuiltWithRecipe(RecipeOfClass(R, Class, RecipeName));
			Actor->FinishSpawning(Transform);
		}
		return Actor;
	}

	// R8: the game's dismantle called from code: refunds, CanDismantle, Dismantle and the lightweight removal, at scale, and a crate that
	// grows after it was spawned; nothing comes back after a save and a load
	void AddDismantleProbe(FRun& R)
	{
		struct FProbe
		{
			TArray<TWeakObjectPtr<AActor>> Actors;
			TArray<FString> ClassPaths;
			TArray<FVector> Places;
			TWeakObjectPtr<AFGCrate> Crate;
			FVector Foundation = FVector::ZeroVector;
			double DismantledAt = 0.0;
			uint64 DismantledFrame = 0;
			TArray<TWeakObjectPtr<AActor>> Mass;
			TArray<FVector> MassPlaces;
			TArray<FVector> MassFoundations;
			FVector GrownCrate = FVector::ZeroVector;
			int32 GrownItems = 0;
		};
		TSharedRef<FProbe> P = MakeShared<FProbe>();
		const FString SaveName = TEXT("AATest_DismantleProbe");
		R.Steps.Add([P](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			const FVector Base = Ground(Self, Self.GetPlayer()->GetPawn()->GetActorLocation() + FVector(3000.0, 0.0, 0.0));
			AFGBuildableManufacturer* Machine = SpawnMachine(Self, Asset::Constructor, Base + FVector(0, -1500, 0), Asset::IronPlate, 10, 5);
			AFGBuildableStorage* Storage = SpawnStorage(Self, Base + FVector(0, 0, 0), 50, 0);
			if (Storage)
			{
				Storage->GetStorageInventory()->AddStack(StateItem(123.5f), true);
			}
			AFGBuildable* Belt = SpawnBelt(Self, Base + FVector(0, 1500, 100));
			for (AActor* Actor : TArray<AActor*>{ Machine, Storage, Belt })
			{
				P->Actors.Add(Actor);
				P->ClassPaths.Add(Actor ? Actor->GetClass()->GetPathName() : FString());
				P->Places.Add(Actor ? Actor->GetActorLocation() : FVector::ZeroVector);
			}
			FSpawnInventoryCrateParameters Parameters;
			Parameters.bAllowCrateMerging = false;
			P->Crate = AFGItemPickup_Spawnable::SpawnInventoryCrate(World, { FInventoryStack(25, Item(Asset::IronRodItem)) }, Base + FVector(1500, 0, 0), Parameters);
			AreaActionsBuildings::FSnapshot Foundation;
			Foundation.Class = GameClass(TEXT("Build_Foundation_8x1_01"));
			Foundation.BuiltWith = RecipeOfClass(Self, Foundation.Class, TEXT("Recipe_Foundation_8x1_01"));
			P->Foundation = Base + FVector(-1500, 0, 0);
			const bool bFoundation = Foundation.Class && AreaActionsBuildings::Spawn(World, Foundation, FTransform(P->Foundation));
			Self.Check(Machine && Storage && Belt && P->Crate.IsValid() && bFoundation, FString::Printf(TEXT("probe built: Constructor %d, container %d, belt %d, crate %d, foundation %d"),
				Machine != nullptr, Storage != nullptr, Belt != nullptr, P->Crate.IsValid(), bFoundation));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([P](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			for (const TWeakObjectPtr<AActor>& Weak : P->Actors)
			{
				AActor* Actor = Weak.Get();
				if (!Actor)
				{
					continue;
				}
				const bool bCan = IFGDismantleInterface::Execute_CanDismantle(Actor);
				TArray<FInventoryStack> Full, OnlyContents;
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Full, false);
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, OnlyContents, true);
				TArray<AActor*> Children;
				IFGDismantleInterface::Execute_GetChildDismantleActors(Actor, Children);
				const AFGBuildable* Buildable = Cast<AFGBuildable>(Actor);
				TArray<FInventoryStack> Ingredients;
				for (const FItemAmount& Each : UFGRecipe::GetIngredients(World, Buildable ? Buildable->GetBuiltWithRecipe() : nullptr))
				{
					Ingredients.Emplace(Each.Amount, Each.ItemClass);
				}
				const TArray<FInventoryStack> Contents = ContentsOf(Actor);
				Self.Step(FString::Printf(TEXT("R8 %s: CanDismantle %d, children %d; refund %s; refund without cost %s; contents %s; recipe %s %s"), *Actor->GetClass()->GetName(), bCan,
					Children.Num(), *StacksText(Full), *StacksText(OnlyContents), *StacksText(Contents), *GetNameSafe(Buildable ? Buildable->GetBuiltWithRecipe() : nullptr), *StacksText(Ingredients)));
				Self.Check(bCan, FString::Printf(TEXT("R8 %s can be dismantled"), *Actor->GetClass()->GetName()));
				// The refund without cost is the contents, the full one adds the recipe's ingredients (times the game's own multiplier)
				Self.Check(StacksText(OnlyContents) == StacksText(Contents), FString::Printf(TEXT("R8 %s refund without cost equals the contents"), *Actor->GetClass()->GetName()));
				for (const FInventoryStack& Ingredient : Ingredients)
				{
					const int32 Extra = StacksCount(Full, Ingredient.Item.GetItemClass()) - StacksCount(OnlyContents, Ingredient.Item.GetItemClass());
					Self.Check(Extra > 0 && Extra % Ingredient.NumItems == 0, FString::Printf(TEXT("R8 %s refund has %s x%d (%d per recipe)"), *Actor->GetClass()->GetName(),
						*GetNameSafe(Ingredient.Item.GetItemClass()), Extra, Ingredient.NumItems));
				}
				if (Actor->IsA<AFGBuildableStorage>())
				{
					const FInventoryStack* Saw = OnlyContents.FindByPredicate([](const FInventoryStack& Stack) { return Stack.Item.HasState(); });
					Self.Check(Saw && FMath::IsNearlyEqual(StateEnergy(*Saw), 123.5f), FString::Printf(TEXT("R8 refund keeps the chainsaw's state: energy %.1f"), Saw ? StateEnergy(*Saw) : -1.0f));
				}
			}
			int32 Index = INDEX_NONE;
			UClass* FoundationClass = GameClass(TEXT("Build_Foundation_8x1_01"));
			const FRuntimeBuildableInstanceData* Data = FindLightweight(World, FoundationClass, P->Foundation, &Index);
			TArray<FInventoryStack> Lightweight;
			if (Data)
			{
				FoundationClass->GetDefaultObject<AFGBuildable>()->GetLightweightBuildableDismantleRefundReturns(World, Data->BuiltWithRecipe, Data->TypeSpecificData, Lightweight);
			}
			Self.Check(Data && !Lightweight.IsEmpty(), FString::Printf(TEXT("R8 lightweight foundation refund from its class: %s"), *StacksText(Lightweight)));
			for (const TCHAR* Name : { TEXT("Build_TradingPost"), TEXT("Build_SpaceElevator") })
			{
				UClass* Class = GameClass(Name);
				const AFGBuildable* Default = Class ? Class->GetDefaultObject<AFGBuildable>() : nullptr;
				Self.Step(FString::Printf(TEXT("R8 %s class default CanDismantle %d"), Name, Default ? IFGDismantleInterface::Execute_CanDismantle(Default) : -1));
			}
			// The game's dismantle, then the lightweight's removal by its index
			const double Start = FPlatformTime::Seconds();
			for (const TWeakObjectPtr<AActor>& Weak : P->Actors)
			{
				if (AActor* Actor = Weak.Get())
				{
					IFGDismantleInterface::Execute_Dismantle(Actor);
				}
			}
			if (AFGCrate* Crate = P->Crate.Get())
			{
				Crate->Destroy();
			}
			if (Data)
			{
				AFGLightweightBuildableSubsystem::Get(World)->RemoveByInstanceIndex(FoundationClass, Index);
			}
			P->DismantledAt = FPlatformTime::Seconds();
			P->DismantledFrame = GFrameCounter;
			Self.Step(FString::Printf(TEXT("R8 dismantled 3 actors, destroyed the crate, removed the foundation in %.3f ms"), (P->DismantledAt - Start) * 1000.0));
			Self.Check(!FindLightweight(World, FoundationClass, P->Foundation), TEXT("R8 the foundation's instance is gone at once"));
			return EStep::Next;
		});
		R.Steps.Add([P](FRun& Self)
		{
			const int32 Left = Algo::CountIf(P->Actors, [](const TWeakObjectPtr<AActor>& Weak) { return IsValid(Weak.Get()); });
			if (Left > 0 && Self.Now() - P->DismantledAt < 20.0)
			{
				return EStep::Wait;
			}
			Self.Check(Left == 0 && !P->Crate.IsValid(), FString::Printf(TEXT("R8 dismantled actors gone after %.2f s, %llu frames (left %d, crate %d)"),
				Self.Now() - P->DismantledAt, GFrameCounter - P->DismantledFrame, Left, P->Crate.IsValid()));
			return EStep::Next;
		});
		// At scale: 500 containers with items and 2000 foundations, each set refunded and dismantled in one frame
		R.Steps.Add([P](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			const FVector Base = Ground(Self, Self.GetPlayer()->GetPawn()->GetActorLocation() + FVector(12000.0, -10000.0, 0.0));
			for (int32 Index = 0; Index < 500; ++Index)
			{
				const FVector At = Base + FVector((Index % 25) * 800.0, (Index / 25) * 800.0, 0.0);
				P->Mass.Add(SpawnStorage(Self, At, 10, 0));
				P->MassPlaces.Add(At);
			}
			AreaActionsBuildings::FSnapshot Foundation;
			Foundation.Class = GameClass(TEXT("Build_Foundation_8x1_01"));
			Foundation.BuiltWith = RecipeOfClass(Self, Foundation.Class, TEXT("Recipe_Foundation_8x1_01"));
			for (int32 Index = 0; Index < 2000; ++Index)
			{
				const FVector At = Base + FVector((Index % 50) * 800.0, 25000.0 + (Index / 50) * 800.0, 0.0);
				if (AreaActionsBuildings::Spawn(World, Foundation, FTransform(At)))
				{
					P->MassFoundations.Add(At);
				}
			}
			Self.Check(Algo::CountIf(P->Mass, [](const TWeakObjectPtr<AActor>& Weak) { return Weak.IsValid(); }) == 500 && P->MassFoundations.Num() == 2000,
				FString::Printf(TEXT("R8 scale: %d containers, %d foundations built"), P->Mass.Num(), P->MassFoundations.Num()));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([P](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			double Start = FPlatformTime::Seconds();
			int32 Items = 0;
			for (const TWeakObjectPtr<AActor>& Weak : P->Mass)
			{
				if (AActor* Actor = Weak.Get(); IsValid(Actor) && IFGDismantleInterface::Execute_CanDismantle(Actor))
				{
					TArray<FInventoryStack> Refund;
					IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Refund, false);
					Items += StacksCount(Refund, nullptr);
					IFGDismantleInterface::Execute_Dismantle(Actor);
				}
			}
			const double ActorsMs = (FPlatformTime::Seconds() - Start) * 1000.0;
			Start = FPlatformTime::Seconds();
			AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
			UClass* FoundationClass = GameClass(TEXT("Build_Foundation_8x1_01"));
			const AFGBuildable* Default = FoundationClass->GetDefaultObject<AFGBuildable>();
			int32 Removed = 0;
			for (const FVector& At : P->MassFoundations)
			{
				int32 Index = INDEX_NONE;
				if (const FRuntimeBuildableInstanceData* Data = FindLightweight(World, FoundationClass, At, &Index))
				{
					TArray<FInventoryStack> Refund;
					Default->GetLightweightBuildableDismantleRefundReturns(World, Data->BuiltWithRecipe, Data->TypeSpecificData, Refund);
					Items += StacksCount(Refund, nullptr);
					Lightweights->RemoveByInstanceIndex(FoundationClass, Index);
					++Removed;
				}
			}
			const double LightweightsMs = (FPlatformTime::Seconds() - Start) * 1000.0;
			Self.Step(FString::Printf(TEXT("R8 scale: 500 containers refunded and dismantled in %.1f ms (%.1f us each); %d foundations in %.1f ms (%.1f us each, the test's own search included); %d items"),
				ActorsMs, ActorsMs * 2.0, Removed, LightweightsMs, LightweightsMs * 1000.0 / FMath::Max(1, Removed), Items));
			Self.Check(Removed == 2000, FString::Printf(TEXT("R8 scale: %d of 2000 foundations removed"), Removed));
			P->DismantledAt = Self.Now();
			return EStep::Next;
		});
		R.Steps.Add([P](FRun& Self)
		{
			const int32 Left = Algo::CountIf(P->Mass, [](const TWeakObjectPtr<AActor>& Weak) { return IsValid(Weak.Get()); });
			if (Left > 0 && Self.Now() - P->DismantledAt < 30.0)
			{
				return EStep::Wait;
			}
			Self.Check(Left == 0, FString::Printf(TEXT("R8 scale: containers gone after %.2f s (left %d)"), Self.Now() - P->DismantledAt, Left));
			return EStep::Next;
		});
		// A crate grown after its spawn: room made, items added, a stated item kept
		R.Steps.Add([P](FRun& Self)
		{
			FSpawnInventoryCrateParameters Parameters;
			Parameters.bAllowCrateMerging = false;
			AFGCrate* Crate = AFGItemPickup_Spawnable::SpawnInventoryCrate(Self.GetWorld(), { FInventoryStack(1, Item(Asset::IronPlateItem)) }, Self.GetPlayer()->GetPawn()->GetActorLocation(), Parameters);
			UFGInventoryComponent* Inventory = Crate ? Crate->GetInventory() : nullptr;
			if (!Self.Check(Inventory != nullptr, TEXT("R8 crate spawned")))
			{
				return EStep::Next;
			}
			const int32 SizeAtSpawn = Inventory->GetSizeLinear();
			const int32 RefusedBefore = Inventory->AddStack(FInventoryStack(10, Item(Asset::IronRodItem)), true);
			Crate->SetAllowAddingItems(true);
			Inventory->Resize(200);
			int32 Added = 0;
			for (int32 Index = 0; Index < 150; ++Index)
			{
				Added += Inventory->AddStack(FInventoryStack(100, Item(Asset::IronPlateItem)), true);
			}
			Added += Inventory->AddStack(StateItem(77.0f), true);
			Crate->SetAllowAddingItems(false);
			P->GrownCrate = Crate->GetActorLocation();
			P->GrownItems = Inventory->GetNumItems(nullptr);
			Self.Step(FString::Printf(TEXT("R8 crate: %d slots at spawn, %d added before allowing, %d added after; now %d slots, %d items"), SizeAtSpawn, RefusedBefore, Added,
				Inventory->GetSizeLinear(), P->GrownItems));
			Self.Check(Added == 15001 && P->GrownItems == 15002, TEXT("R8 crate takes items after its spawn"));
			return EStep::Next;
		});
		AddSaveAndLoad(R, SaveName);
		R.Steps.Add([P](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			int32 Back = 0;
			for (int32 Index = 0; Index < P->ClassPaths.Num(); ++Index)
			{
				Back += FindActor(World, LoadClass<UObject>(nullptr, *P->ClassPaths[Index]), P->Places[Index]) ? 1 : 0;
			}
			UClass* FoundationClass = GameClass(TEXT("Build_Foundation_8x1_01"));
			Back += FindLightweight(World, FoundationClass, P->Foundation) ? 1 : 0;
			UClass* StorageClass = LoadClass<AFGBuildable>(nullptr, Asset::Storage);
			for (const FVector& At : P->MassPlaces)
			{
				Back += FindActor(World, StorageClass, At) ? 1 : 0;
			}
			for (const FVector& At : P->MassFoundations)
			{
				Back += FindLightweight(World, FoundationClass, At) ? 1 : 0;
			}
			Self.Check(Back == 0, FString::Printf(TEXT("R8 loaded: %d dismantled buildings came back"), Back));
			AFGCrate* Grown = nullptr;
			for (TActorIterator<AFGCrate> It(World); It; ++It)
			{
				if (FVector::Dist(It->GetActorLocation(), P->GrownCrate) < 10.0)
				{
					Grown = *It;
				}
			}
			TArray<FInventoryStack> Stacks;
			if (Grown)
			{
				Grown->GetInventory()->GetInventoryStacks(Stacks);
			}
			const FInventoryStack* Saw = Stacks.FindByPredicate([](const FInventoryStack& Stack) { return Stack.Item.HasState(); });
			Self.Check(Grown && StacksCount(Stacks, nullptr) == P->GrownItems && Saw && FMath::IsNearlyEqual(StateEnergy(*Saw), 77.0f),
				FString::Printf(TEXT("R8 loaded: grown crate %d with %d of %d items, chainsaw energy %.1f"), Grown != nullptr, StacksCount(Stacks, nullptr), P->GrownItems, Saw ? StateEnergy(*Saw) : -1.0f));
			return EStep::Next;
		});
		AddDeleteSave(R, SaveName);
	}

	using FCounts = AreaActionsBuildings::FItemCounts;

	void AddSolids(FCounts& Out, const TArray<FInventoryStack>& Stacks)
	{
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems() && UFGItemDescriptor::GetForm(Stack.Item.GetItemClass()) == EResourceForm::RF_SOLID)
			{
				Out.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
			}
		}
	}

	int32 Total(const FCounts& Counts)
	{
		int32 Sum = 0;
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Counts)
		{
			Sum += Entry.Value;
		}
		return Sum;
	}

	bool SameCounts(const FCounts& A, const FCounts& B)
	{
		FCounts Left = A;
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : B)
		{
			Left.FindOrAdd(Entry.Key) -= Entry.Value;
		}
		return !Algo::AnyOf(Left, [](const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry) { return Entry.Value != 0; });
	}

	// The items of all crates in the world
	int32 AllCrateItems(UWorld* World)
	{
		int32 Sum = 0;
		for (TActorIterator<AFGCrate> It(World); It; ++It)
		{
			Sum += IsValid(*It) && It->GetInventory() ? It->GetInventory()->GetNumItems(nullptr) : 0;
		}
		return Sum;
	}

	// Load a save made before: back to the main menu, the steps after this run in the loaded world
	void AddLoad(FRun& R, const FString& SaveName)
	{
		R.Steps.Add([SaveName](FRun& Self)
		{
			Self.bAwaitWorld = true;
			Self.bLoadIssued = false;
			Self.LoadSave = SaveName;
			Self.AwaitStarted = Self.Now();
			Self.Step(FString::Printf(TEXT("to the main menu to load %s"), *SaveName));
			UFGBlueprintFunctionLibrary::TravelToMainMenu(Self.GetPlayer());
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
	}

	void SetBudget(float Ms)
	{
		if (IConsoleVariable* Budget = IConsoleManager::Get().FindConsoleVariable(TEXT("AreaActions.ServerBudgetMs")))
		{
			Budget->Set(Ms, ECVF_SetByCode);
		}
	}

	// A Constructor whose input holds 2 m3 of water past its filters (as ClearInventories' scene); the water added
	int32 AddWater(AFGBuildableManufacturer* Constructor)
	{
		UFGInventoryComponent* Input = Constructor ? Constructor->GetInputInventory() : nullptr;
		if (!Input || Input->GetSizeLinear() == 0)
		{
			return 0;
		}
		const FItemFilter ItemFilter = Input->mItemFilter;
		const FFormFilter FormFilter = Input->mFormFilter;
		Input->mItemFilter.Unbind();
		Input->mFormFilter.Unbind();
		const TSubclassOf<UFGItemDescriptor> Allowed = Input->GetAllowedItemOnIndex(0);
		Input->SetAllowedItemOnIndex(0, nullptr);
		const int32 Water = Input->AddStackToIndex(0, FInventoryStack(2000, Item(Asset::WaterItem)), true);
		Input->SetAllowedItemOnIndex(0, Allowed);
		Input->mItemFilter = ItemFilter;
		Input->mFormFilter = FormFilter;
		return Water;
	}

	// What a dismantle scene holds and what the refund should be
	struct FDismantleState
	{
		FCopyState Pieces;
		TWeakObjectPtr<AFGCrate> AreaCrate;
		// (a) the game's refunds read before the action: GetDismantleRefund of each actor, the lightweights' from their class, the crate's
		// contents
		FCounts Expected;
		// (b) the contents read straight from the inventories and the crate, and the actors' refunds without cost: they must agree
		FCounts Contents;
		FCounts RefundWithoutCost;
		int32 Buildings = 0;
		bool bNoCost = false;
		int32 CrateItemsBefore = 0;
		FVector CratePlace = FVector::ZeroVector;
		int32 CrateItems = 0;
	};
	using FDismantleRef = TSharedRef<FDismantleState>;

	// The card's scene: 4 foundations, 2 walls, a Constructor and a container with items, a belt, a pole with a wire, a crate with items
	void BuildDismantleBasic(FRun& R, FScene& S, FDismantleState& D)
	{
		FCopyState& C = D.Pieces;
		C.Base = Ground(R, S.Center);
		for (const FVector2D Offset : { FVector2D(-400, -400), FVector2D(400, -400), FVector2D(-400, 400), FVector2D(400, 400) })
		{
			AddLightweight(R, C, TEXT("Build_Foundation_8x1_01"), TEXT("Recipe_Foundation_8x1_01"), FTransform(C.Base + FVector(Offset, 0)));
		}
		AddLightweight(R, C, TEXT("Build_Wall_8x4_01"), TEXT("Recipe_Wall_8x4_01"), FTransform(C.Base + FVector(0, -800, 100)));
		AddLightweight(R, C, TEXT("Build_Wall_8x4_01"), TEXT("Recipe_Wall_8x4_01"), FTransform(C.Base + FVector(0, 800, 100)));
		AFGBuildableManufacturer* Constructor = Cast<AFGBuildableManufacturer>(AddActor(R, S, C, TEXT("Build_ConstructorMk1"), TEXT("Recipe_ConstructorMk1"),
			FTransform(C.Base + FVector(-2500, -1500, 50))));
		if (Constructor)
		{
			Constructor->SetRecipe(Recipe(Asset::IronPlate));
			Constructor->GetInputInventory()->AddStack(FInventoryStack(10, Item(Asset::IronIngotItem)), true);
			Constructor->GetOutputInventory()->AddStack(FInventoryStack(5, Item(Asset::IronPlateItem)), true);
		}
		if (AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(AddActor(R, S, C, TEXT("Build_StorageContainerMk1"), TEXT("Recipe_StorageContainerMk1"),
			FTransform(C.Base + FVector(-2500, 0, 50)))))
		{
			Storage->GetStorageInventory()->AddStack(FInventoryStack(50, Item(Asset::IronPlateItem)), true);
			Storage->GetStorageInventory()->AddStack(FInventoryStack(30, Item(Asset::IronIngotItem)), true);
		}
		AddActor(R, S, C, TEXT("Build_ConveyorBeltMk1"), TEXT("Recipe_ConveyorBeltMk1"), FTransform(C.Base + FVector(-2500, 1500, 150)), [](AFGBuildable* Actor)
		{
			if (TArray<FSplinePointData>* Spline = Cast<AFGBuildableConveyorBelt>(Actor)->GetMutableSplinePointData())
			{
				Spline->Reset();
				Spline->Emplace(FVector::ZeroVector, FVector(0, 600, 0));
				Spline->Emplace(FVector(0, 600, 0), FVector(0, 600, 0));
			}
		});
		AFGBuildablePowerPole* Pole = Cast<AFGBuildablePowerPole>(AddActor(R, S, C, TEXT("Build_PowerPoleMK1"), TEXT("Recipe_PowerPoleMk1"),
			FTransform(C.Base + FVector(2500, -1500, 50))));
		AFGBuildableWire* Wire = Cast<AFGBuildableWire>(AddActor(R, S, C, TEXT("Build_PowerLine"), TEXT("Recipe_PowerLine"), FTransform(C.Base + FVector(0, -1500, 300))));
		UFGPowerConnectionComponent* MachinePlug = Constructor ? Constructor->FindComponentByClass<UFGPowerConnectionComponent>() : nullptr;
		const bool bWired = Pole && Wire && MachinePlug && Pole->GetPowerConnections().Num() > 0 && Wire->Connect(Pole->GetPowerConnection(0), MachinePlug);
		R.Check(bWired, FString::Printf(TEXT("wire from the pole to the Constructor: pole %d, wire %d, plug %d, length %.0f"), Pole != nullptr, Wire != nullptr,
			MachinePlug != nullptr, Wire ? Wire->GetLength() : -1.0f));
		FSpawnInventoryCrateParameters Parameters;
		Parameters.bAllowCrateMerging = false;
		D.AreaCrate = AFGItemPickup_Spawnable::SpawnInventoryCrate(R.GetWorld(), { FInventoryStack(25, Item(Asset::IronRodItem)) }, C.Base + FVector(2500, 1500, 100), Parameters);
		R.Check(D.AreaCrate.IsValid() && S.Area.Contains(D.AreaCrate->GetActorLocation()), TEXT("crate with 25 rods in the area"));
		for (const FVector& Place : S.ControlPlaces())
		{
			S.Controls.Add(SpawnStorage(R, Place, 15, 0));
		}
		D.Buildings = C.Pattern.Num() + 1;
	}

	// (a) and (b) read right before the request
	void RecordRefund(FRun& R, const FScene& S, FDismantleState& D)
	{
		UWorld* World = R.GetWorld();
		D.Expected.Reset();
		D.Contents.Reset();
		D.RefundWithoutCost.Reset();
		for (const TWeakObjectPtr<AFGBuildable>& Weak : S.Inside)
		{
			if (AFGBuildable* Actor = Weak.Get())
			{
				TArray<FInventoryStack> Refund, WithoutCost;
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Refund, D.bNoCost);
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, WithoutCost, true);
				AddSolids(D.Expected, Refund);
				AddSolids(D.RefundWithoutCost, WithoutCost);
				AddSolids(D.Contents, ContentsOf(Actor));
			}
		}
		for (FPiece& Piece : D.Pieces.Pattern)
		{
			const FRuntimeBuildableInstanceData* Data = Piece.bLightweight ? FindLightweight(World, Piece.Class, Piece.Transform.GetLocation()) : nullptr;
			if (Data && !D.bNoCost)
			{
				TArray<FInventoryStack> Refund;
				Piece.Class->GetDefaultObject<AFGBuildable>()->GetLightweightBuildableDismantleRefundReturns(World, Data->BuiltWithRecipe, Data->TypeSpecificData, Refund);
				AddSolids(D.Expected, Refund);
			}
		}
		if (const AFGCrate* Crate = D.AreaCrate.Get())
		{
			TArray<FInventoryStack> Stacks;
			Crate->GetInventory()->GetInventoryStacks(Stacks);
			AddSolids(D.Expected, Stacks);
			AddSolids(D.Contents, Stacks);
			AddSolids(D.RefundWithoutCost, Stacks);
		}
		R.Check(SameCounts(D.RefundWithoutCost, D.Contents), FString::Printf(TEXT("(b) the game's refund without cost %d equals the contents read straight %d"),
			Total(D.RefundWithoutCost), Total(D.Contents)));
		R.Step(FString::Printf(TEXT("expected refund %s (%d items)"), *ItemsText(D.Expected), Total(D.Expected)));
	}

	void AddDismantleScene(FRun& R, const FSceneRef& S, const FDismantleRef& D, double Side = 0.0)
	{
		AddScene(R, S, 4000.0, [D](FRun& Self, FScene& Scene) { BuildDismantleBasic(Self, Scene, *D); }, Side);
		R.Steps.Add([S, D](FRun& Self)
		{
			RecordRefund(Self, *S, *D);
			CountWorld(Self, D->Pieces);
			D->Pieces.JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			return EStep::Next;
		});
	}

	// The menu, the question answered with its first (Refund to crate) or second (No refund) button, the result
	void AddDismantle(FRun& R, const FSceneRef& S, bool bToCrate)
	{
		AddOpenAction(R, TEXT("MassDismantle"));
		AddOnWindow(R, TEXT("Widget_MessageYesNo"), [bToCrate](FRun& Self, UUserWidget* Window)
		{
			// The answers are named after what they do (D16)
			const UButton* Button = Cast<UButton>(Window->GetWidgetFromName(bToCrate ? TEXT("YesButton") : TEXT("NoButton")));
			FString Label;
			if (Button)
			{
				UWidgetTree::ForWidgetAndChildren(const_cast<UButton*>(Button), [&Label](UWidget* Child)
				{
					if (const UTextBlock* Text = Cast<UTextBlock>(Child))
					{
						Label = Text->GetText().ToString();
					}
				});
			}
			const TCHAR* Want = bToCrate ? TEXT("Refund to crate") : TEXT("No refund");
			Self.Check(Label == Want, FString::Printf(TEXT("button reads '%s' (want '%s')"), *Label, Want));
			Self.Check(PressButton(Window, bToCrate ? TEXT("YesButton") : TEXT("NoButton")), FString::Printf(TEXT("'%s' pressed"), Want));
		});
		AddResultWindow(R, S);
	}

	// Every building of the scene is gone, actors after their dismantle effect, the controls stay with their items
	void AddCheckGone(FRun& R, const FSceneRef& S, const FDismantleRef& D)
	{
		TSharedRef<double> Since = MakeShared<double>(0.0);
		R.Steps.Add([S, D, Since](FRun& Self)
		{
			*Since = *Since > 0.0 ? *Since : Self.Now();
			const int32 Left = Algo::CountIf(S->Inside, [](const TWeakObjectPtr<AFGBuildable>& Weak) { return IsValid(Weak.Get()); });
			if (Left > 0 && Self.Now() - *Since < 15.0)
			{
				return EStep::Wait;
			}
			Self.Check(Left == 0, FString::Printf(TEXT("actors gone: %d of %d left"), Left, S->Inside.Num()));
			int32 Lightweights = 0;
			for (const FPiece& Piece : D->Pieces.Pattern)
			{
				Lightweights += Piece.bLightweight && FindLightweight(Self.GetWorld(), Piece.Class, Piece.Transform.GetLocation()) ? 1 : 0;
			}
			Self.Check(Lightweights == 0, FString::Printf(TEXT("lightweight instances left: %d"), Lightweights));
			Self.Check(!D->AreaCrate.IsValid(), TEXT("the area's crate is gone"));
			for (int32 Index = 0; Index < S->Controls.Num(); ++Index)
			{
				Self.Check(StorageItems(S->Controls[Index].Get()) == 15, FString::Printf(TEXT("control container %d (W4): %d items"), Index, StorageItems(S->Controls[Index].Get())));
			}
			return EStep::Next;
		});
	}

	// One new crate next to the player with exactly the expected refund
	void AddCheckRefundCrate(FRun& R, const FSceneRef& S, const FDismantleRef& D)
	{
		R.Steps.Add([S, D](FRun& Self)
		{
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			FCounts InCrate;
			for (const AFGCrate* Crate : Crates)
			{
				TArray<FInventoryStack> Stacks;
				Crate->GetInventory()->GetInventoryStacks(Stacks);
				AddSolids(InCrate, Stacks);
			}
			Self.Check(Crates.Num() == 1 && SameCounts(InCrate, D->Expected), FString::Printf(TEXT("new crates %d; in them %s (%d), expected %s (%d)"), Crates.Num(),
				*ItemsText(InCrate), Total(InCrate), *ItemsText(D->Expected), Total(D->Expected)));
			const double Distance = Crates.Num() == 1 ? FVector::Dist(Crates[0]->GetActorLocation(), Self.GetPlayer()->GetPawn()->GetActorLocation()) : -1.0;
			Self.Check(Distance >= 0.0 && Distance < 1500.0, FString::Printf(TEXT("crate next to the player: %.0f cm"), Distance));
			if (Crates.Num() == 1)
			{
				D->CratePlace = Crates[0]->GetActorLocation();
				D->CrateItems = Total(InCrate);
			}
			const FString Want = FString::Printf(TEXT("Dismantled %d buildings. %d items were refunded. The items are in 1 crate next to you."), D->Buildings, Total(D->Expected));
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			Self.Check(AAreaActionsServerSubsystem::FinishedJobs - D->Pieces.JobsBefore == 1, TEXT("one AAJOB line"));
			return EStep::Next;
		});
	}

	void AddDismantleBasic(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		AddDismantleScene(R, S, D);
		AddDismantle(R, S, true);
		AddCheckGone(R, S, D);
		AddCheckRefundCrate(R, S, D);
	}

	void AddDismantleNoCrate(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		AddDismantleScene(R, S, D);
		AddDismantle(R, S, false);
		AddCheckGone(R, S, D);
		R.Steps.Add([S, D](FRun& Self)
		{
			Self.Check(NewCrates(Self, *S).IsEmpty(), FString::Printf(TEXT("no new crate: %d"), NewCrates(Self, *S).Num()));
			const FString Want = FString::Printf(TEXT("Dismantled %d buildings. Nothing was refunded."), D->Buildings);
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			return EStep::Next;
		});
	}

	void AddDismantleNoCost(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		D->bNoCost = true;
		R.Steps.Add([](FRun& Self)
		{
			AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>();
			if (State)
			{
				State->SetCheatNoCost(true);
			}
			Self.Check(State && PlayerInventory(Self)->GetNoBuildCost(), TEXT("No Build Cost on"));
			return EStep::Next;
		});
		AddDismantleScene(R, S, D);
		R.Steps.Add([D](FRun& Self)
		{
			Self.Check(SameCounts(D->Expected, D->Contents), FString::Printf(TEXT("expected refund is the contents only: %d of %d"), Total(D->Expected), Total(D->Contents)));
			return EStep::Next;
		});
		AddDismantle(R, S, true);
		AddCheckGone(R, S, D);
		AddCheckRefundCrate(R, S, D);
		R.Steps.Add([](FRun& Self)
		{
			if (AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>())
			{
				State->SetCheatNoCost(false);
			}
			return EStep::Next;
		});
	}

	// The HUB and the Space Elevator, and a railroad switch lever (which the game dismantles only with its track): CanDismantle of each,
	// read right before the request, decides what goes
	void AddDismantleProtected(FRun& R)
	{
		struct FProtected
		{
			TArray<TWeakObjectPtr<AFGBuildable>> Buildings;
			TArray<bool> CanDismantle;
			int32 Refused = 0;
		};
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<FProtected> P = MakeShared<FProtected>();
		AddScene(R, S, 6000.0, [P](FRun& Self, FScene& Scene)
		{
			const FVector Base = Ground(Self, Scene.Center);
			struct FWhat { const TCHAR* Class; const TCHAR* Recipe; FVector Offset; };
			for (const FWhat& What : { FWhat{ TEXT("Build_TradingPost"), TEXT("Recipe_TradingPost"), FVector(-3000, -3000, 0) },
				FWhat{ TEXT("Build_SpaceElevator"), TEXT("Recipe_SpaceElevator"), FVector(1500, 1500, 0) },
				FWhat{ TEXT("Build_RailroadSwitchControl"), TEXT("Recipe_RailroadSwitchControl"), FVector(-3000, 3000, 0) } })
			{
				AFGBuildable* Building = SpawnGameBuildable(Self, What.Class, What.Recipe, Base + What.Offset);
				Self.Step(FString::Printf(TEXT("%s spawned: %d"), What.Class, Building != nullptr));
				if (Building)
				{
					P->Buildings.Add(Building);
					Scene.Inside.Add(Building);
				}
			}
			for (const FVector& Place : Scene.ControlPlaces())
			{
				Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
		});
		R.Steps.Add([P](FRun& Self)
		{
			for (const TWeakObjectPtr<AFGBuildable>& Weak : P->Buildings)
			{
				const bool bCan = Weak.IsValid() && IFGDismantleInterface::Execute_CanDismantle(Weak.Get());
				P->CanDismantle.Add(bCan);
				P->Refused += bCan ? 0 : 1;
				Self.Step(FString::Printf(TEXT("%s CanDismantle %d"), *GetNameSafe(Weak.IsValid() ? Weak->GetClass() : nullptr), bCan));
			}
			Self.Check(P->Refused > 0, FString::Printf(TEXT("the scene has %d buildings the game refuses to dismantle"), P->Refused));
			return EStep::Next;
		});
		AddDismantle(R, S, true);
		R.Steps.Add(WaitFrames(60));
		R.Steps.Add([S, P](FRun& Self)
		{
			for (int32 Index = 0; Index < P->Buildings.Num(); ++Index)
			{
				const bool bStands = IsValid(P->Buildings[Index].Get()) && !P->Buildings[Index]->GetIsDismantled();
				Self.Check(bStands != P->CanDismantle[Index], FString::Printf(TEXT("building %d: CanDismantle %d, stands %d"), Index, P->CanDismantle[Index], bStands));
			}
			const FString Want = FString::Printf(TEXT("%d %s the game does not let you dismantle %s left."), P->Refused, P->Refused == 1 ? TEXT("building") : TEXT("buildings"),
				P->Refused == 1 ? TEXT("was") : TEXT("were"));
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			for (int32 Index = 0; Index < S->Controls.Num(); ++Index)
			{
				Self.Check(StorageItems(S->Controls[Index].Get()) == 15, FString::Printf(TEXT("control container %d (W4): %d items"), Index, StorageItems(S->Controls[Index].Get())));
			}
			return EStep::Next;
		});
	}

	void AddDismantleSaveLoad(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		const FString SaveName = TEXT("AATest_DismantleSaveLoad");
		AddDismantleScene(R, S, D);
		AddDismantle(R, S, true);
		AddCheckGone(R, S, D);
		AddCheckRefundCrate(R, S, D);
		AddSaveAndLoad(R, SaveName);
		R.Steps.Add([D](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			int32 Back = 0;
			for (FPiece& Piece : D->Pieces.Pattern)
			{
				Piece.Resolve();
				const FVector At = Piece.Transform.GetLocation();
				Back += (Piece.bLightweight ? FindLightweight(World, Piece.Class, At) != nullptr : FindActor(World, Piece.Class, At) != nullptr) ? 1 : 0;
			}
			Self.Check(Back == 0, FString::Printf(TEXT("loaded: %d of %d dismantled buildings came back"), Back, D->Pieces.Pattern.Num()));
			const AFGCrate* Crate = nullptr;
			for (TActorIterator<AFGCrate> It(World); It; ++It)
			{
				Crate = FVector::Dist(It->GetActorLocation(), D->CratePlace) < 10.0 ? *It : Crate;
			}
			const int32 Items = Crate ? Crate->GetInventory()->GetNumItems(nullptr) : -1;
			Self.Check(Items == D->CrateItems, FString::Printf(TEXT("loaded: the refund crate holds %d of %d items"), Items, D->CrateItems));
			return EStep::Next;
		});
		AddDeleteSave(R, SaveName);
	}

	// W5 on 4938 foundations and 103 containers with items, at one height above the ground
	void AddDismantleLarge(FRun& R)
	{
		// 71 x 71 = 5041 places
		static constexpr int32 Side = 71;
		static constexpr double Spacing = 800.0;
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<TArray<FVector>> Foundations = MakeShared<TArray<FVector>>();
		AddEquipTool(R);
		R.Steps.Add([S](FRun& Self)
		{
			PlaceScene(Self, *S, Side * Spacing / 2.0, 2000.0);
			S->Center.Z += 2000.0;
			S->Area.MinZ = S->Center.Z - 500.0;
			S->Area.MaxZ = S->Center.Z + 500.0;
			for (const FVector& Place : S->ControlPlaces())
			{
				S->Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
			ListenResults(Self, S);
			return EStep::Next;
		});
		for (int32 Batch = 0; Batch < 6; ++Batch)
		{
			R.Steps.Add([S, Foundations, Batch](FRun& Self)
			{
				AreaActionsBuildings::FSnapshot Foundation;
				Foundation.Class = GameClass(TEXT("Build_Foundation_8x1_01"));
				Foundation.BuiltWith = RecipeOfClass(Self, Foundation.Class, TEXT("Recipe_Foundation_8x1_01"));
				for (int32 Index = Batch * 1000; Index < FMath::Min((Batch + 1) * 1000, Side * Side); ++Index)
				{
					const FVector At = S->Center + FVector((Index % Side - Side / 2.0 + 0.5) * Spacing, (Index / Side - Side / 2.0 + 0.5) * Spacing, 0.0);
					// Every 49th place holds a container instead
					if (Index % 49 == 0)
					{
						S->Inside.Add(SpawnStorage(Self, At, 10, 0));
					}
					else if (AreaActionsBuildings::Spawn(Self.GetWorld(), Foundation, FTransform(At)))
					{
						Foundations->Add(At);
					}
				}
				return EStep::Next;
			});
		}
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([S, Foundations](FRun& Self)
		{
			Self.Check(S->Inside.Num() + Foundations->Num() == Side * Side && AllValid(S->Inside), FString::Printf(TEXT("%d containers and %d foundations built"),
				S->Inside.Num(), Foundations->Num()));
			Self.Check(SetToolArea(Self, *S), TEXT("tool area set"));
			SnapshotCrates(Self, *S);
			Self.MaxFrameMs = 0.0;
			Self.bMeasureFrames = true;
			Self.StepTimeoutSec = 180.0;
			return EStep::Next;
		});
		AddDismantle(R, S, true);
		R.Steps.Add([S, Foundations](FRun& Self)
		{
			Self.bMeasureFrames = false;
			Self.StepTimeoutSec = 60.0;
			Self.Check(Self.MaxFrameMs <= 200.0, FString::Printf(TEXT("longest frame during the job: %.1f ms (W5: 200)"), Self.MaxFrameMs));
			Self.Check(S->Message.Contains(FString::Printf(TEXT("Dismantled %s buildings."), *FText::AsNumber(Side * Side).ToString())), FString::Printf(TEXT("result '%s'"), *S->Message));
			int32 Left = 0;
			UClass* FoundationClass = GameClass(TEXT("Build_Foundation_8x1_01"));
			for (const FVector& At : *Foundations)
			{
				Left += FindLightweight(Self.GetWorld(), FoundationClass, At) ? 1 : 0;
			}
			Self.Check(Left == 0, FString::Printf(TEXT("foundations left: %d"), Left));
			for (int32 Index = 0; Index < S->Controls.Num(); ++Index)
			{
				Self.Check(StorageItems(S->Controls[Index].Get()) == 15, FString::Printf(TEXT("control container %d (W4): %d items"), Index, StorageItems(S->Controls[Index].Get())));
			}
			return EStep::Next;
		});
	}

	// AA-078 (1): one building per frame, a save while the job is half done, the save loaded: every item is in a building or a crate
	enum class EJobKind : uint8 { SetRecipe, Clear, Dismantle };

	void AddSaveDuringJob(FRun& R, EJobKind Kind, int32 RequestId)
	{
		static constexpr int32 Count = 25;
		struct FJobState
		{
			TArray<FVector> Places;
			FString ClassPath;
			int32 Before = 0;
			int32 CostEach = 0;
			int32 SaveResult = -1;
			uint64 SentFrame = 0;
			int32 DoneAtSave = 0;
		};
		const TCHAR* Names[] = { TEXT("SetRecipe"), TEXT("ClearInventories"), TEXT("MassDismantle") };
		const FString Name = Names[static_cast<int32>(Kind)];
		const FString SaveName = FString::Printf(TEXT("AATest_SaveDuringJob_%s"), *Name);
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<FJobState> J = MakeShared<FJobState>();
		auto Done = [Kind](const AFGBuildable* Building)
		{
			switch (Kind)
			{
			case EJobKind::SetRecipe:
				return Cast<AFGBuildableManufacturer>(Building) && Cast<AFGBuildableManufacturer>(Building)->GetCurrentRecipe() == Recipe(Asset::IronRod);
			case EJobKind::Clear:
				return StorageItems(Building) == 0;
			default:
				return !IsValid(Building) || Building->GetIsDismantled();
			}
		};
		auto ItemsAt = [](UWorld* World, const FJobState& State, int32& OutMissing)
		{
			int32 Sum = 0;
			OutMissing = 0;
			UClass* Class = LoadClass<UObject>(nullptr, *State.ClassPath);
			for (const FVector& At : State.Places)
			{
				const AFGBuildable* Building = FindActor(World, Class, At);
				OutMissing += Building ? 0 : 1;
				for (const FInventoryStack& Stack : Building ? ContentsOf(Building) : TArray<FInventoryStack>())
				{
					Sum += Stack.HasItems() ? Stack.NumItems : 0;
				}
			}
			return Sum;
		};
		AddScene(R, S, 4000.0, [Kind, J](FRun& Self, FScene& Scene)
		{
			MakeAvailable(Self, Asset::IronRod);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FVector At = Scene.Center + FVector(-2800.0 + (Index % 5) * 1400.0, -2800.0 + (Index / 5) * 1400.0, 0.0);
				AFGBuildable* Building = Kind == EJobKind::SetRecipe ? static_cast<AFGBuildable*>(SpawnMachine(Self, Asset::Constructor, At, Asset::IronPlate, 10, 5)) : SpawnStorage(Self, At, 50, 30);
				Scene.Inside.Add(Building);
				if (Building)
				{
					J->Places.Add(Building->GetActorLocation());
					J->ClassPath = Building->GetClass()->GetPathName();
				}
			}
			for (const FVector& Place : Scene.ControlPlaces())
			{
				Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
		});
		R.Steps.Add([S, J, Kind, RequestId, Name, ItemsAt](FRun& Self)
		{
			int32 Missing = 0;
			J->Before = ItemsAt(Self.GetWorld(), *J, Missing) + AllCrateItems(Self.GetWorld());
			for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(Self.GetWorld(), GameClass(TEXT("Recipe_StorageContainerMk1"))))
			{
				J->CostEach += Ingredient.Amount;
			}
			SetBudget(0.0f);
			J->SentFrame = GFrameCounter;
			Self.Step(FString::Printf(TEXT("%s: %d items in the buildings and crates; request %d with a budget of 0 ms"), *Name, J->Before, RequestId));
			UAreaActionsRCO* Rco = UAreaActionsRCO::Of(Self.GetPlayer());
			switch (Kind)
			{
			case EJobKind::SetRecipe:
				Rco->ServerSetRecipe(RequestId, S->Area, Recipe(Asset::IronRod));
				break;
			case EJobKind::Clear:
				Rco->ServerClearInventories(RequestId, S->Area, true);
				break;
			default:
				Rco->ServerDismantle(RequestId, S->Area, true);
				break;
			}
			return EStep::Next;
		});
		R.Steps.Add([S, J, Done, SaveName](FRun& Self)
		{
			const int32 Changed = Algo::CountIf(S->Inside, [&Done](const TWeakObjectPtr<AFGBuildable>& Weak) { return Done(Weak.Get()); });
			if (Changed < Count / 2)
			{
				return EStep::Wait;
			}
			J->DoneAtSave = Changed;
			Self.Step(FString::Printf(TEXT("saving %s with %d of %d buildings done, %llu frames after the request"), *SaveName, Changed, Count, GFrameCounter - J->SentFrame));
			UFGSaveSession::Get(Self.GetWorld())->SaveGame(SaveName, FOnSaveGameComplete::CreateLambda([J](bool bOk, const FText&, void*) { J->SaveResult = bOk ? 1 : 0; }), nullptr);
			return EStep::Next;
		});
		R.Steps.Add([S, J, RequestId](FRun& Self)
		{
			const FAreaActionsResult* Result = S->Results.Find(RequestId);
			if (J->SaveResult < 0 || !Result)
			{
				return EStep::Wait;
			}
			SetBudget(2.0f);
			Self.Check(J->SaveResult == 1, TEXT("saved during the job"));
			Self.Check(J->DoneAtSave < Count, FString::Printf(TEXT("the save came mid-job: %d of %d done"), J->DoneAtSave, Count));
			Self.Check(Result->Code == EAreaActionsResultCode::Ok && Result->Buildings == Count && GFrameCounter - J->SentFrame >= 10,
				FString::Printf(TEXT("job done: code %d, buildings %d, %llu frames (at least 10)"), static_cast<int32>(Result->Code), Result->Buildings, GFrameCounter - J->SentFrame));
			return EStep::Next;
		});
		AddLoad(R, SaveName);
		R.Steps.Add([J, Kind, Name, ItemsAt](FRun& Self)
		{
			int32 Missing = 0;
			const int32 InBuildings = ItemsAt(Self.GetWorld(), *J, Missing);
			const int32 InCrates = AllCrateItems(Self.GetWorld());
			// A building dismantled before the save gave its build cost too
			const int32 Want = J->Before + (Kind == EJobKind::Dismantle ? Missing * J->CostEach : 0);
			Self.Check(InBuildings + InCrates == Want && (Kind == EJobKind::Dismantle ? Missing > 0 && Missing < Count : Missing == 0),
				FString::Printf(TEXT("%s loaded: %d in the buildings + %d in crates = %d, want %d (%d buildings missing, cost %d each)"), *Name, InBuildings, InCrates,
					InBuildings + InCrates, Want, Missing, J->CostEach));
			return EStep::Next;
		});
		AddDeleteSave(R, SaveName);
	}

	// AA-078 (2): a chainsaw with energy stored keeps it in the crate, through "Move to crate" and through MassDismantle
	void AddCrateItemState(FRun& R, bool bDismantle, float Energy, double Side)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [Energy](FRun& Self, FScene& Scene)
		{
			AFGBuildableStorage* Storage = SpawnStorage(Self, Scene.Slot(0), 10, 0);
			if (Storage)
			{
				Storage->GetStorageInventory()->AddStack(StateItem(Energy), true);
			}
			Scene.Inside.Add(Storage);
			for (const FVector& Place : Scene.ControlPlaces())
			{
				Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
		}, Side);
		if (bDismantle)
		{
			AddDismantle(R, S, true);
		}
		else
		{
			AddOpenAction(R, TEXT("ClearInventories"));
			AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("NoButton"));
			AddResultWindow(R, S);
		}
		R.Steps.Add([S, Energy, bDismantle](FRun& Self)
		{
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			TArray<FInventoryStack> Stacks;
			if (Crates.Num() == 1)
			{
				Crates[0]->GetInventory()->GetInventoryStacks(Stacks);
			}
			const FInventoryStack* Saw = Stacks.FindByPredicate([](const FInventoryStack& Stack) { return Stack.Item.HasState(); });
			Self.Check(Crates.Num() == 1 && Saw && FMath::IsNearlyEqual(StateEnergy(*Saw), Energy), FString::Printf(TEXT("%s: crates %d, chainsaw energy %.1f (want %.1f)"),
				bDismantle ? TEXT("MassDismantle") : TEXT("Move to crate"), Crates.Num(), Saw ? StateEnergy(*Saw) : -1.0f, Energy));
			return EStep::Next;
		});
	}

	// AA-085 (1): more stacks than one crate takes: crates of 200 stacks, the last one partly filled, every item there
	void AddCrateSplit(FRun& R, bool bDismantle, double Side)
	{
		constexpr int32 Containers = 10;
		struct FSplit
		{
			FCounts Taken;
		};
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<FSplit> P = MakeShared<FSplit>();
		AddScene(R, S, 4000.0, [P, bDismantle](FRun& Self, FScene& Scene)
		{
			const TSubclassOf<UFGItemDescriptor> Plate = Item(Asset::IronPlateItem);
			const int32 PlateStack = UFGItemDescriptor::GetStackSize(Plate);
			for (int32 Index = 0; Index < Containers; ++Index)
			{
				AFGBuildableStorage* Storage = SpawnStorage(Self, Scene.Slot(Index), 0, 0);
				if (Storage)
				{
					UFGInventoryComponent* Inventory = Storage->GetStorageInventory();
					P->Taken.FindOrAdd(Plate) += Inventory->AddStack(FInventoryStack(Inventory->GetSizeLinear() * PlateStack, Plate), true);
					if (bDismantle)
					{
						TArray<FInventoryStack> Cost;
						IFGDismantleInterface::Execute_GetDismantleRefund(Storage, Cost, false);
						FCounts Refund;
						AddSolids(Refund, Cost);
						for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Refund)
						{
							// The refund holds the contents again: only the build cost is extra
							P->Taken.FindOrAdd(Entry.Key) += Entry.Key == Plate ? Entry.Value - Inventory->GetNumItems(Plate) : Entry.Value;
						}
					}
				}
				Scene.Inside.Add(Storage);
			}
			for (const FVector& Place : Scene.ControlPlaces())
			{
				Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
			}
		}, Side);
		if (bDismantle)
		{
			AddDismantle(R, S, true);
		}
		else
		{
			AddOpenAction(R, TEXT("ClearInventories"));
			AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("NoButton"));
			AddResultWindow(R, S);
		}
		R.Steps.Add([S, P, bDismantle](FRun& Self)
		{
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			FCounts InCrates;
			int32 Full = 0;
			int32 Stacks = 0;
			for (const AFGCrate* Crate : Crates)
			{
				TArray<FInventoryStack> Each;
				Crate->GetInventory()->GetInventoryStacks(Each);
				AddSolids(InCrates, Each);
				Full += Each.Num() == 200 ? 1 : 0;
				Self.Check(Each.Num() <= 200 && Crate->GetInventory()->GetSizeLinear() == Each.Num(), FString::Printf(TEXT("crate %s: %d stacks in %d slots (at most 200)"),
					*Crate->GetName(), Each.Num(), Crate->GetInventory()->GetSizeLinear()));
			}
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : P->Taken)
			{
				Stacks += FMath::DivideAndRoundUp(Entry.Value, UFGItemDescriptor::GetStackSize(Entry.Key));
			}
			const int32 Least = FMath::DivideAndRoundUp(Stacks, 200);
			Self.Check(Crates.Num() >= Least && Crates.Num() <= Least + 1 && Full == Crates.Num() - 1 && SameCounts(InCrates, P->Taken),
				FString::Printf(TEXT("%s: %d crates (%d full) for %d stacks; in them %s, taken %s"), bDismantle ? TEXT("MassDismantle") : TEXT("Move to crate"), Crates.Num(), Full,
					Stacks, *ItemsText(InCrates), *ItemsText(P->Taken)));
			const FString Want = FString::Printf(TEXT("The items are in %d crates next to you."), Crates.Num());
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			return EStep::Next;
		});
	}

	// AA-085 (2): only fluids taken: no word of a crate; no room next to the player: the crate stands by the building and the message says
	// so; no crate at all: the items are named lost and an Error line is written
	void AddCrateMessages(FRun& R)
	{
		{
			FSceneRef S = MakeShared<FScene>();
			AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
			{
				AFGBuildableManufacturer* Constructor = SpawnMachine(Self, Asset::Constructor, Scene.Slot(0), Asset::IronPlate, 0, 0);
				Self.Check(AddWater(Constructor) == 2000, TEXT("2 m³ of water in the Constructor"));
				Scene.Inside.Add(Constructor);
				for (const FVector& Place : Scene.ControlPlaces())
				{
					Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
				}
			});
			AddOpenAction(R, TEXT("ClearInventories"));
			AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("NoButton"));
			AddResultWindow(R, S);
			R.Steps.Add([S](FRun& Self)
			{
				Self.Check(NewCrates(Self, *S).IsEmpty() && !S->Message.Contains(TEXT("crate")) && S->Message.Contains(TEXT("2 m³ of fluids were deleted")),
					FString::Printf(TEXT("fluids only: new crates %d, message '%s'"), NewCrates(Self, *S).Num(), *S->Message));
				return EStep::Next;
			});
		}
		for (const int32 Mode : { 1, 2 })
		{
			FSceneRef S = MakeShared<FScene>();
			TSharedRef<int32> Errors = MakeShared<int32>(0);
			TSharedRef<TUniquePtr<FErrorCounter>> Counter = MakeShared<TUniquePtr<FErrorCounter>>();
			AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
			{
				Scene.Inside.Add(SpawnStorage(Self, Scene.Slot(0), 20, 0));
				for (const FVector& Place : Scene.ControlPlaces())
				{
					Scene.Controls.Add(SpawnStorage(Self, Place, 15, 0));
				}
			}, Mode * 12000.0);
			R.Steps.Add([Mode, Counter](FRun& Self)
			{
				AreaActionsJobs::SetCratesFailForTest(Mode);
				*Counter = MakeUnique<FErrorCounter>();
				return EStep::Next;
			});
			AddOpenAction(R, TEXT("ClearInventories"));
			AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("NoButton"));
			AddResultWindow(R, S);
			R.Steps.Add([S, Mode, Counter](FRun& Self)
			{
				AreaActionsJobs::SetCratesFailForTest(0);
				const int32 ErrorLines = (*Counter)->Errors;
				Counter->Reset();
				const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
				if (Mode == 1)
				{
					const FVector Storage = S->Slot(0);
					const double Distance = Crates.Num() == 1 ? FVector::Dist2D(Crates[0]->GetActorLocation(), Storage) : -1.0;
					Self.Check(Crates.Num() == 1 && Distance >= 0.0 && Distance < 1500.0 && Crates[0]->GetInventory()->GetNumItems(nullptr) == 20,
						FString::Printf(TEXT("no room at the player: crates %d, %.0f cm from the container"), Crates.Num(), Distance));
					const TCHAR* Want = TEXT("The items are in 1 crate; 1 could not be placed next to you and stands by the buildings.");
					Self.Check(S->Message.Contains(Want) && ErrorLines == 0, FString::Printf(TEXT("result '%s' has '%s'; error lines %d"), *S->Message, Want, ErrorLines));
				}
				else
				{
					const TCHAR* Want = TEXT("20 items were lost: no crate could be placed.");
					Self.Check(Crates.IsEmpty() && S->Message.Contains(Want) && !S->Message.Contains(TEXT("next to you")) && ErrorLines == 1,
						FString::Printf(TEXT("no crate: crates %d, result '%s' has '%s', error lines %d (want 1)"), Crates.Num(), *S->Message, Want, ErrorLines));
				}
				return EStep::Next;
			});
		}
	}

	bool BuildScenario(FRun& R)
	{
		if (R.Name == TEXT("DismantleBasic"))
		{
			AddDismantleBasic(R);
			return true;
		}
		if (R.Name == TEXT("DismantleNoCrate"))
		{
			AddDismantleNoCrate(R);
			return true;
		}
		if (R.Name == TEXT("DismantleNoCost"))
		{
			AddDismantleNoCost(R);
			return true;
		}
		if (R.Name == TEXT("DismantleProtected"))
		{
			AddDismantleProtected(R);
			return true;
		}
		if (R.Name == TEXT("DismantleSaveLoad"))
		{
			R.StepTimeoutSec = 60.0;
			AddDismantleSaveLoad(R);
			return true;
		}
		if (R.Name == TEXT("DismantleLarge"))
		{
			R.StepTimeoutSec = 60.0;
			AddDismantleLarge(R);
			return true;
		}
		if (R.Name == TEXT("SaveDuringJob"))
		{
			R.StepTimeoutSec = 60.0;
			AddSaveDuringJob(R, EJobKind::SetRecipe, 301);
			AddSaveDuringJob(R, EJobKind::Clear, 302);
			AddSaveDuringJob(R, EJobKind::Dismantle, 303);
			return true;
		}
		if (R.Name == TEXT("CrateItemState"))
		{
			AddCrateItemState(R, false, 55.5f, 0.0);
			AddCrateItemState(R, true, 66.5f, 12000.0);
			return true;
		}
		if (R.Name == TEXT("CrateSplit"))
		{
			AddCrateSplit(R, false, 0.0);
			AddCrateSplit(R, true, 12000.0);
			return true;
		}
		if (R.Name == TEXT("CrateMessages"))
		{
			AddCrateMessages(R);
			return true;
		}
		if (R.Name == TEXT("DismantleProbe"))
		{
			R.StepTimeoutSec = 60.0;
			AddDismantleProbe(R);
			return true;
		}
		if (R.Name == TEXT("CopyBasic"))
		{
			AddCopyBasic(R);
			return true;
		}
		if (R.Name == TEXT("CopyRotate"))
		{
			AddCopyRotate(R);
			return true;
		}
		if (R.Name == TEXT("CopyTwice"))
		{
			AddCopyTwice(R);
			return true;
		}
		if (R.Name == TEXT("CopySaveLoad"))
		{
			R.StepTimeoutSec = 60.0;
			AddCopySaveLoad(R);
			return true;
		}
		if (R.Name == TEXT("CopyCost"))
		{
			AddCopyCost(R);
			return true;
		}
		if (R.Name == TEXT("CopyCostMissing"))
		{
			AddCopyCostMissing(R);
			return true;
		}
		if (R.Name == TEXT("CopyNoCost"))
		{
			AddCopyNoCost(R);
			return true;
		}
		if (R.Name == TEXT("CopyCostDepot"))
		{
			AddCopyCostDepot(R);
			return true;
		}
		if (R.Name == TEXT("FillGrid"))
		{
			AddFillGrid(R, false);
			return true;
		}
		if (R.Name == TEXT("FillNegative"))
		{
			AddFillGrid(R, true);
			return true;
		}
		if (R.Name == TEXT("Preview"))
		{
			R.StepTimeoutSec = 60.0;
			AddPreview(R);
			return true;
		}
		if (R.Name == TEXT("SelectLightweight"))
		{
			AddSelectLightweight(R);
			return true;
		}
		if (R.Name == TEXT("FillLarge"))
		{
			AddFillLarge(R);
			return true;
		}
		if (R.Name == TEXT("DoubleClick"))
		{
			AddDoubleClick(R, true, 0, -27000.0);
			AddDoubleClick(R, true, 3, -9000.0);
			AddDoubleClick(R, false, 0, 9000.0);
			AddDoubleClick(R, false, 3, 27000.0);
			return true;
		}
		if (R.Name == TEXT("SpawnProbe"))
		{
			R.StepTimeoutSec = 60.0;
			AddSpawnProbe(R);
			return true;
		}
		if (R.Name == TEXT("BindSignature"))
		{
			AddBindSignature(R);
			return true;
		}
		if (R.Name == TEXT("ToolMenuClose"))
		{
			AddToolMenuClose(R);
			return true;
		}
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
			AddClearInventories(R, false);
			return true;
		}
		if (R.Name == TEXT("ClearInventoriesCrate"))
		{
			AddClearInventories(R, true);
			return true;
		}
		if (R.Name == TEXT("SetStationCancel"))
		{
			AddSetStationCancel(R);
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
		if (R.bAwaitWorld)
		{
			if (R.Now() - R.AwaitStarted > 180.0)
			{
				R.Check(false, FString::Printf(TEXT("no game world 180 s after leaving for the menu (load %s issued %d)"), *R.LoadSave, R.bLoadIssued));
				Finish(R);
				return false;
			}
			// The load is started from the menu. The game's main menu is the game map with ?closed (TravelToMainMenu), an older one a Menu map.
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Menu = Context.World();
				const bool bMenu = Menu && Menu->IsGameWorld() && (Menu->GetMapName().Contains(TEXT("Menu")) || Menu->URL.HasOption(TEXT("closed")));
				if (!R.bLoadIssued && bMenu && Menu->HasBegunPlay() && R.Now() - R.AwaitStarted > 5.0)
				{
					R.bLoadIssued = true;
					R.Step(FString::Printf(TEXT("in %s: loading %s"), *Menu->GetMapName(), *R.LoadSave));
					UGameplayStatics::OpenLevel(Menu, FName(TEXT("/Game/FactoryGame/Map/GameLevel01/Persistent_Level")), true, FString::Printf(TEXT("skipOnboarding?listen?loadgame=%s"), *R.LoadSave));
				}
			}
			return true;
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
	if (Run && Run->bAwaitWorld && World && Run->bLoadIssued)
	{
		// The loaded save: the scenario goes on with its next step after the load has settled
		Run->World = World;
		Run->bAwaitWorld = false;
		Run->bStarted = false;
		Run->Started = FPlatformTime::Seconds();
		Run->StartFrame = GFrameCounter;
		Run->Step(FString::Printf(TEXT("loaded into %s, net mode %d"), *World->GetMapName(), static_cast<int32>(World->GetNetMode())));
		return;
	}
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
	AddPlace(*Run);
	if (!BuildScenario(*Run))
	{
		Run->Check(false, TEXT("unknown scenario"));
		Finish(*Run);
		return;
	}
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
}

#endif

#include "AreaActionsTests.h"

#if AA_WITH_TESTS

#include "AreaActionsEquipment.h"
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

	bool BuildScenario(FRun& R)
	{
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

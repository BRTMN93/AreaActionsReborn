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
#include "Buildables/FGBuildableBlueprintDesigner.h"
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
#include "AkComponent.h"
#include "UI/FGInteractWidget.h"
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
#include "AreaActionsSettings.h"
#include "UnrealClient.h"
#include "Components/LineBatchComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGSchematic.h"
#include "FGSchematicManager.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Containers/Ticker.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if !UE_SERVER
#include "FGServerManager.h"
#include "FGServerObject.h"
#endif
#if UE_SERVER
#include "FGServerSubsystem.h"
#endif

namespace
{
	enum class EStep : uint8 { Next, Wait };
	// Two game instances: who runs a step
	enum class ESide : uint8 { Each, Host, Client };

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
			if (bDry)
			{
				// A try that does not count (see Eventually)
				DryFails += bOk ? 0 : 1;
				return bOk;
			}
			++Total;
			Passed += bOk ? 1 : 0;
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s check %s %s"), *Name, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
			return bOk;
		}

		// Two game instances on one computer (AA-040), scenarios named MP_<name>: the host and the client (-AATestRole=client) hold the same
		// list of steps and each runs the steps marked for its side; at every change of side they wait for each other. The client's player
		// runs the action and the host builds and checks the world, unless bPeerActs is off (the host acts, the client watches).
		bool bMp = false;
		bool bClient = false;
		// A dedicated server in the host's place (AA-050): it has no player of its own
		bool bDedicated = false;
		bool bPeerActs = true;
		// The scenario's name without MP_ or MP_Host
		FString Scenario;
		// Checks made while this is on neither count nor log, only DryFails says how many would fail
		bool bDry = false;
		int32 DryFails = 0;
		// Client: buildings of the scene this game has not got from the host (see SpawnBuildable)
		int32 Missing = 0;
		// Host: the client's game left on purpose, nothing waits for it any more
		bool bPeerLeft = false;
		// While the steps are listed: the ones from here on come after the client has left (the host's own player acts)
		bool bBuildAfterLeave = false;
		TArray<TPair<int32, ESide>> Marks;
		// Host: the client's controller
		TWeakObjectPtr<APlayerController> Peer;
		bool bLinked = false;
		double Created = 0.0;
		double LastHello = 0.0;
		FDelegateHandle LinkHandle;
		// The acting player's place, the same numbers in both games: every scene is laid out from it
		FVector Origin = FVector::ZeroVector;
		// The last section the other game reported done, the sections this game reported, values the other game sent (by kind)
		int32 OtherDone = -1;
		TSet<int32> Sent;
		TMap<int32, int32> Shared;
		// Most mod windows, tool indicators and action actors alive in this game at once (AA-040 point 2)
		int32 MaxWindows = 0, MaxIndicators = 0, MaxActions = 0, MaxGhosts = 0;

		double Now() const { return FPlatformTime::Seconds(); }
		UWorld* GetWorld() const { return World.Get(); }
		// This game's own player; a dedicated server has none (its first controller is a client's)
		APlayerController* GetLocalPlayer() const { return World.IsValid() && !bDedicated ? UGameplayStatics::GetPlayerController(World.Get(), 0) : nullptr; }
		// The player the scenario is about: the one who has the tool and runs the action
		APlayerController* GetPlayer() const { return bMp && !bClient && bPeerActs ? Peer.Get() : GetLocalPlayer(); }
		// This game runs the action's windows
		bool Acts() const { return !bMp || bClient == bPeerActs; }
		// Where the acting player stands, the same in both games (the crates of an action land next to it)
		FVector ActingPlace() const { return bMp ? Origin : GetPlayer()->GetPawn()->GetActorLocation(); }
	};

	TUniquePtr<FRun> Run;

	// The steps added from here on belong to Side (two game instances; one game alone runs them all)
	void Mark(FRun& R, ESide Side)
	{
		R.Marks.Emplace(R.Steps.Num(), Side);
	}

	// Messages between the two games ride on the RCO's ping and its answer, which a game without the test mode has as well: 5 bits of
	// kind, 24 bits of value, one bit per direction (the server echoes every ping, and Smoke pings with plain numbers)
	constexpr int32 FromClient = 1 << 30, FromHost = 1 << 29;
	// Kinds 1 to 30 carry values (FRun::Shared)
	constexpr int32 MsgSection = 0, MsgHello = 31;
	constexpr int32 MsgOriginX = 1, MsgOriginY = 2, MsgOriginZ = 3;
	// Host to client: the number of items the result must name; the largest crate's stacks, items, contents hash and place
	constexpr int32 MsgExpected = 4, MsgCrateStacks = 5, MsgCrateItems = 6, MsgCrateHash = 7, MsgCrateX = 8, MsgCrateY = 9, MsgCrateZ = 10;
	// Client to host: what the client saw in that crate after opening it
	constexpr int32 MsgSeenStacks = 11, MsgSeenItems = 12, MsgSeenHash = 13;

	void Send(FRun& R, int32 Kind, int32 Value)
	{
		const int32 Token = (Kind << 24) | (Value & 0xFFFFFF);
		if (R.bClient)
		{
			if (UAreaActionsRCO* Rco = UAreaActionsRCO::Of(R.GetLocalPlayer()))
			{
				Rco->ServerPing(FromClient | Token);
			}
		}
		else if (UAreaActionsRCO* Rco = UAreaActionsRCO::Of(R.Peer.Get()))
		{
			Rco->ClientPong(FromHost | Token);
		}
	}

	void Receive(FRun& R, int32 Token)
	{
		const int32 Kind = (Token >> 24) & 31;
		int32 Value = Token & 0xFFFFFF;
		if (Value & 0x800000)
		{
			Value -= 0x1000000;
		}
		if (Kind == MsgSection)
		{
			R.OtherDone = FMath::Max(R.OtherDone, Value);
		}
		else if (Kind == MsgHello)
		{
			R.bLinked = true;
		}
		else
		{
			R.Shared.Add(Kind, Value);
		}
	}

	// Splits the steps by their marks: this game keeps its own, and at the end of every section tells the other game and waits for it
	void ApplySides(FRun& R)
	{
		TArray<FStepFn> Out;
		if (R.Marks.IsEmpty() || R.Marks[0].Key > 0)
		{
			R.Marks.Insert(TPair<int32, ESide>(0, ESide::Each), 0);
		}
		for (int32 Section = 0; Section < R.Marks.Num(); ++Section)
		{
			const int32 From = R.Marks[Section].Key;
			const int32 To = R.Marks.IsValidIndex(Section + 1) ? R.Marks[Section + 1].Key : R.Steps.Num();
			if (From == To)
			{
				continue;
			}
			const ESide Side = R.Marks[Section].Value;
			const bool bMine = Side == ESide::Each || (Side == ESide::Client) == R.bClient;
			const bool bOthers = Side == ESide::Each || (Side == ESide::Client) != R.bClient;
			for (int32 Index = From; bMine && Index < To; ++Index)
			{
				Out.Add(R.Steps[Index]);
			}
			Out.Add([Section, bMine, bOthers](FRun& Self)
			{
				if (bMine && !Self.Sent.Contains(Section))
				{
					Self.Sent.Add(Section);
					Send(Self, MsgSection, Section);
				}
				return bOthers && !Self.bPeerLeft && Self.OtherDone < Section ? EStep::Wait : EStep::Next;
			});
		}
		R.Steps = MoveTemp(Out);
	}

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

	FStepFn WaitSeconds(double Seconds)
	{
		return [Seconds, Since = MakeShared<double>(0.0)](FRun& Self)
		{
			*Since = *Since > 0.0 ? *Since : Self.Now();
			if (Self.Now() - *Since < Seconds)
			{
				return EStep::Wait;
			}
			*Since = 0.0;
			return EStep::Next;
		};
	}

	// Waits until the game has settled after a scene was built (streaming, first loads of meshes and materials): Frames frames in a row under
	// 50 ms, at most 25 s (the step timeout is 30 s), so W5 measures the job and not the scene's own hitches
	FStepFn WaitCalm(int32 Frames)
	{
		struct FState { int32 Calm = 0; double Start = 0.0; };
		return [Frames, State = MakeShared<FState>()](FRun& Self) mutable
		{
			if (State->Start == 0.0)
			{
				State->Start = FPlatformTime::Seconds();
				State->Calm = 0;
			}
			State->Calm = FApp::GetDeltaTime() < 0.05 ? State->Calm + 1 : 0;
			const double Waited = FPlatformTime::Seconds() - State->Start;
			if (State->Calm < Frames && Waited < 25.0)
			{
				return EStep::Wait;
			}
			Self.Step(FString::Printf(TEXT("game settled: %d calm frames after %.1f s"), State->Calm, Waited));
			State->Start = 0.0;
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

	// The side whose player has the tool and runs the action
	ESide ActSide(const FRun& R)
	{
		return R.bPeerActs && !R.bBuildAfterLeave ? ESide::Client : ESide::Host;
	}

	// One step of the host alone (the world's truth: building, inventories), or of both games; the steps after it are the acting player's again
	void AddHost(FRun& R, FStepFn Fn)
	{
		Mark(R, ESide::Host);
		R.Steps.Add(MoveTemp(Fn));
		Mark(R, ActSide(R));
	}

	void AddBoth(FRun& R, FStepFn Fn)
	{
		Mark(R, ESide::Each);
		R.Steps.Add(MoveTemp(Fn));
		Mark(R, ActSide(R));
	}

	// A step of checks alone (no side effects, always Next). The client's game sees the world through replication, which takes a moment:
	// there the checks are tried without counting until none fails, at most 15 s, and then count as they are
	FStepFn Eventually(FStepFn Fn)
	{
		return [Fn = MoveTemp(Fn), Since = MakeShared<double>(0.0)](FRun& Self)
		{
			if (!Self.bClient)
			{
				return Fn(Self);
			}
			*Since = *Since > 0.0 ? *Since : Self.Now();
			Self.bDry = true;
			Self.DryFails = 0;
			Fn(Self);
			Self.bDry = false;
			if (Self.DryFails > 0 && Self.Now() - *Since < 15.0)
			{
				return EStep::Wait;
			}
			Self.Step(FString::Printf(TEXT("replication waited for %.1f s"), Self.Now() - *Since));
			*Since = 0.0;
			return Fn(Self);
		};
	}

	// The checks after an action, in both games: each checks what it knows (Self.Acts(): the result's message; !Self.bClient: inventories)
	void AddChecks(FRun& R, FStepFn Fn)
	{
		AddBoth(R, Eventually(MoveTemp(Fn)));
	}

	// Two games: the host puts the acting player at the test place and the other one 40 m behind it (the scenes lie ahead, along +X),
	// and sends the place; the client waits until it stands where the host put it
	void AddMpPlace(FRun& R)
	{
		Mark(R, ESide::Host);
		R.Steps.Add([](FRun& Self)
		{
			AFGCharacterPlayer* Own = Self.GetLocalPlayer() ? Cast<AFGCharacterPlayer>(Self.GetLocalPlayer()->GetPawn()) : nullptr;
			if ((!Own && !Self.bDedicated) || !Self.Peer.IsValid() || !Cast<AFGCharacterPlayer>(Self.Peer->GetPawn()))
			{
				return EStep::Wait;
			}
			if (!Own)
			{
				// A dedicated server holds the whole map: the place is traced at once, the client's player goes there in the next step
				return EStep::Next;
			}
			// The host's own character goes first: the ground there streams in around it
			Self.Step(FString::Printf(TEXT("host to X=%.0f Y=%.0f from %s"), TestPlace.X, TestPlace.Y, *Own->GetActorLocation().ToCompactString()));
			Own->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Own->TeleportTo(FVector(TestPlace, 30000.0), FRotator::ZeroRotator);
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			FHitResult Hit, BehindHit;
			AFGCharacterPlayer* Own = Self.GetLocalPlayer() ? Cast<AFGCharacterPlayer>(Self.GetLocalPlayer()->GetPawn()) : nullptr;
			AFGCharacterPlayer* Theirs = Self.Peer.IsValid() ? Cast<AFGCharacterPlayer>(Self.Peer->GetPawn()) : nullptr;
			const FVector2D Behind(TestPlace.X - 4000.0, TestPlace.Y);
			if ((!Own && !Self.bDedicated) || !Theirs || !TraceGround(Self.GetWorld(), TestPlace, Hit) || !TraceGround(Self.GetWorld(), Behind, BehindHit))
			{
				return EStep::Wait;
			}
			const double Up = Theirs->GetSimpleCollisionHalfHeight() + 20.0;
			// Whole centimetres: the place travels as three numbers
			Self.Origin = FVector(TestPlace.X, TestPlace.Y, FMath::RoundToDouble(Hit.Location.Z + Up));
			AFGCharacterPlayer* Acting = Self.bPeerActs ? Theirs : Own;
			AFGCharacterPlayer* Watching = Self.bPeerActs ? Own : Theirs;
			if (Own)
			{
				Own->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
				Self.GetLocalPlayer()->SetControlRotation(FRotator::ZeroRotator);
			}
			if (Watching)
			{
				Watching->TeleportTo(BehindHit.Location + FVector(0, 0, Up), FRotator::ZeroRotator);
			}
			Acting->TeleportTo(Self.Origin, FRotator::ZeroRotator);
			Send(Self, MsgOriginX, FMath::RoundToInt32(Self.Origin.X));
			Send(Self, MsgOriginY, FMath::RoundToInt32(Self.Origin.Y));
			Send(Self, MsgOriginZ, FMath::RoundToInt32(Self.Origin.Z));
			Self.Check(true, FString::Printf(TEXT("place %s for the %s's player, the other one at %s"), *Self.Origin.ToCompactString(), Self.bPeerActs ? TEXT("client") : TEXT("host"),
				Watching ? *Watching->GetActorLocation().ToCompactString() : TEXT("none (dedicated server)")));
			return EStep::Next;
		});
		Mark(R, ESide::Client);
		R.Steps.Add([Calm = MakeShared<int32>(0)](FRun& Self)
		{
			Self.Origin = FVector(Self.Shared.FindRef(MsgOriginX), Self.Shared.FindRef(MsgOriginY), Self.Shared.FindRef(MsgOriginZ));
			const AFGCharacterPlayer* Own = GetCharacter(Self);
			const FVector Want = Self.Origin - FVector(Self.bPeerActs ? 0.0 : 4000.0, 0.0, 0.0);
			// Standing there on ground that has streamed in, for half a second
			const bool bThere = Own && FVector::Dist2D(Own->GetActorLocation(), Want) < 300.0 && Own->GetCharacterMovement()->IsMovingOnGround();
			*Calm = bThere ? *Calm + 1 : 0;
			if (*Calm < 30)
			{
				return EStep::Wait;
			}
			Self.GetLocalPlayer()->SetControlRotation(FRotator::ZeroRotator);
			Self.Check(true, FString::Printf(TEXT("place %s from the host, standing at %s"), *Self.Origin.ToCompactString(), *Own->GetActorLocation().ToCompactString()));
			return EStep::Next;
		});
		Mark(R, ESide::Each);
		R.Steps.Add(WaitFrames(30));
	}

	void AddPlace(FRun& R)
	{
		if (R.bMp)
		{
			AddMpPlace(R);
			return;
		}
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
		// The inventory is the server's
		Mark(R, ESide::Host);
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
			// A tool put away earlier is still in the slot, at the active index (setting it again does nothing): it moves to a free index
			for (int32 Index = 0; Index < Arms->GetSizeLinear(); ++Index)
			{
				FInventoryStack Stack;
				if (Arms->GetStackFromIndex(Index, Stack) && Stack.Item.GetItemClass() == Item)
				{
					Arms->RemoveAllFromIndex(Index);
					Self.Step(FString::Printf(TEXT("tool item taken from arms slot index %d"), Index));
				}
			}
			int32 Free = INDEX_NONE;
			for (int32 Index = 0; Index < Arms->GetSizeLinear() && Free == INDEX_NONE; ++Index)
			{
				Free = Arms->IsSomethingOnIndex(Index) || Index == Arms->GetActiveIndex() ? INDEX_NONE : Index;
			}
			const int32 Added = Free == INDEX_NONE ? 0 : Arms->AddStackToIndex(Free, FInventoryStack(1, Item));
			Self.Step(FString::Printf(TEXT("tool item put in arms slot index %d: %d"), Free, Added));
			Arms->SetActiveIndex(Free);
			return EStep::Next;
		});
		Mark(R, ActSide(R));
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
			// The tool's own window, made by its Blueprint when this game's player takes the tool
			const UUserWidget* Status = Cast<UUserWidget>(ObjectValue(GetTool(Self), TEXT("StatusWidget")));
			Self.Check(Status && Status->IsInViewport(), FString::Printf(TEXT("the tool's status window: %s"), *GetNameSafe(Status)));
			return EStep::Next;
		});
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
	void AddThreeCorners(FRun& R);

	void AddCorners(FRun& R)
	{
		AddEquipTool(R);
		AddThreeCorners(R);
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

	// Corners around a point in front of the player at the given angles (degrees, in click order), clicked in the corner mode
	void AddClickCorners(FRun& R, const TArray<double>& Angles);

	void AddThreeCorners(FRun& R)
	{
		AddClickCorners(R, {0.0, 120.0, 240.0});
	}

	// AA-074: corners clicked out of order around the area (a square's corners, opposite ones one after the other) used to join into an
	// X; the tool puts them in order, so no two walls cross
	void AddCornersOrder(FRun& R)
	{
		AddEquipTool(R);
		AddClickCorners(R, {45.0, 225.0, 135.0, 315.0});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			const FArrayProperty* Property = Tool ? FindFProperty<FArrayProperty>(Tool->GetClass(), TEXT("CornerLocations")) : nullptr;
			const TArray<FVector> Corners = Property ? *Property->ContainerPtrToValuePtr<TArray<FVector>>(Tool) : TArray<FVector>();
			Self.Check(Corners.Num() == 4, FString::Printf(TEXT("corners after 4 clicks: %d"), Corners.Num()));
			Self.Check(ArrayNum(Tool, TEXT("AreaWalls")) == 4 && ArrayNum(Tool, TEXT("CornerIndicators")) == 4,
				FString::Printf(TEXT("walls %d, corner indicators %d (want 4 and 4)"), ArrayNum(Tool, TEXT("AreaWalls")), ArrayNum(Tool, TEXT("CornerIndicators"))));
			int32 Crossings = 0;
			const int32 N = Corners.Num();
			for (int32 I = 0; I < N; ++I)
			{
				for (int32 J = I + 2; J < N; ++J)
				{
					if ((J + 1) % N == I)
					{
						continue;
					}
					FVector Cross;
					const FVector A(FVector2D(Corners[I]), 0.0), B(FVector2D(Corners[(I + 1) % N]), 0.0), C(FVector2D(Corners[J]), 0.0), D(FVector2D(Corners[(J + 1) % N]), 0.0);
					Crossings += FMath::SegmentIntersection2D(A, B, C, D, Cross) ? 1 : 0;
				}
			}
			FString Order;
			for (const FVector& Corner : Corners)
			{
				Order += Corner.ToCompactString() + TEXT(" ");
			}
			Self.Check(Crossings == 0, FString::Printf(TEXT("walls that cross: %d (want 0), corners %s"), Crossings, *Order));
			return EStep::Next;
		});
	}

	void AddClickCorners(FRun& R, const TArray<double>& Angles)
	{
		TSharedRef<TArray<FVector>> Targets = MakeShared<TArray<FVector>>();
		R.Steps.Add([Targets, Angles](FRun& Self)
		{
			APawn* Pawn = Self.GetPlayer()->GetPawn();
			const FVector Center = Pawn->GetActorLocation() + Pawn->GetActorForwardVector() * 1200.0;
			for (const double Angle : Angles)
			{
				const FVector Point = Center + FRotator(0, Angle, 0).RotateVector(FVector(500, 0, 0));
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
		for (int32 Index = 0; Index < Angles.Num(); ++Index)
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
		// Two games lay the scene out from the place the host sent, along +X
		const APawn* Pawn = R.bMp ? nullptr : R.GetPlayer()->GetPawn();
		const FVector At = Pawn ? Pawn->GetActorLocation() : R.Origin;
		const FVector Forward = Pawn ? Pawn->GetActorForwardVector().GetSafeNormal2D() : FVector::ForwardVector;
		const FVector Right = FVector(-Forward.Y, Forward.X, 0.0);
		S.HalfSize = HalfSize;
		S.Center = At + Forward * (HalfSize + Ahead) + Right * Side + FVector(0, 0, 300);
		S.Area.MinZ = At.Z - 3000.0;
		S.Area.MaxZ = At.Z + 3000.0;
		S.Corners.Reset();
		S.Area.Polygon.Reset();
		for (const FVector2D Sign : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(1, 1), FVector2D(-1, 1) })
		{
			const FVector Corner = S.Center + FVector(Sign.X * HalfSize, Sign.Y * HalfSize, 0);
			S.Corners.Add(Corner);
			S.Area.Polygon.Add(FVector2D(Corner));
		}
	}

	AFGBuildable* FindActor(UWorld* World, UClass* Class, const FVector& At, double Within = 1.0);
	int32 LightweightCount(UWorld* World);
	int32 ActorCount(UWorld* World);

	// Client: the world is the host's. A scene's Build runs in the client's game as well, and there every spawn is a search for the
	// building the host made at that place (5 cm: a replicated place is rounded); one not found counts in Missing
	template <typename T>
	T* FoundOnClient(FRun& R, T* Found, const TCHAR* What, const FVector& At)
	{
		if (!Found)
		{
			++R.Missing;
			R.Step(FString::Printf(TEXT("not replicated here: %s at %s"), What, *At.ToCompactString()));
		}
		return Found;
	}

	AFGBuildable* SpawnBuildable(FRun& R, const TCHAR* Path, const FVector& At)
	{
		UClass* Class = LoadClass<AFGBuildable>(nullptr, Path);
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(R.GetWorld());
		if (R.bClient)
		{
			return FoundOnClient(R, FindActor(R.GetWorld(), Class, At, 5.0), Path, At);
		}
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
	AFGBuildableManufacturer* SpawnMachine(FRun& R, const TCHAR* Path, const FVector& At, const TCHAR* RecipePath, int32 InItems, int32 OutItems)
	{
		AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(SpawnBuildable(R, Path, At));
		const TSubclassOf<UFGRecipe> With = Recipe(RecipePath);
		if (!Machine || !With || R.bClient)
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
		if (!R.bClient && Recipes && !Recipes->IsRecipeAvailable(Recipe(Path)))
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

	// AA-093: the crates hold the items packed: each crate's stacks sorted by type (a type's stacks next to each other), a type in one
	// crate unless it has more than 200 stacks, and as many crates as first-fit-decreasing packing needs (PackedCrates). Far is the
	// largest distance from Near
	struct FCratesSeen
	{
		AreaActionsBuildings::FItemCounts Items;
		int32 All = 0;
		int32 Types = 0;
		double Far = 0.0;
		bool bOneTypeEach = true;
		FString Text() const { return FString::Printf(TEXT("%d types, sorted and each type in one crate: %s, farthest %.0f cm"), Types, bOneTypeEach ? TEXT("yes") : TEXT("no"), Far); }
	};

	FCratesSeen SeeCrates(const TArray<AFGCrate*>& Crates, const FVector& Near)
	{
		FCratesSeen Seen;
		TMap<UClass*, TPair<int32, int32>> PerType;
		for (const AFGCrate* Crate : Crates)
		{
			TArray<FInventoryStack> Each;
			Crate->GetInventory()->GetInventoryStacks(Each);
			TSet<UClass*> Kinds;
			UClass* Last = nullptr;
			for (const FInventoryStack& Stack : Each)
			{
				UClass* Kind = Stack.Item.GetItemClass();
				// A type that comes back after another one is not sorted
				Seen.bOneTypeEach &= Kind == Last || !Kinds.Contains(Kind);
				Kinds.Add(Kind);
				Last = Kind;
				Seen.Items.FindOrAdd(Kind) += Stack.NumItems;
				Seen.All += Stack.NumItems;
				++PerType.FindOrAdd(Kind).Value;
			}
			for (UClass* Kind : Kinds)
			{
				++PerType.FindOrAdd(Kind).Key;
			}
			Seen.Far = FMath::Max(Seen.Far, FVector::Dist(Crate->GetActorLocation(), Near));
		}
		for (const TPair<UClass*, TPair<int32, int32>>& Entry : PerType)
		{
			// Crates holding the type: one, or for a type over 200 stacks its whole crates and at most one more with its rest
			Seen.bOneTypeEach &= Entry.Value.Key <= FMath::DivideAndRoundUp(Entry.Value.Value, 200);
		}
		Seen.Types = PerType.Num();
		return Seen;
	}

	// The crates the packing needs for these items: a type's whole crates of 200 stacks, then the rests by first-fit-decreasing
	int32 PackedCrates(const AreaActionsBuildings::FItemCounts& Items)
	{
		int32 Whole = 0;
		TArray<int32> Rests;
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Items)
		{
			if (Entry.Value <= 0)
			{
				continue;
			}
			const int32 Stacks = FMath::DivideAndRoundUp(Entry.Value, FMath::Max(1, UFGItemDescriptor::GetStackSize(Entry.Key)));
			Whole += Stacks / 200;
			if (Stacks % 200 > 0)
			{
				Rests.Add(Stacks % 200);
			}
		}
		Rests.Sort(TGreater<int32>());
		TArray<int32> Bins;
		for (const int32 Rest : Rests)
		{
			int32* Into = Bins.FindByPredicate([Rest](int32 Used) { return Used + Rest <= 200; });
			if (Into)
			{
				*Into += Rest;
			}
			else
			{
				Bins.Add(Rest);
			}
		}
		return Whole + Bins.Num();
	}

	// The result's sentence about the crates next to the player
	FString CratesNextToYou(int32 Crates)
	{
		return Crates == 1 ? FString(TEXT("The items are in 1 crate next to you.")) : FString::Printf(TEXT("The items are in %d crates next to you."), Crates);
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
		// -AATestBreakLayout: the checks measure a list that is never drawn (no size, no rows), so they must fail after the wait (AA-094)
		static const auto GetList = [](UUserWidget* Window) -> UListView*
		{
			if (Window && FParse::Param(FCommandLine::Get(), TEXT("AATestBreakLayout")))
			{
				return NewObject<UListView>(Window);
			}
			return Window ? Cast<UListView>(Window->GetWidgetFromName(TEXT("RecipeList"))) : nullptr;
		};
		// The window is laid out some frames after it opens, more under load (AA-094): wait for a list with a size and a row, at most 2 s;
		// after that the checks below run on whatever is there
		R.Steps.Add([Since = MakeShared<double>(0.0)](FRun& Self)
		{
			if (*Since == 0.0)
			{
				*Since = Self.Now();
			}
			UListView* List = GetList(TopWindow(Self));
			const bool bLaidOut = List && List->GetCachedGeometry().GetAbsoluteSize().X > 0.0 && List->GetDisplayedEntryWidgets().Num() > 0;
			if (!bLaidOut && Self.Now() - *Since < 2.0)
			{
				return EStep::Wait;
			}
			// A minimized game window is not laid out at all: said here, so such a run is told apart
			const TSharedPtr<SWindow> GameWindow = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWindow() : nullptr;
			Self.Step(FString::Printf(TEXT("recipe window laid out: %s after %.2f s (game window minimized %d, visible %d)"), bLaidOut ? TEXT("yes") : TEXT("no"), Self.Now() - *Since,
				GameWindow.IsValid() && GameWindow->IsWindowMinimized(), GameWindow.IsValid() && GameWindow->IsVisible()));
			*Since = 0.0;
			return EStep::Next;
		});
		// One more frame: the rows' own text boxes get their size a pass after the rows appear
		R.Steps.Add(WaitFrames(2));
		R.Steps.Add([](FRun& Self)
		{
			UListView* List = GetList(TopWindow(Self));
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

	// A crate with items standing at a place; the client finds the host's crate there (it settles on the ground: 5 m)
	AFGCrate* SpawnCrate(FRun& R, const TArray<FInventoryStack>& Stacks, const FVector& At)
	{
		if (R.bClient)
		{
			AFGCrate* Nearest = nullptr;
			for (TActorIterator<AFGCrate> It(R.GetWorld()); It; ++It)
			{
				if (IsValid(*It) && FVector::Dist(It->GetActorLocation(), At) < (Nearest ? FVector::Dist(Nearest->GetActorLocation(), At) : 500.0))
				{
					Nearest = *It;
				}
			}
			return FoundOnClient(R, Nearest, TEXT("crate"), At);
		}
		FSpawnInventoryCrateParameters Parameters;
		Parameters.bAllowCrateMerging = false;
		return AFGItemPickup_Spawnable::SpawnInventoryCrate(R.GetWorld(), Stacks, At, Parameters);
	}

	// Client: waits until the host's scene has arrived: the numbers of buildings and crates here stand still for 2 s after they changed
	// (5 s when they never change), at most 20 s
	FStepFn WaitReplicated()
	{
		struct FState { double Start = 0.0, Changed = 0.0; int32 Last = -1; bool bMoved = false; };
		return [State = MakeShared<FState>()](FRun& Self)
		{
			int32 Count = ActorCount(Self.GetWorld()) + LightweightCount(Self.GetWorld());
			for (TActorIterator<AFGCrate> It(Self.GetWorld()); It; ++It)
			{
				++Count;
			}
			if (!Self.bClient)
			{
				return EStep::Next;
			}
			const double Now = Self.Now();
			if (State->Start == 0.0)
			{
				*State = FState{ Now, Now, Count, false };
			}
			if (Count != State->Last)
			{
				*State = FState{ State->Start, Now, Count, true };
			}
			if (Now - State->Changed < (State->bMoved ? 2.0 : 5.0) && Now - State->Start < 20.0)
			{
				return EStep::Wait;
			}
			Self.Step(FString::Printf(TEXT("the host's scene arrived: %d buildings and crates here after %.1f s"), Count, Now - State->Start));
			State->Start = 0.0;
			return EStep::Next;
		};
	}

	// Scene step: what Build puts in the area, then the tool's area, the crates before, the results listener
	void AddScene(FRun& R, const FSceneRef& S, double HalfSize, TFunction<void(FRun&, FScene&)> Build, double Side = 0.0)
	{
		AddEquipTool(R);
		// Two games: both know where the scene is, the host builds it, the client finds the host's buildings in its own world (the same
		// Build, see SpawnBuildable), the acting player's game sets the tool
		Mark(R, ESide::Each);
		R.Steps.Add([S, HalfSize, Side](FRun& Self)
		{
			PlaceScene(Self, *S, HalfSize, 1000.0, Side);
			return EStep::Next;
		});
		Mark(R, ESide::Host);
		R.Steps.Add([S, Build](FRun& Self)
		{
			Build(Self, *S);
			Self.Check(S->Inside.Num() > 0 && AllValid(S->Inside) && S->Controls.Num() == 3 && AllValid(S->Controls),
				FString::Printf(TEXT("scene: %d in the area, %d controls"), S->Inside.Num(), S->Controls.Num()));
			SnapshotCrates(Self, *S);
			S->RejectsBefore = UAreaActionsRCO::RejectCount;
			return EStep::Next;
		});
		Mark(R, ESide::Client);
		R.Steps.Add(WaitReplicated());
		R.Steps.Add([S, Build](FRun& Self)
		{
			if (!Self.bClient)
			{
				// One game alone runs every step
				return EStep::Next;
			}
			// The checks inside Build are the host's
			Self.Missing = 0;
			Self.bDry = true;
			Build(Self, *S);
			Self.bDry = false;
			Self.Check(Self.Missing == 0 && S->Inside.Num() > 0 && AllValid(S->Inside) && S->Controls.Num() == 3 && AllValid(S->Controls),
				FString::Printf(TEXT("the host's scene here: %d in the area, %d controls, %d not replicated"), S->Inside.Num(), S->Controls.Num(), Self.Missing));
			SnapshotCrates(Self, *S);
			return EStep::Next;
		});
		Mark(R, ActSide(R));
		R.Steps.Add([S](FRun& Self)
		{
			Self.Check(SetToolArea(Self, *S), TEXT("tool area set"));
			ListenResults(Self, S);
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
			// The client's game sees the recipe; what an inventory holds is the host's to check
			R.Check(Control && Control->GetCurrentRecipe() == Recipe(RecipePath) && (R.bClient || MachineItems(Control) == Items),
				FString::Printf(TEXT("control %d: recipe %s, items %d"), Index, Control ? *GetNameSafe(Control->GetCurrentRecipe()) : TEXT("-"), R.bClient ? -1 : MachineItems(Control)));
		}
	}

	void AddControlMachines(FRun& R, FScene& S, const TCHAR* RecipePath, int32 In, int32 Out)
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
		AddChecks(R, [S](FRun& Self)
		{
			if (Self.Acts())
			{
				Self.Check(S->Message.Contains(TEXT("3 Constructor")), FString::Printf(TEXT("result names 3 Constructors: '%s'"), *S->Message));
			}
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const AFGBuildableManufacturer* Constructor = Machine(*S, Index);
				Self.Check(Constructor && Constructor->GetCurrentRecipe() == Recipe(Asset::IronRod) && (Self.bClient || MachineItems(Constructor) == 0),
					FString::Printf(TEXT("Constructor %d: recipe %s, items %d"), Index, Constructor ? *GetNameSafe(Constructor->GetCurrentRecipe()) : TEXT("-"),
						Self.bClient ? -1 : MachineItems(Constructor)));
			}
			const AFGBuildableManufacturer* Assembler = Machine(*S, 3);
			Self.Check(Assembler && Assembler->GetCurrentRecipe() == Recipe(Asset::ReinforcedPlate), TEXT("Assembler keeps its recipe"));
			CheckControlMachines(Self, *S, Asset::IronPlate, 6);
			// D8: one crate with exactly the machines' items, sorted (AA-093)
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			const FCratesSeen Seen = SeeCrates(Crates, Self.ActingPlace());
			if (Self.bClient)
			{
				Self.Check(Crates.Num() == 1, FString::Printf(TEXT("new crates here %d (want 1)"), Crates.Num()));
			}
			else
			{
				const int32 Ingots = Seen.Items.FindRef(Item(Asset::IronIngotItem));
				const int32 Plates = Seen.Items.FindRef(Item(Asset::IronPlateItem));
				Self.Check(Crates.Num() == 1 && Seen.bOneTypeEach && Ingots == 37 && Plates == 9 && Seen.All == 46,
					FString::Printf(TEXT("new crates %d (%s); ingots %d of 37, plates %d of 9, all %d of 46"), Crates.Num(), *Seen.Text(), Ingots, Plates, Seen.All));
			}
			Self.Check(!Crates.IsEmpty() && Seen.Far < 1500.0, FString::Printf(TEXT("crates next to the player: %.0f cm"), Seen.Far));
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

	AFGBuildableStorage* SpawnStorage(FRun& R, const FVector& At, int32 Plates, int32 Ingots)
	{
		AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(SpawnBuildable(R, Asset::Storage, At));
		if (Storage && !R.bClient)
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
			UFGInventoryComponent* Input = Constructor && !Self.bClient ? Constructor->GetInputInventory() : nullptr;
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
			UFGInventoryComponent* Upgrades = Constructor && !Self.bClient ? Constructor->GetPotentialInventory() : nullptr;
			int32 Shards = 0;
			if (Upgrades && Upgrades->GetSizeLinear() > 0)
			{
				const FItemFilter Filter = Upgrades->mItemFilter;
				Upgrades->mItemFilter.Unbind();
				Shards = Upgrades->AddStackToIndex(0, FInventoryStack(1, Item(Asset::PowerShard)), true);
				Upgrades->mItemFilter = Filter;
			}
			Self.Check(Shards == 1, FString::Printf(TEXT("power shard in the Constructor's upgrade slot: %d (slots %d)"), Shards, Upgrades ? Upgrades->GetSizeLinear() : -1));
			*Crate = SpawnCrate(Self, { FInventoryStack(25, Item(Asset::IronRodItem)) }, Scene.Slot(5));
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
		AddChecks(R, [S, Crate, bToCrate](FRun& Self)
		{
			if (Self.Acts())
			{
				const FString Want = FString::Printf(TEXT("Cleared 4 buildings %s a total of %d items"), bToCrate ? TEXT("taking") : TEXT("deleting"), S->Expected);
				Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
				Self.Check(S->Message.Contains(TEXT("2 m³ of fluids were deleted")), FString::Printf(TEXT("result '%s' names 2 m³ of fluids"), *S->Message));
			}
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			if (Self.bClient)
			{
				// What the inventories hold is the host's to check; the client's game sees the buildings and the crates
				Self.Check(AllValid(S->Inside) && AllValid(S->Controls) && Crates.Num() == (bToCrate ? 1 : 0),
					FString::Printf(TEXT("here: buildings and controls stand, new crates %d (want %d)"), Crates.Num(), bToCrate ? 1 : 0));
				return EStep::Next;
			}
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
			if (!bToCrate)
			{
				Self.Check(Crates.IsEmpty(), FString::Printf(TEXT("no new crate: %d"), Crates.Num()));
				return EStep::Next;
			}
			// Plates 50 + 20 + 5, ingots 30, rods 25: every solid item of the area, no water, nothing more; one crate, sorted (AA-093)
			const FCratesSeen Seen = SeeCrates(Crates, Self.ActingPlace());
			const int32 Plates = Seen.Items.FindRef(Item(Asset::IronPlateItem));
			const int32 Ingots = Seen.Items.FindRef(Item(Asset::IronIngotItem));
			const int32 Rods = Seen.Items.FindRef(Item(Asset::IronRodItem));
			Self.Check(Crates.Num() == 1 && Seen.bOneTypeEach && Plates == 75 && Ingots == 30 && Rods == 25 && Seen.All == S->Expected,
				FString::Printf(TEXT("new crates %d (%s); plates %d of 75, ingots %d of 30, rods %d of 25, all %d of %d"), Crates.Num(), *Seen.Text(), Plates, Ingots, Rods,
					Seen.All, S->Expected));
			Self.Check(!Crates.IsEmpty() && Seen.Far < 1500.0, FString::Printf(TEXT("crates next to the player: %.0f cm"), Seen.Far));
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

	void AddControlStations(FRun& R, FScene& S)
	{
		for (const FVector& Place : S.ControlPlaces())
		{
			AFGBuildable* Station = SpawnBuildable(R, Asset::TruckStation, Place);
			SetLoadMode(R.bClient ? nullptr : Station, false);
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
		AddChecks(R, [S, bLoad](FRun& Self)
		{
			// AA-081: one space after the comma
			const FString Want = FString::Printf(TEXT("Set 1 Truck Stations, 1 Freight Platforms to %s"), bLoad ? TEXT("Load") : TEXT("Unload"));
			if (Self.Acts())
			{
				Self.Check(S->Message.Equals(Want), FString::Printf(TEXT("result '%s' is '%s'"), *S->Message, *Want));
			}
			for (int32 Index = 0; Index < S->Inside.Num(); ++Index)
			{
				const AFGBuildable* Station = S->Inside[Index].Get();
				Self.Check(Station && LoadMode(Station) == bLoad, FString::Printf(TEXT("%s load mode %d"), *GetNameSafe(Station), Station ? LoadMode(Station) : -1));
			}
			CheckControlStations(Self, *S);
			return EStep::Next;
		});
	}

	void AddSetStation(FRun& R, double Side = 0.0)
	{
		FSceneRef S = MakeShared<FScene>();
		AddScene(R, S, 4000.0, [](FRun& Self, FScene& Scene)
		{
			Scene.Inside.Add(SpawnBuildable(Self, Asset::TruckStation, Scene.Slot(0)));
			Scene.Inside.Add(SpawnBuildable(Self, Asset::FreightPlatform, Scene.Slot(6)));
			for (const TWeakObjectPtr<AFGBuildable>& Station : Scene.Inside)
			{
				SetLoadMode(Self.bClient ? nullptr : Station.Get(), true);
			}
			AddControlStations(Self, Scene);
		}, Side);
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
		// No tool in hand: the tool's item leaves the arms slot (the inventory is the server's)
		AddHost(R, [S](FRun& Self)
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
		AddHost(R, [S](FRun& Self)
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
			return EStep::Next;
		});
		// AA-092 (3): "Cost only" with No Build Cost on; the flag is the server's, the request carries none
		AddHost(R, [](FRun& Self)
		{
			if (AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>())
			{
				State->SetCheatNoCost(true);
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([S](FRun& Self)
		{
			Self.Step(TEXT("request 107: MassDismantle cost only with No Build Cost on"));
			UAreaActionsRCO::Of(Self.GetPlayer())->ServerDismantle(107, S->Area, EAreaActionsDismantleRefund::CostOnly);
			return EStep::Next;
		});
		R.Steps.Add([S](FRun& Self)
		{
			if (S->Results.Num() < 7)
			{
				return EStep::Wait;
			}
			const FAreaActionsResult* Result = S->Results.Find(107);
			Self.Check(Result && Result->Code == EAreaActionsResultCode::Rejected, FString::Printf(TEXT("request 107: code %d (Rejected)"), Result ? static_cast<int32>(Result->Code) : -1));
			return EStep::Next;
		});
		AddHost(R, [](FRun& Self)
		{
			if (AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>())
			{
				State->SetCheatNoCost(false);
			}
			return EStep::Next;
		});
		AddChecks(R, [S](FRun& Self)
		{
			if (!Self.bClient)
			{
				// The reject lines are in the server's log
				const int32 Rejects = UAreaActionsRCO::RejectCount - S->RejectsBefore;
				Self.Check(Rejects == 6, FString::Printf(TEXT("reject lines: %d of 6"), Rejects));
			}
			const AFGBuildableManufacturer* Constructor = Machine(*S, 0);
			Self.Check(Constructor && Constructor->GetCurrentRecipe() == Recipe(Asset::IronPlate) && (Self.bClient || MachineItems(Constructor) == 10), TEXT("Constructor in the area unchanged"));
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

	AFGBuildable* FindActor(UWorld* World, UClass* Class, const FVector& At, double Within)
	{
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
		if (!Buildables)
		{
			return nullptr;
		}
		for (AFGBuildable* Actor : Buildables->GetAllBuildablesRef())
		{
			if (IsValid(Actor) && Actor->GetClass() == Class && FVector::Dist(Actor->GetActorLocation(), At) <= Within)
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
		// Two games: the items of the player who does not act (AA-040 point 3: the cost is the acting player's)
		int32 OtherItems = -1;
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
		if (R.bMp)
		{
			// Two games must get the same number: the test place is flat, the acting player stands about 110 cm above its ground
			return FVector(At.X, At.Y, R.Origin.Z - 60.0);
		}
		FVector Hit;
		return GroundBelow(R.GetWorld(), At, Hit) ? Hit + FVector(0, 0, 50) : At;
	}

	void AddLightweight(FRun& R, FCopyState& C, const TCHAR* ClassName, const TCHAR* RecipeName, const FTransform& Transform, float Length = -1.0f)
	{
		AreaActionsBuildings::FSnapshot Building;
		Building.Class = GameClass(ClassName);
		Building.BuiltWith = RecipeOfClass(R, Building.Class, RecipeName);
		if (R.bClient)
		{
			// The host built it (see SpawnBuildable)
			FoundOnClient(R, FindLightweight(R.GetWorld(), Building.Class, Transform.GetLocation()), ClassName, Transform.GetLocation());
			C.Pattern.Add(MakePiece(Building.Class, Transform, true, Length, Building.BuiltWith));
			return;
		}
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
		if (R.bClient)
		{
			// The host built it (see SpawnBuildable); nullptr for the caller, so nothing is set on it here
			if (AFGBuildable* Found = FoundOnClient(R, FindActor(R.GetWorld(), Class, Transform.GetLocation(), 5.0), ClassName, Transform.GetLocation()))
			{
				S.Inside.Add(Found);
			}
			C.Pattern.Add(MakePiece(Class, Transform, false, -1.0f, RecipeOfClass(R, Class, RecipeName)));
			return nullptr;
		}
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

	void AddFillWindow(FRun& R, const FVector& BorderMetres, const FIntVector& Amount, const FVector2D& RampMetres, bool bUseCost, const TCHAR* Button = TEXT("OkButton"))
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
			Self.Check(PressButton(Window, Button), FString::Printf(TEXT("Fill: %s with gap %s m, amount %s, ramp %s m, cost %d"), Button, *BorderMetres.ToCompactString(), *Amount.ToString(),
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

	// Host of two games: all items of the player who does not act; -1 with one game
	int32 OtherItems(const FRun& R)
	{
		const APlayerController* Other = R.bMp && !R.bClient ? (R.bPeerActs ? R.GetLocalPlayer() : R.Peer.Get()) : nullptr;
		AFGCharacterPlayer* Character = Other ? Cast<AFGCharacterPlayer>(Other->GetPawn()) : nullptr;
		return Character ? Character->GetInventory()->GetNumItems(nullptr) : -1;
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
		C.OtherItems = OtherItems(R);
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
		if (R.bMp)
		{
			R.Check(OtherItems(R) == C.OtherItems, FString::Printf(TEXT("%s: the other player's inventory %d items (were %d)"), When, OtherItems(R), C.OtherItems));
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
		AddBoth(R, [C](FRun& Self)
		{
			CountWorld(Self, *C);
			C->JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			return EStep::Next;
		});
	}

	// One Copy through the window and its result window
	void AddCopy(FRun& R, const FSceneRef& S, const FCopyRef& C, const FVector& DeltaMetres, double Yaw, bool bUseCost)
	{
		AddBoth(R, [C, DeltaMetres, Yaw](FRun& Self)
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
		AddChecks(R, [S, C](FRun& Self)
		{
			CheckCopies(Self, *C, true, TEXT("copied"));
			CheckPattern(Self, *S, *C, TEXT("copied"));
			if (!Self.bClient)
			{
				Self.Check(AAreaActionsServerSubsystem::FinishedJobs - C->JobsBefore == 1, TEXT("one AAJOB line"));
			}
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
		AddHost(R, [C](FRun& Self) { SetCostSources(Self, *C, CostOne, CostAllButOne, CostAll, CostNone); return EStep::Next; });
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, true);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		AddChecks(R, [C](FRun& Self)
		{
			if (!Self.bClient)
			{
				CheckSources(Self, *C, CostOne, CostAllButOne, CostNone, TEXT("paid"));
			}
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
		AddChecks(R, [C, bNegative, Ramp](FRun& Self)
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
		AddBoth(R, [C](FRun& Self) { C->Delta = FVector(2000, 0, 0); C->Yaw = 0.0; return EStep::Next; });
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
		TArray<FVector> CratePlaces;
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
		D.AreaCrate = SpawnCrate(R, { FInventoryStack(25, Item(Asset::IronRodItem)) }, C.Base + FVector(2500, 1500, 100));
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
		// (b) for the cost: each actor's own cost times the game's multiplier (GetDismantleRefundReturns), the lightweights' as in (a)
		FCounts CostOnly;
		for (const TWeakObjectPtr<AFGBuildable>& Weak : S.Inside)
		{
			if (AFGBuildable* Actor = Weak.Get())
			{
				TArray<FInventoryStack> Refund, WithoutCost;
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Refund, D.bNoCost);
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, WithoutCost, true);
				FCounts Own;
				AddSolids(Own, Refund);
				// The game's refund of a building includes the wires on its power connections; every wire of the scene is in it on its own,
				// so it is taken out of the buildings it hangs on and counted once
				TInlineComponentArray<UFGCircuitConnectionComponent*> Connections(Actor);
				for (const UFGCircuitConnectionComponent* Connection : Connections)
				{
					TArray<AFGBuildableWire*> Wires;
					Connection->GetWires(Wires);
					for (AFGBuildableWire* Wire : Wires)
					{
						TArray<FInventoryStack> WireRefund;
						IFGDismantleInterface::Execute_GetDismantleRefund(Wire, WireRefund, D.bNoCost);
						FCounts Hanging;
						AddSolids(Hanging, WireRefund);
						for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Hanging)
						{
							Own.FindOrAdd(Entry.Key) -= Entry.Value;
						}
					}
				}
				TArray<FInventoryStack> Cost;
				if (!D.bNoCost)
				{
					Actor->GetDismantleRefundReturns(Cost);
					AddSolids(CostOnly, Cost);
				}
				R.Step(FString::Printf(TEXT("refund of %s: game %s, wires taken out %s; cost %s"), *Actor->GetClass()->GetName(), *StacksText(Refund),
					*ItemsText(Own), *StacksText(Cost)));
				for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Own)
				{
					D.Expected.FindOrAdd(Entry.Key) += Entry.Value;
				}
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
				AddSolids(CostOnly, Refund);
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
		FCounts CostAndContents = CostOnly;
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : D.Contents)
		{
			CostAndContents.FindOrAdd(Entry.Key) += Entry.Value;
		}
		R.Check(SameCounts(D.Expected, CostAndContents), FString::Printf(TEXT("(a) %s equals (b) cost and contents %s"), *ItemsText(D.Expected),
			*ItemsText(CostAndContents)));
		R.Step(FString::Printf(TEXT("expected refund %s (%d items)"), *ItemsText(D.Expected), Total(D.Expected)));
	}

	void AddDismantleScene(FRun& R, const FSceneRef& S, const FDismantleRef& D, double Side = 0.0)
	{
		AddScene(R, S, 4000.0, [D](FRun& Self, FScene& Scene) { BuildDismantleBasic(Self, Scene, *D); }, Side);
		AddBoth(R, [S, D](FRun& Self)
		{
			if (!Self.bClient)
			{
				// The refunds are the server's to read; the client's result message must name their sum
				RecordRefund(Self, *S, *D);
				Send(Self, MsgExpected, Total(D->Expected));
			}
			CountWorld(Self, D->Pieces);
			D->Pieces.JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			return EStep::Next;
		});
	}

	// The menu, the question answered with one of its buttons (YesButton: "Refund to crate", or "OK" with No Build Cost; NoButton: "No
	// refund"; ChoiceButton2: "Cost only", AA-092), the result
	void AddDismantle(FRun& R, const FSceneRef& S, const TCHAR* ButtonName, const TCHAR* Want, bool bResult = true)
	{
		AddOpenAction(R, TEXT("MassDismantle"));
		AddOnWindow(R, TEXT("Widget_MessageYesNo"), [ButtonName, Want](FRun& Self, UUserWidget* Window)
		{
			// The answers are named after what they do (D16)
			UButton* Button = Cast<UButton>(Window->GetWidgetFromName(ButtonName));
			FString Label;
			if (Button)
			{
				UWidgetTree::ForWidgetAndChildren(Button, [&Label](UWidget* Child)
				{
					if (const UTextBlock* Text = Cast<UTextBlock>(Child))
					{
						Label = Text->GetText().ToString();
					}
				});
			}
			Self.Check(Label == Want, FString::Printf(TEXT("button %s reads '%s' (want '%s')"), ButtonName, *Label, Want));
			Self.Check(PressButton(Window, ButtonName), FString::Printf(TEXT("'%s' pressed"), Want));
		});
		if (bResult)
		{
			AddResultWindow(R, S);
		}
	}

	// Every building of the scene is gone, actors after their dismantle effect, the controls stay with their items
	void AddCheckGone(FRun& R, const FSceneRef& S, const FDismantleRef& D)
	{
		TSharedRef<double> Since = MakeShared<double>(0.0);
		AddBoth(R, [S, D, Since](FRun& Self)
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
				// The client's game sees the container; what it holds is the host's to check
				Self.Check(S->Controls[Index].IsValid() && (Self.bClient || StorageItems(S->Controls[Index].Get()) == 15),
					FString::Printf(TEXT("control container %d (W4): %d items"), Index, Self.bClient ? -1 : StorageItems(S->Controls[Index].Get())));
			}
			return EStep::Next;
		});
	}

	// New crates next to the player, packed (AA-093), with exactly the expected refund
	void AddCheckRefundCrate(FRun& R, const FSceneRef& S, const FDismantleRef& D)
	{
		AddChecks(R, [S, D](FRun& Self)
		{
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			const FCratesSeen Seen = SeeCrates(Crates, Self.ActingPlace());
			const FCounts& InCrate = Seen.Items;
			if (!Self.bClient)
			{
				Self.Check(Crates.Num() == PackedCrates(D->Expected) && Seen.bOneTypeEach && SameCounts(InCrate, D->Expected), FString::Printf(TEXT("new crates %d (%s); in them %s (%d), expected %s (%d)"),
					Crates.Num(), *Seen.Text(), *ItemsText(InCrate), Total(InCrate), *ItemsText(D->Expected), Total(D->Expected)));
			}
			Self.Check(!Crates.IsEmpty() && Seen.Far < 1500.0, FString::Printf(TEXT("crates next to the player: %.0f cm"), Seen.Far));
			D->CrateItems = Total(InCrate);
			D->CratePlaces.Reset();
			for (const AFGCrate* Crate : Crates)
			{
				D->CratePlaces.Add(Crate->GetActorLocation());
			}
			if (Self.Acts())
			{
				// The client's game has the sum of the refunds from the host
				const int32 Refunded = Self.bClient ? Self.Shared.FindRef(MsgExpected) : Total(D->Expected);
				const FString Want = FString::Printf(TEXT("Dismantled %d buildings. %d items were refunded. %s"), D->Buildings, Refunded, *CratesNextToYou(Crates.Num()));
				Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			}
			if (!Self.bClient)
			{
				Self.Check(AAreaActionsServerSubsystem::FinishedJobs - D->Pieces.JobsBefore == 1, TEXT("one AAJOB line"));
			}
			return EStep::Next;
		});
	}

	void AddDismantleBasic(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		AddDismantleScene(R, S, D);
		AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"));
		AddCheckGone(R, S, D);
		AddCheckRefundCrate(R, S, D);
	}

	// AA-030 pt 1, the user's leftovers: buildings the first pass does not take (here two built in the area after the collection) go
	// with the sweeps; the job runs one building per frame so they come while it runs
	void AddDismantleSweep(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		struct FLate { TWeakObjectPtr<AFGBuildableStorage> Storage; FVector Foundation = FVector::ZeroVector; TSubclassOf<AFGBuildable> Class; };
		TSharedRef<FLate> Late = MakeShared<FLate>();
		R.Steps.Add([](FRun& Self)
		{
			SetBudget(0.0f);
			return EStep::Next;
		});
		AddDismantleScene(R, S, D);
		AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"), false);
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([S, D, Late](FRun& Self)
		{
			const FVector Base = D->Pieces.Base;
			Late->Storage = SpawnStorage(Self, Base + FVector(1200.0, 2500.0, 50.0), 5, 0);
			AreaActionsBuildings::FSnapshot Foundation;
			Foundation.Class = GameClass(TEXT("Build_Foundation_8x1_01"));
			Foundation.BuiltWith = RecipeOfClass(Self, Foundation.Class, TEXT("Recipe_Foundation_8x1_01"));
			Late->Class = Foundation.Class;
			Late->Foundation = Base + FVector(-1200.0, 2800.0, 0.0);
			const bool bFoundation = Foundation.Class && AreaActionsBuildings::Spawn(Self.GetWorld(), Foundation, FTransform(Late->Foundation));
			const bool bRunning = AAreaActionsServerSubsystem::FinishedJobs == D->Pieces.JobsBefore;
			Self.Check(Late->Storage.IsValid() && bFoundation && S->Area.Contains(Late->Storage->GetActorLocation()) && S->Area.Contains(Late->Foundation) && bRunning,
				FString::Printf(TEXT("a container and a foundation built in the area while the job runs (job running %d)"), bRunning));
			return EStep::Next;
		});
		AddResultWindow(R, S);
		AddCheckGone(R, S, D);
		TSharedRef<double> Since = MakeShared<double>(0.0);
		R.Steps.Add([S, D, Late, Since](FRun& Self)
		{
			if (*Since == 0.0)
			{
				*Since = FPlatformTime::Seconds();
			}
			const bool bStorageGone = !Late->Storage.IsValid() || Late->Storage->GetIsDismantled();
			const bool bFoundationGone = FindLightweight(Self.GetWorld(), Late->Class, Late->Foundation) == nullptr;
			if ((!bStorageGone || !bFoundationGone) && FPlatformTime::Seconds() - *Since < 3.0)
			{
				return EStep::Wait;
			}
			SetBudget(2.0f);
			Self.Check(bStorageGone && bFoundationGone, FString::Printf(TEXT("the late container gone %d, the late foundation gone %d"), bStorageGone, bFoundationGone));
			const FString Want = FString::Printf(TEXT("Dismantled %d buildings."), D->Buildings + 2);
			Self.Check(S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			return EStep::Next;
		});
	}

	void AddDismantleNoCrate(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		AddDismantleScene(R, S, D);
		AddDismantle(R, S, TEXT("NoButton"), TEXT("No refund"));
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
		AddDismantle(R, S, TEXT("YesButton"), TEXT("OK"));
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

	// The HUB and the Space Elevator, and a blueprint designer with a building inside (which the game refuses to dismantle): CanDismantle
	// of each, read right before the request, decides what goes. The designer's building stands outside the polygon (its place is), inside
	// the designer's floor.
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
				FWhat{ TEXT("Build_BlueprintDesigner"), TEXT("Recipe_BlueprintDesigner"), FVector(-3000, Scene.HalfSize - 500.0, 0) } })
			{
				AFGBuildable* Building = SpawnGameBuildable(Self, What.Class, What.Recipe, Base + What.Offset);
				Self.Step(FString::Printf(TEXT("%s spawned: %d"), What.Class, Building != nullptr));
				if (Building)
				{
					P->Buildings.Add(Building);
					Scene.Inside.Add(Building);
				}
			}
			AFGBuildableBlueprintDesigner* Designer = P->Buildings.IsEmpty() ? nullptr : Cast<AFGBuildableBlueprintDesigner>(P->Buildings.Last().Get());
			UClass* MachineClass = LoadClass<AFGBuildable>(nullptr, Asset::Constructor);
			AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(Self.GetWorld());
			const FTransform Inner(Base + FVector(-3000, Scene.HalfSize + 500.0, 100));
			AFGBuildable* Machine = Designer && MachineClass ? Buildables->BeginSpawnBuildable(MachineClass, Inner) : nullptr;
			if (Machine)
			{
				Machine->SetInsideBlueprintDesigner(Designer);
				Machine->FinishSpawning(Inner);
				Designer->OnBuildableConstructedInsideDesigner(Machine);
			}
			Self.Check(Designer && Machine && Designer->HasBuildings() && !Scene.Area.Contains(Machine->GetActorLocation()),
				FString::Printf(TEXT("designer %d holds a Constructor outside the polygon: %d"), Designer != nullptr, Designer && Designer->HasBuildings()));
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
		AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"));
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
		AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"));
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
			int32 Items = 0;
			int32 Found = 0;
			for (TActorIterator<AFGCrate> It(World); It; ++It)
			{
				if (D->CratePlaces.ContainsByPredicate([&It](const FVector& Place) { return FVector::Dist(It->GetActorLocation(), Place) < 10.0; }))
				{
					Items += It->GetInventory()->GetNumItems(nullptr);
					++Found;
				}
			}
			Self.Check(Found == D->CratePlaces.Num() && Items == D->CrateItems, FString::Printf(TEXT("loaded: %d of %d refund crates hold %d of %d items"), Found,
				D->CratePlaces.Num(), Items, D->CrateItems));
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
		R.Steps.Add(WaitCalm(60));
		// Building 5000 lightweights leaves the physics scene a few seconds of work in the background; the first change to it before that
		// is done waits for it in TG_DuringPhysics (measured: 0.4 to 6.8 s in one frame right after the build, 36 ms after 25 s). Real
		// players do not dismantle a fresh 5000-building scene, so the job is measured after it
		R.Steps.Add([Until = MakeShared<double>(0.0)](FRun& Self)
		{
			if (*Until == 0.0)
			{
				*Until = FPlatformTime::Seconds() + 25.0;
			}
			return FPlatformTime::Seconds() < *Until ? EStep::Wait : EStep::Next;
		});
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
		AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"));
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
			Self.Step(FString::Printf(TEXT("scene at %s"), *Scene.Center.ToCompactString()));
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
		}, (static_cast<int32>(Kind) - 1) * 15000.0);
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
				Rco->ServerDismantle(RequestId, S->Area, EAreaActionsDismantleRefund::All);
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
			AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"));
		}
		else
		{
			AddOpenAction(R, TEXT("ClearInventories"));
			AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("NoButton"));
			AddResultWindow(R, S);
		}
		R.Steps.Add([S, Energy, bDismantle](FRun& Self)
		{
			// The crates packed (AA-093); the chainsaw keeps its state
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			const FCratesSeen Seen = SeeCrates(Crates, Self.GetPlayer()->GetPawn()->GetActorLocation());
			TArray<FInventoryStack> Stacks;
			for (const AFGCrate* Crate : Crates)
			{
				TArray<FInventoryStack> Each;
				Crate->GetInventory()->GetInventoryStacks(Each);
				Stacks.Append(Each);
			}
			const FInventoryStack* Saw = Stacks.FindByPredicate([](const FInventoryStack& Stack) { return Stack.Item.HasState(); });
			Self.Check(Seen.bOneTypeEach && Saw && FMath::IsNearlyEqual(StateEnergy(*Saw), Energy), FString::Printf(TEXT("%s: crates %d (%s), chainsaw energy %.1f (want %.1f)"),
				bDismantle ? TEXT("MassDismantle") : TEXT("Move to crate"), Crates.Num(), *Seen.Text(), Saw ? StateEnergy(*Saw) : -1.0f, Energy));
			return EStep::Next;
		});
	}

	// The same number in both games for the same contents, whatever the order of the stacks
	int32 ContentsHash(const TArray<FInventoryStack>& Stacks, int32& OutStacks, int32& OutItems)
	{
		uint32 Hash = 0;
		OutStacks = OutItems = 0;
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems())
			{
				++OutStacks;
				OutItems += Stack.NumItems;
				Hash += FCrc::StrCrc32(*Stack.Item.GetItemClass()->GetPathName()) * static_cast<uint32>(Stack.NumItems);
			}
		}
		return static_cast<int32>(Hash & 0x7FFFFF);
	}

	// AA-085 point 3, two games: the client's player opens the largest of the new crates as a player does: stands by it, looks at it,
	// presses the use key. A crate's contents go only to a player who has it open: they must arrive within 10 s without a frame over
	// 200 ms, and the host compares what the client saw (stacks, items, a hash of kinds and amounts) with what the crate holds
	void AddOpenLargestCrate(FRun& R, const FSceneRef& S)
	{
		struct FOpen
		{
			TWeakObjectPtr<AFGCrate> Crate;
			int32 Stacks = 0, Items = 0, Hash = 0;
			double Pressed = 0.0;
		};
		TSharedRef<FOpen> O = MakeShared<FOpen>();
		Mark(R, ESide::Host);
		R.Steps.Add([S, O](FRun& Self)
		{
			TArray<FInventoryStack> Stacks;
			for (AFGCrate* Crate : NewCrates(Self, *S))
			{
				TArray<FInventoryStack> Each;
				Crate->GetInventory()->GetInventoryStacks(Each);
				if (Each.Num() > Stacks.Num())
				{
					Stacks = Each;
					O->Crate = Crate;
				}
			}
			AFGCharacterPlayer* Character = GetCharacter(Self);
			if (!Self.Check(O->Crate.IsValid() && Character, TEXT("largest crate found")))
			{
				return EStep::Next;
			}
			O->Hash = ContentsHash(Stacks, O->Stacks, O->Items);
			const FVector At = O->Crate->GetActorLocation();
			// The player goes 2 m from the crate, on the side of the place it stood at
			const FVector Spot = At + (Self.Origin - At).GetSafeNormal2D() * 200.0 + FVector(0, 0, Self.Origin.Z - At.Z);
			Character->TeleportTo(Spot, (At - Spot).Rotation());
			Self.Step(FString::Printf(TEXT("largest crate %s at %s: %d stacks, %d items, hash %d; the player goes to %s"), *O->Crate->GetName(), *At.ToCompactString(),
				O->Stacks, O->Items, O->Hash, *Spot.ToCompactString()));
			Send(Self, MsgCrateStacks, O->Stacks);
			Send(Self, MsgCrateItems, O->Items);
			Send(Self, MsgCrateHash, O->Hash);
			Send(Self, MsgCrateX, FMath::RoundToInt32(At.X));
			Send(Self, MsgCrateY, FMath::RoundToInt32(At.Y));
			Send(Self, MsgCrateZ, FMath::RoundToInt32(At.Z));
			return EStep::Next;
		});
		Mark(R, ESide::Client);
		R.Steps.Add([O](FRun& Self)
		{
			const AFGCharacterPlayer* Character = GetCharacter(Self);
			if (!Self.Shared.Contains(MsgCrateZ) || !Character)
			{
				return EStep::Wait;
			}
			const FVector At(Self.Shared.FindRef(MsgCrateX), Self.Shared.FindRef(MsgCrateY), Self.Shared.FindRef(MsgCrateZ));
			if (!O->Crate.IsValid())
			{
				for (TActorIterator<AFGCrate> It(Self.GetWorld()); It; ++It)
				{
					if (IsValid(*It) && FVector::Dist(It->GetActorLocation(), At) < 100.0)
					{
						O->Crate = *It;
					}
				}
			}
			// Looking at the crate from where the host put the player, until the game offers the crate for use
			if (!O->Crate.IsValid() || FVector::Dist(Character->GetActorLocation(), At) > 500.0)
			{
				return EStep::Wait;
			}
			LookAt(Self, O->Crate->GetActorLocation());
			if (Character->GetBestUsableActor() != O->Crate.Get())
			{
				return EStep::Wait;
			}
			TArray<FInventoryStack> Before;
			O->Crate->GetInventory()->GetInventoryStacks(Before);
			int32 Stacks = 0, Items = 0;
			ContentsHash(Before, Stacks, Items);
			Self.Check(true, FString::Printf(TEXT("the crate %s is the one to use; closed, this game sees %d stacks of it; use key down"), *O->Crate->GetName(), Stacks));
			Self.MaxFrameMs = 0.0;
			Self.bMeasureFrames = true;
			O->Pressed = Self.Now();
			FViewport* Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
			Self.GetPlayer()->InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), EKeys::E, IE_Pressed));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([O](FRun& Self)
		{
			FViewport* Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
			Self.GetPlayer()->InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), EKeys::E, IE_Released));
			return EStep::Next;
		});
		R.Steps.Add([O](FRun& Self)
		{
			TArray<FInventoryStack> Seen;
			if (O->Crate.IsValid())
			{
				O->Crate->GetInventory()->GetInventoryStacks(Seen);
			}
			int32 Stacks = 0, Items = 0;
			const int32 Hash = ContentsHash(Seen, Stacks, Items);
			const double Waited = Self.Now() - O->Pressed;
			// The game's interact window, wherever its HUD puts it
			const UFGInteractWidget* Window = nullptr;
			for (TObjectIterator<UFGInteractWidget> It; It; ++It)
			{
				if (IsValid(*It) && It->GetWorld() == Self.GetWorld() && It->GetCachedWidget().IsValid())
				{
					Window = *It;
				}
			}
			if ((Stacks < Self.Shared.FindRef(MsgCrateStacks) || !Window) && Waited < 10.0)
			{
				return EStep::Wait;
			}
			Self.bMeasureFrames = false;
			Self.Check(Window != nullptr, FString::Printf(TEXT("the crate's window is open: %s"), Window ? *Window->GetClass()->GetName() : TEXT("none")));
			Self.Check(Stacks > 0 && Waited < 10.0, FString::Printf(TEXT("the crate's contents arrived %.2f s after the key: %d stacks, %d items"), Waited, Stacks, Items));
			Self.Check(Self.MaxFrameMs <= 200.0, FString::Printf(TEXT("longest frame while the crate opened: %.1f ms (200)"), Self.MaxFrameMs));
			Send(Self, MsgSeenStacks, Stacks);
			Send(Self, MsgSeenItems, Items);
			Send(Self, MsgSeenHash, Hash);
			return EStep::Next;
		});
		Mark(R, ESide::Host);
		R.Steps.Add([O](FRun& Self)
		{
			if (!Self.Shared.Contains(MsgSeenHash))
			{
				return EStep::Wait;
			}
			const int32 Stacks = Self.Shared.FindRef(MsgSeenStacks), Items = Self.Shared.FindRef(MsgSeenItems), Hash = Self.Shared.FindRef(MsgSeenHash);
			Self.Check(Stacks == O->Stacks && Items == O->Items && Hash == O->Hash, FString::Printf(TEXT("the client saw %d stacks, %d items, hash %d; the crate holds %d stacks, %d items, hash %d"),
				Stacks, Items, Hash, O->Stacks, O->Items, O->Hash));
			return EStep::Next;
		});
		Mark(R, ActSide(R));
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
				if (Storage && !Self.bClient)
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
			AddDismantle(R, S, TEXT("YesButton"), TEXT("Refund to crate"));
		}
		else
		{
			AddOpenAction(R, TEXT("ClearInventories"));
			AddPress(R, TEXT("Widget_MessageYesNo"), TEXT("NoButton"));
			AddResultWindow(R, S);
		}
		AddChecks(R, [S, P, bDismantle](FRun& Self)
		{
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			if (Self.bClient)
			{
				// What the crates hold the client's game learns when it opens one (AddOpenLargestCrate)
				const FString Want = FString::Printf(TEXT("The items are in %d crates next to you."), Crates.Num());
				Self.Check(Crates.Num() > 1 && (!Self.Acts() || S->Message.Contains(Want)), FString::Printf(TEXT("new crates here %d; result '%s' has '%s'"), Crates.Num(), *S->Message, *Want));
				return EStep::Next;
			}
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
			// Packed (AA-093): as many crates as first-fit-decreasing needs, each type in one crate unless it has more than 200 stacks
			const int32 WantCrates = PackedCrates(P->Taken);
			const FCratesSeen Seen = SeeCrates(Crates, Self.ActingPlace());
			Self.Check(Crates.Num() == WantCrates && Seen.bOneTypeEach && SameCounts(InCrates, P->Taken),
				FString::Printf(TEXT("%s: %d crates (want %d, %d full, %s) for %d stacks; in them %s, taken %s"), bDismantle ? TEXT("MassDismantle") : TEXT("Move to crate"),
					Crates.Num(), WantCrates, Full, *Seen.Text(), Stacks, *ItemsText(InCrates), *ItemsText(P->Taken)));
			const FString Want = FString::Printf(TEXT("The items are in %d crates next to you."), Crates.Num());
			Self.Check(!Self.Acts() || S->Message.Contains(Want), FString::Printf(TEXT("result '%s' has '%s'"), *S->Message, *Want));
			return EStep::Next;
		});
		if (R.bMp)
		{
			AddOpenLargestCrate(R, S);
		}
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

	// A window of the tool closed with its X
	void AddCloseWithX(FRun& R, const TCHAR* Name)
	{
		AddOnWindow(R, Name, [Name](FRun& Self, UUserWidget* Window)
		{
			UUserWidget* Frame = Cast<UUserWidget>(Window->GetWidgetFromName(TEXT("WindowDark")));
			FMulticastDelegateProperty* Property = Frame ? FindFProperty<FMulticastDelegateProperty>(Frame->GetClass(), TEXT("OnClose")) : nullptr;
			if (Property)
			{
				Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Frame)->ProcessMulticastDelegate<UObject>(nullptr);
			}
			Self.Check(Property != nullptr, FString::Printf(TEXT("%s closed with X"), Name));
		});
	}

	// AA-089 point 1, two games: a Copy preview of 9 buildings stays open for 5 s; the other game counts the holograms and preview actors
	// it has all the time (Watch), which must be none
	void AddPreviewHold(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		AddCopyPreview(R, C);
		R.Steps.Add(WaitSeconds(5.0));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(AAreaActionsPreview::LiveGhosts == 9 && PreviewHolograms(Self) == 8, FString::Printf(TEXT("after 5 s: ghosts %d (want 9), holograms %d (want 8)"),
				AAreaActionsPreview::LiveGhosts, PreviewHolograms(Self)));
			return EStep::Next;
		});
		AddCloseWithX(R, TEXT("Widget_Copy"));
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(AAreaActionsPreview::LiveGhosts == 0 && PreviewHolograms(Self) == 0 && !ActiveAction(Self), FString::Printf(TEXT("after Cancel: ghosts %d, holograms %d, action %s"),
				AAreaActionsPreview::LiveGhosts, PreviewHolograms(Self), *GetNameSafe(ActiveAction(Self))));
			return EStep::Next;
		});
	}

	// AA-089 point 2: a Fill preview with 300 of the game's holograms (the most a preview makes): none of their sound components plays
	void AddPreviewSound(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildSingle);
		AddOpenAction(R, TEXT("Fill"));
		AddFillWindow(R, FVector(1, 1, 0), FIntVector(31, 10, 1), FVector2D(0, 0), false, TEXT("PreviewButton"));
		// The sounds get their time to start
		R.Steps.Add(WaitSeconds(3.0));
		R.Steps.Add([](FRun& Self)
		{
			int32 Holograms = 0, Sounds = 0, Playing = 0, Loops = 0;
			for (TActorIterator<AFGHologram> It(Self.GetWorld()); It; ++It)
			{
				if (!It->GetOwner() || !It->GetOwner()->IsA<AAreaActionsPreview>())
				{
					continue;
				}
				++Holograms;
				TInlineComponentArray<UAkComponent*> Components(*It);
				for (const UAkComponent* Sound : Components)
				{
					++Sounds;
					Playing += Sound->IsPlaying() ? 1 : 0;
				}
				const FObjectPropertyBase* Loop = FindFProperty<FObjectPropertyBase>(AFGHologram::StaticClass(), TEXT("mLoopSound"));
				Loops += Loop && Loop->GetObjectPropertyValue_InContainer(*It) ? 1 : 0;
			}
			Self.Check(Holograms == 300 && AAreaActionsPreview::LiveGhosts == 309, FString::Printf(TEXT("preview: %d holograms (want 300), %d ghosts (want 309)"), Holograms, AAreaActionsPreview::LiveGhosts));
			Self.Check(Playing == 0, FString::Printf(TEXT("sound components on the preview's holograms: %d, playing %d (want 0); holograms with a loop sound set %d"), Sounds, Playing, Loops));
			return EStep::Next;
		});
		AddCloseWithX(R, TEXT("Widget_Fill"));
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(AAreaActionsPreview::LiveGhosts == 0 && PreviewHolograms(Self) == 0, FString::Printf(TEXT("after Cancel: ghosts %d, holograms %d"), AAreaActionsPreview::LiveGhosts, PreviewHolograms(Self)));
			return EStep::Next;
		});
	}

	// AA-040 point 5: the client's game ends here: its result line, the report of this section to the host, then the game closes, which
	// is the disconnect. The host waits until the client's player is gone and 2 s more; by then it must not have had a window, an indicator,
	// an action actor or a ghost of the client's. From here on the host's own player is the one who acts.
	void AddClientLeaves(FRun& R, const TCHAR* While)
	{
		Mark(R, ESide::Client);
		R.Steps.Add([While](FRun& Self)
		{
			Self.Check(true, FString::Printf(TEXT("the client's game leaves %s"), While));
			// This step and its section's report stay, the rest of the list goes
			Self.Steps.SetNum(Self.Index + 2, EAllowShrinking::No);
			return EStep::Next;
		});
		Mark(R, ESide::Host);
		R.Steps.Add([](FRun& Self)
		{
			if (Self.Peer.IsValid())
			{
				return EStep::Wait;
			}
			Self.bPeerLeft = true;
			Self.Step(TEXT("the client's player is gone"));
			return EStep::Next;
		});
		R.Steps.Add(WaitSeconds(2.0));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(Self.MaxWindows == 0 && Self.MaxIndicators == 0 && Self.MaxActions == 0 && Self.MaxGhosts == 0, FString::Printf(
				TEXT("until 2 s after the client left: here at most mod windows %d, tool indicators %d, action actors %d, ghosts %d (want 0)"),
				Self.MaxWindows, Self.MaxIndicators, Self.MaxActions, Self.MaxGhosts));
			Self.bPeerActs = false;
			return EStep::Next;
		});
		R.bBuildAfterLeave = true;
	}

	// The host goes on for 10 s and its own player runs an action: the server's jobs are alive. A dedicated server has no player of
	// its own: it goes on for 10 s with no job left
	void AddHostGoesOn(FRun& R)
	{
		R.Steps.Add(WaitSeconds(10.0));
		if (R.bDedicated)
		{
			R.Steps.Add([](FRun& Self)
			{
				const AAreaActionsServerSubsystem* Server = AAreaActionsServerSubsystem::Get(Self.GetWorld());
				Self.Check(Server && Server->NumJobs() == 0, FString::Printf(TEXT("the dedicated server went on 10 s after the client left: jobs %d"), Server ? Server->NumJobs() : -1));
				return EStep::Next;
			});
			return;
		}
		AddSetStation(R, 12000.0);
	}

	// The client asks for SetRecipe on 1000 machines that hold items, the server changes one a frame (budget 0), and the client leaves:
	// the job ends without it, and the items of the machines it changed are in crates
	void AddDisconnectJob(FRun& R)
	{
		constexpr int32 Columns = 50;
		constexpr int32 Rows = 20;
		constexpr double Spacing = 1000.0;
		FSceneRef S = MakeShared<FScene>();
		TSharedRef<int32> JobsBefore = MakeShared<int32>(0);
		TSharedRef<int32> UnsentBefore = MakeShared<int32>(0);
		AddEquipTool(R);
		AddBoth(R, [S](FRun& Self)
		{
			PlaceScene(Self, *S, Columns * Spacing / 2.0, 2000.0);
			S->Center.Z += 2000.0;
			S->Area.MinZ = S->Center.Z - 500.0;
			S->Area.MaxZ = S->Center.Z + 500.0;
			return EStep::Next;
		});
		Mark(R, ESide::Host);
		R.Steps.Add([](FRun& Self)
		{
			MakeAvailable(Self, Asset::IronRod);
			SetBudget(0.0f);
			return EStep::Next;
		});
		for (int32 Batch = 0; Batch < Columns * Rows / 100; ++Batch)
		{
			R.Steps.Add([S, Batch](FRun& Self)
			{
				for (int32 Index = Batch * 100; Index < (Batch + 1) * 100; ++Index)
				{
					const FVector At = S->Center + FVector((Index % Columns - Columns / 2.0 + 0.5) * Spacing, (Index / Columns - Rows / 2.0 + 0.5) * Spacing, 0.0);
					S->Inside.Add(SpawnMachine(Self, Asset::Constructor, At, Asset::IronPlate, 2, 1));
				}
				return EStep::Next;
			});
		}
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([S, JobsBefore, UnsentBefore](FRun& Self)
		{
			Self.Check(S->Inside.Num() == Columns * Rows && AllValid(S->Inside), FString::Printf(TEXT("%d machines built, 3 items in each"), S->Inside.Num()));
			SnapshotCrates(Self, *S);
			*JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			*UnsentBefore = AAreaActionsServerSubsystem::ResultsWithoutPlayer;
			return EStep::Next;
		});
		Mark(R, ESide::Client);
		R.Steps.Add([S](FRun& Self)
		{
			Self.Step(TEXT("request 401: SetRecipe for 1000 machines"));
			UAreaActionsRCO::Of(Self.GetPlayer())->ServerSetRecipe(401, S->Area, Recipe(Asset::IronRod));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		AddClientLeaves(R, TEXT("in the middle of its SetRecipe job"));
		TSharedRef<int32> JobsAtLeave = MakeShared<int32>(-1);
		R.Steps.Add([S, JobsBefore, JobsAtLeave, UnsentBefore](FRun& Self)
		{
			const AAreaActionsServerSubsystem* Server = AAreaActionsServerSubsystem::Get(Self.GetWorld());
			if (*JobsAtLeave < 0)
			{
				*JobsAtLeave = Server ? Server->NumJobs() : 0;
				Self.Check(*JobsAtLeave == 1, FString::Printf(TEXT("2 s after the client left its job still runs: jobs %d"), *JobsAtLeave));
			}
			if (Server && Server->NumJobs() > 0)
			{
				return EStep::Wait;
			}
			int32 Changed = 0, InChanged = 0, InMachines = 0;
			for (const TWeakObjectPtr<AFGBuildable>& Built : S->Inside)
			{
				const AFGBuildableManufacturer* Constructor = Cast<AFGBuildableManufacturer>(Built.Get());
				const bool bChanged = Constructor && Constructor->GetCurrentRecipe() == Recipe(Asset::IronRod);
				Changed += bChanged ? 1 : 0;
				InChanged += bChanged ? MachineItems(Constructor) : 0;
				InMachines += Constructor ? MachineItems(Constructor) : 0;
			}
			const FCratesSeen Seen = SeeCrates(NewCrates(Self, *S), Self.Origin);
			const int32 All = Columns * Rows;
			// AA-095: the job runs to its end without the player, and its result goes to nobody
			const int32 Finished = AAreaActionsServerSubsystem::FinishedJobs - *JobsBefore;
			const int32 Unsent = AAreaActionsServerSubsystem::ResultsWithoutPlayer - *UnsentBefore;
			Self.Check(Server && Changed == All && Finished == 1 && Unsent == 1,
				FString::Printf(TEXT("the job ran to its end after the player left: %d of %d machines changed, AAJOB lines %d (want 1), results with no player to go to %d (want 1)"),
					Changed, All, Finished, Unsent));
			Self.Check(InChanged == 0 && Seen.All == Changed * 3 && InMachines + Seen.All == All * 3,
				FString::Printf(TEXT("the changed machines' items are in crates: %d in crates (want %d), %d left in changed machines, %d in all machines and crates (want %d)"),
					Seen.All, Changed * 3, InChanged, InMachines + Seen.All, All * 3));
			return EStep::Next;
		});
		AddHostGoesOn(R);
	}

	// The client leaves with its Copy preview open: nothing of it is at the host, nothing was built
	void AddDisconnectPreview(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildBasic);
		AddCopyPreview(R, C);
		R.Steps.Add(WaitSeconds(2.0));
		AddClientLeaves(R, TEXT("with its Copy preview open"));
		R.Steps.Add([S, C](FRun& Self)
		{
			CheckCopies(Self, *C, false, TEXT("after the client left"));
			CheckPattern(Self, *S, *C, TEXT("after the client left"));
			return EStep::Next;
		});
		AddHostGoesOn(R);
	}

	// AA-095: the client fills with build cost, paid from a container next to it; the server builds one copy a frame (budget 0) and the
	// client leaves on the way. The server builds every copy and takes the cost once. With bRefuse every 10th copy counts as refused by
	// the game: its cost goes into a crate by the pattern, because the player is gone
	void AddDisconnectCopy(FRun& R, bool bRefuse)
	{
		const int32 Side = 20;
		const int32 Copies = Side * Side - 1;
		const int32 EveryNth = 10;
		const int32 Refused = bRefuse ? Copies / EveryNth : 0;
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		TSharedRef<int32> UnsentBefore = MakeShared<int32>(0);
		AddCopyScene(R, S, C, BuildSingle);
		AddHost(R, [S, C, UnsentBefore, Copies, EveryNth, bRefuse](FRun& Self)
		{
			// The whole cost and 7 more of each item in the container 15 m behind the player, none in the inventory
			SetCostSources(Self, *C, CostNone, [Copies](int32 Cost) { return Cost * Copies + 7; }, CostNone, CostNone);
			SnapshotCrates(Self, *S);
			*UnsentBefore = AAreaActionsServerSubsystem::ResultsWithoutPlayer;
			SetBudget(0.0f);
			AreaActionsJobs::SetRefuseCopiesForTest(bRefuse ? EveryNth : 0);
			return EStep::Next;
		});
		AddOpenAction(R, TEXT("Fill"));
		AddFillWindow(R, FVector(1, 1, 0), FIntVector(Side, Side, 1), FVector2D(0, 0), true);
		R.Steps.Add(WaitFrames(10));
		AddClientLeaves(R, TEXT("in the middle of its Fill job with build cost"));
		R.Steps.Add([S, C, UnsentBefore, Copies, Refused](FRun& Self)
		{
			const AAreaActionsServerSubsystem* Server = AAreaActionsServerSubsystem::Get(Self.GetWorld());
			if (Server && Server->NumJobs() > 0)
			{
				return EStep::Wait;
			}
			SetBudget(2.0f);
			AreaActionsJobs::SetRefuseCopiesForTest(0);
			const int32 Lightweights = LightweightCount(Self.GetWorld());
			const int32 Finished = AAreaActionsServerSubsystem::FinishedJobs - C->JobsBefore;
			const int32 Unsent = AAreaActionsServerSubsystem::ResultsWithoutPlayer - *UnsentBefore;
			Self.Check(Lightweights == C->LightweightsBefore + Copies - Refused && Finished == 1 && Unsent == 1,
				FString::Printf(TEXT("the Fill ran to its end after the player left: lightweights %d (want %d), AAJOB lines %d (want 1), results with no player to go to %d (want 1)"),
					Lightweights, C->LightweightsBefore + Copies - Refused, Finished, Unsent));
			// The cost of every planned copy went once, from the container next to the player; the far one and the depot kept theirs
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C->Cost)
			{
				const int32 Near = StorageCount(C->NearBox, Entry.Key);
				const int32 WantNear = C->Near.FindRef(Entry.Key) - Entry.Value * Copies;
				Self.Check(Near == WantNear && StorageCount(C->FarBox, Entry.Key) == C->Far.FindRef(Entry.Key) && DepotCount(Self.GetWorld(), Entry.Key) == C->Depot.FindRef(Entry.Key),
					FString::Printf(TEXT("cost taken once: %s near container %d (want %d), far %d (want %d), depot %d (want %d)"), *GetNameSafe(Entry.Key), Near, WantNear,
						StorageCount(C->FarBox, Entry.Key), C->Far.FindRef(Entry.Key), DepotCount(Self.GetWorld(), Entry.Key), C->Depot.FindRef(Entry.Key)));
			}
			// The refused copies' cost: in crates by the pattern's first building, as much as those copies cost
			const TArray<AFGCrate*> Crates = NewCrates(Self, *S);
			const FCratesSeen Seen = SeeCrates(Crates, C->Pattern.IsEmpty() ? Self.Origin : C->Pattern[0].Transform.GetLocation());
			int32 WantAll = 0;
			bool bEach = true;
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : C->Cost)
			{
				WantAll += Entry.Value * Refused;
				bEach &= Seen.Items.FindRef(Entry.Key) == Entry.Value * Refused;
			}
			Self.Check(bEach && Seen.All == WantAll && (Refused == 0 ? Crates.IsEmpty() : Seen.Far < 1500.0),
				FString::Printf(TEXT("refund of %d refused copies with the player gone: %d crates, %d items (want %d), farthest from the pattern %.0f cm"), Refused, Crates.Num(),
					Seen.All, WantAll, Seen.Far));
			return EStep::Next;
		});
		AddHostGoesOn(R);
	}

	// AA-050 point 4: the client copies, the server checks the copies, writes down what it expects and saves; ServerRestart checks it in
	// the server's next process
	const TCHAR* const RestartSave = TEXT("AATest_ServerRestart");

	FString RestartFile()
	{
		return FPaths::ProjectSavedDir() / TEXT("AATest_ServerRestart.txt");
	}

	void AddServerSave(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		TSharedRef<int32> Saved = MakeShared<int32>(-1);
		AddCopyScene(R, S, C, BuildBasic);
		AddCopy(R, S, C, FVector(20, 0, 0), 0.0, false);
		AddCheckMessage(R, S, TEXT("Copied 9 buildings."));
		AddChecks(R, [C](FRun& Self)
		{
			CheckCopies(Self, *C, true, TEXT("copied"));
			return EStep::Next;
		});
		AddHost(R, [C, Saved](FRun& Self)
		{
			TArray<FString> Lines;
			Lines.Add(FString::Printf(TEXT("Copy\t%s\t%s\t%s"), *C->Delta.ToString(), *FString::SanitizeFloat(C->Yaw), *C->Pivot.ToString()));
			for (const FPiece& Piece : C->Pattern)
			{
				Lines.Add(FString::Printf(TEXT("Piece\t%d\t%s\t%s\t%s\t%s"), Piece.bLightweight ? 1 : 0, *FString::SanitizeFloat(Piece.BeamLength), *Piece.ClassPath,
					Piece.RecipePath.IsEmpty() ? TEXT("-") : *Piece.RecipePath, *Piece.Transform.ToString()));
			}
			Self.Check(FFileHelper::SaveStringArrayToFile(Lines, *RestartFile()), FString::Printf(TEXT("expected copies written to %s: %d pieces"), *RestartFile(), C->Pattern.Num()));
			Self.Step(FString::Printf(TEXT("saving %s"), RestartSave));
			UFGSaveSession::Get(Self.GetWorld())->SaveGame(RestartSave, FOnSaveGameComplete::CreateLambda([Saved](bool bOk, const FText& Error, void*)
			{
				*Saved = bOk ? 1 : 0;
				UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST save done: %d %s"), bOk, *Error.ToString());
			}), nullptr);
			return EStep::Next;
		});
		AddHost(R, [Saved](FRun& Self)
		{
			if (*Saved < 0)
			{
				return EStep::Wait;
			}
			Self.Check(*Saved == 1, FString::Printf(TEXT("the server saved %s"), RestartSave));
			return EStep::Next;
		});
	}

	// The mod's Error and Warning lines while it lives, both log categories (the C++ one and the Blueprints' one)
	struct FModLogCounter : public FOutputDevice
	{
		int32 Errors = 0;
		int32 Warnings = 0;
		FModLogCounter() { GLog->AddOutputDevice(this); }
		virtual ~FModLogCounter() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == LogAreaActionsCpp.GetCategoryName() || Category == FName(TEXT("LogAreaActions")))
			{
				Errors += Verbosity <= ELogVerbosity::Error ? 1 : 0;
				Warnings += Verbosity == ELogVerbosity::Warning ? 1 : 0;
			}
		}
	};

	// AA-050 points 4 and 5, the dedicated server alone in its next process: ServerSave's copies stand with their data, then 5 minutes with
	// no player and no mod error, the world not paused
	void AddServerRestart(FRun& R)
	{
		FCopyRef C = MakeShared<FCopyState>();
		R.StepTimeoutSec = 420.0;
		R.Steps.Add(WaitCalm(30));
		R.Steps.Add([C](FRun& Self)
		{
			TArray<FString> Lines;
			if (!Self.Check(FFileHelper::LoadFileToStringArray(Lines, *RestartFile()) && !Lines.IsEmpty(), FString::Printf(TEXT("expected copies read from %s"), *RestartFile())))
			{
				return EStep::Next;
			}
			for (const FString& Line : Lines)
			{
				TArray<FString> Parts;
				Line.ParseIntoArray(Parts, TEXT("\t"), false);
				if (Parts.Num() == 4 && Parts[0] == TEXT("Copy"))
				{
					C->Delta.InitFromString(Parts[1]);
					LexFromString(C->Yaw, *Parts[2]);
					C->Pivot.InitFromString(Parts[3]);
				}
				else if (Parts.Num() == 6 && Parts[0] == TEXT("Piece"))
				{
					FPiece Piece;
					Piece.bLightweight = Parts[1] == TEXT("1");
					LexFromString(Piece.BeamLength, *Parts[2]);
					Piece.ClassPath = Parts[3];
					Piece.RecipePath = Parts[4] == TEXT("-") ? FString() : Parts[4];
					Piece.Transform.InitFromString(Parts[5]);
					Piece.Resolve();
					Piece.bIgnored = Piece.Class && AreaActionsBuildings::ShouldIgnore(Piece.Class);
					C->Pattern.Add(Piece);
				}
			}
			Self.Step(FString::Printf(TEXT("loaded world %s: %d pieces expected, copy moved by %s"), *Self.GetWorld()->GetMapName(), C->Pattern.Num(), *C->Delta.ToCompactString()));
			CheckCopies(Self, *C, true, TEXT("after the server restart"));
			return EStep::Next;
		});
		struct FIdle { TSharedPtr<FModLogCounter> Log; double Start = 0.0; double GameStart = 0.0; double LastReport = 0.0; };
		R.Steps.Add([Idle = MakeShared<FIdle>()](FRun& Self)
		{
			UWorld* World = Self.GetWorld();
			if (!Idle->Log)
			{
				Idle->Log = MakeShared<FModLogCounter>();
				Idle->Start = Idle->LastReport = Self.Now();
				Idle->GameStart = World->GetTimeSeconds();
				Self.Step(FString::Printf(TEXT("5 minutes with no player, net mode %d, players %d"), static_cast<int32>(World->GetNetMode()), World->GetNumPlayerControllers()));
			}
			if (Self.Now() - Idle->LastReport >= 60.0)
			{
				Idle->LastReport = Self.Now();
				Self.Step(FString::Printf(TEXT("idle %.0f s: game time %.0f s, mod errors %d, warnings %d, paused %d"), Self.Now() - Idle->Start, World->GetTimeSeconds() - Idle->GameStart,
					Idle->Log->Errors, Idle->Log->Warnings, World->IsPaused()));
			}
			if (Self.Now() - Idle->Start < 300.0)
			{
				return EStep::Wait;
			}
			const double Real = Self.Now() - Idle->Start;
			const double Game = World->GetTimeSeconds() - Idle->GameStart;
			Self.Check(Idle->Log->Errors == 0, FString::Printf(TEXT("%.0f s with no player: mod error lines %d (want 0), warnings %d"), Real, Idle->Log->Errors, Idle->Log->Warnings));
			Self.Check(World->GetNumPlayerControllers() == 0 && Game >= Real * 0.9, FString::Printf(TEXT("the server ran, not paused: game time %.0f s of %.0f s, players %d"), Game, Real,
				World->GetNumPlayerControllers()));
			Idle->Log.Reset();
			return EStep::Next;
		});
	}

	// A save for the dedicated server's runs: a new game with the player at the test place
	void AddServerBase(FRun& R)
	{
		TSharedRef<int32> Saved = MakeShared<int32>(-1);
		R.Steps.Add(WaitCalm(30));
		R.Steps.Add([Saved](FRun& Self)
		{
			UFGSaveSession::Get(Self.GetWorld())->SaveGame(TEXT("AATest_ServerBase"), FOnSaveGameComplete::CreateLambda([Saved](bool bOk, const FText&, void*) { *Saved = bOk ? 1 : 0; }), nullptr);
			return EStep::Next;
		});
		R.Steps.Add([Saved](FRun& Self)
		{
			if (*Saved < 0)
			{
				return EStep::Wait;
			}
			Self.Check(*Saved == 1, TEXT("saved AATest_ServerBase"));
			return EStep::Next;
		});
	}

	// ---- Chat 10: the extras (AA-060), the dismantle windows (AA-092), the tool's events (AA-096) ----

	UAreaActionsPopupBase* GetPopup(const FRun& R)
	{
		return Cast<UAreaActionsPopupBase>(ObjectValue(GetTool(R), TEXT("PopupWidget")));
	}

	int32 IntValue(UObject* Object, const TCHAR* Name)
	{
		FIntProperty* Property = Object ? FindFProperty<FIntProperty>(Object->GetClass(), Name) : nullptr;
		return Property ? Property->GetPropertyValue_InContainer(Object) : INDEX_NONE;
	}

	// A Blueprint function of the object without parameters that returns a text (the menu's bound texts)
	FString CallText(UObject* Object, const TCHAR* Function)
	{
		UFunction* Fn = Object ? Object->FindFunction(Function) : nullptr;
		FTextProperty* Return = Fn ? CastField<FTextProperty>(Fn->GetReturnProperty()) : nullptr;
		if (!Return)
		{
			return TEXT("<none>");
		}
		uint8* Params = static_cast<uint8*>(FMemory_Alloca(Fn->ParmsSize));
		FMemory::Memzero(Params, Fn->ParmsSize);
		Return->InitializeValue_InContainer(Params);
		Object->ProcessEvent(Fn, Params);
		const FString Text = Return->GetPropertyValue_InContainer(Params).ToString();
		Return->DestroyValue_InContainer(Params);
		return Text;
	}

	void CallNoParams(UObject* Object, const TCHAR* Function)
	{
		if (UFunction* Fn = Object ? Object->FindFunction(Function) : nullptr; Fn && Fn->ParmsSize == 0)
		{
			Object->ProcessEvent(Fn, nullptr);
		}
	}

	// The menu, opened with the right mouse button when it is not open
	void AddOpenMenu(FRun& R)
	{
		R.Steps.Add([](FRun& Self)
		{
			if (!GetPopup(Self))
			{
				Self.Step(TEXT("right mouse button"));
				PressKey(Self, EKeys::RightMouseButton);
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(5));
		R.Steps.Add([](FRun& Self)
		{
			UAreaActionsPopupBase* Popup = GetPopup(Self);
			Self.Check(Popup && Popup->IsInViewport(), FString::Printf(TEXT("menu open: %s"), *GetNameSafe(Popup)));
			return EStep::Next;
		});
	}

	void AddCloseMenu(FRun& R)
	{
		R.Steps.Add([](FRun& Self)
		{
			CallNoParams(GetPopup(Self), TEXT("Close"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(!GetPopup(Self) || !GetPopup(Self)->IsInViewport(), TEXT("menu closed"));
			return EStep::Next;
		});
	}

	// The tool put away (the game's unequip); AddEquipTool takes it again
	void AddPutToolAway(FRun& R)
	{
		// The server puts it away, as it put it in hand; a client's own unequip stays in its game and the server keeps the tool in hand
		AddHost(R, [](FRun& Self)
		{
			AFGCharacterPlayer* Character = GetCharacter(Self);
			if (AAreaActionsEquipment* Tool = GetTool(Self); Character && Tool)
			{
				Character->UnequipEquipment(Tool);
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		AddChecks(R, [](FRun& Self)
		{
			Self.Check(!GetTool(Self), TEXT("tool put away"));
			return EStep::Next;
		});
	}

	// AA-060 point 1: the menu's fly button. The acting game presses it and moves its character forward for 2 s by movement input (as the
	// keys do); the character must stay in the air and never be put back. The server's copy (host or dedicated server) flies too and is
	// where the client went. A second press walks again. Taking off without the tool in hand is refused by the server.
	struct FFly
	{
		FVector Start = FVector::ZeroVector;
		double Farthest = 0.0;
		double LowestZ = 0.0;
		double Since = 0.0;
		FVector ServerStart = FVector::ZeroVector;
		int32 RejectsBefore = 0;
	};

	void AddFly(FRun& R)
	{
		TSharedRef<FFly> F = MakeShared<FFly>();
		AddToolMenu(R);
		R.Steps.Add([](FRun& Self)
		{
			UAreaActionsPopupBase* Popup = GetPopup(Self);
			Self.Check(CallText(Popup, TEXT("GetFlyText")) == TEXT("Enable flying"), FString::Printf(TEXT("fly button reads '%s'"), *CallText(Popup, TEXT("GetFlyText"))));
			Self.Check(PressButton(Popup, TEXT("FlyButton")), TEXT("FlyButton pressed"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		AddChecks(R, [](FRun& Self)
		{
			const AFGCharacterPlayer* Character = GetCharacter(Self);
			const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
			Self.Check(Movement && Movement->MovementMode == MOVE_Flying && Movement->bCheatFlying, FString::Printf(TEXT("%s: the acting player's character flies (mode %d, cheat flying %d)"),
				Self.bClient ? TEXT("client") : Self.bMp ? TEXT("server") : TEXT("game"), Movement ? static_cast<int32>(Movement->MovementMode) : -1, Movement && Movement->bCheatFlying));
			return EStep::Next;
		});
		AddHost(R, [F](FRun& Self)
		{
			F->ServerStart = GetCharacter(Self)->GetActorLocation();
			return EStep::Next;
		});
		R.Steps.Add([F](FRun& Self)
		{
			AFGCharacterPlayer* Character = GetCharacter(Self);
			const FVector At = Character->GetActorLocation();
			if (F->Since == 0.0)
			{
				F->Since = Self.Now();
				F->Start = At;
				F->LowestZ = At.Z;
				// Up first, then along the camera's heading: a flying character goes where the input points
			}
			const double Elapsed = Self.Now() - F->Since;
			const FVector Heading = FRotator(0.0, Character->GetControlRotation().Yaw, 0.0).Vector();
			Character->AddMovementInput(Elapsed < 0.5 ? FVector::UpVector : Heading, 1.0f, true);
			const double Along = FVector::DotProduct(At - F->Start, Heading);
			F->Farthest = FMath::Max(F->Farthest, Along);
			F->LowestZ = FMath::Min(F->LowestZ, At.Z);
			if (Elapsed < 2.5)
			{
				return EStep::Wait;
			}
			Self.Step(FString::Printf(TEXT("flew from %s to %s"), *F->Start.ToCompactString(), *At.ToCompactString()));
			Self.Check(F->Farthest >= 400.0, FString::Printf(TEXT("the character moved %.0f cm along its heading (at least 400)"), F->Farthest));
			Self.Check(At.Z >= F->Start.Z - 30.0 && F->LowestZ >= F->Start.Z - 30.0, FString::Printf(TEXT("it did not fall: Z %.0f, lowest %.0f, start %.0f"), At.Z, F->LowestZ, F->Start.Z));
			Self.Check(Along >= F->Farthest - 50.0, FString::Printf(TEXT("never put back: ends %.0f cm out, farthest %.0f"), Along, F->Farthest));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		AddHost(R, [F](FRun& Self)
		{
			const AFGCharacterPlayer* Character = GetCharacter(Self);
			const double Moved = FVector::Dist2D(Character->GetActorLocation(), F->ServerStart);
			Self.Check(AAreaActionsEquipment::IsFlying(Character) && Moved >= 400.0, FString::Printf(TEXT("the server's copy of the character flies and moved %.0f cm (at least 400), at %s"),
				Moved, *Character->GetActorLocation().ToCompactString()));
			return EStep::Next;
		});
		AddOpenMenu(R);
		R.Steps.Add([](FRun& Self)
		{
			UAreaActionsPopupBase* Popup = GetPopup(Self);
			Self.Check(CallText(Popup, TEXT("GetFlyText")) == TEXT("Disable flying"), FString::Printf(TEXT("fly button reads '%s'"), *CallText(Popup, TEXT("GetFlyText"))));
			Self.Check(PressButton(Popup, TEXT("FlyButton")), TEXT("FlyButton pressed again"));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		AddChecks(R, [](FRun& Self)
		{
			const AFGCharacterPlayer* Character = GetCharacter(Self);
			const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
			Self.Check(Movement && Movement->MovementMode != MOVE_Flying && !Movement->bCheatFlying, FString::Printf(TEXT("%s: walking again (mode %d)"),
				Self.bClient ? TEXT("client") : Self.bMp ? TEXT("server") : TEXT("game"), Movement ? static_cast<int32>(Movement->MovementMode) : -1));
			return EStep::Next;
		});
		AddCloseMenu(R);
		// Rule 4: the server takes nobody off the ground without the tool in hand
		AddPutToolAway(R);
		AddHost(R, [F](FRun& Self)
		{
			F->RejectsBefore = UAreaActionsRCO::RejectCount;
			return EStep::Next;
		});
		R.Steps.Add([](FRun& Self)
		{
			Self.Step(TEXT("asking the server to fly with no tool in hand"));
			UAreaActionsRCO::Of(Self.GetPlayer())->ServerSetFlying(true);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		AddHost(R, [F](FRun& Self)
		{
			const AFGCharacterPlayer* Character = GetCharacter(Self);
			Self.Check(UAreaActionsRCO::RejectCount - F->RejectsBefore == 1 && !AAreaActionsEquipment::IsFlying(Character), FString::Printf(TEXT("refused: reject lines %d (want 1), flying %d"),
				UAreaActionsRCO::RejectCount - F->RejectsBefore, AAreaActionsEquipment::IsFlying(Character)));
			return EStep::Next;
		});
	}

	bool TutorialShown(const FRun& R)
	{
		const UUserWidget* Popup = GetPopup(R);
		const UWidget* Steps = Popup ? Popup->GetWidgetFromName(TEXT("TutorialWidget")) : nullptr;
		return Steps && Steps->GetVisibility() != ESlateVisibility::Collapsed && Steps->GetVisibility() != ESlateVisibility::Hidden;
	}

	FString SettingsText()
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *AreaActionsSettings::GetPath());
		return Text.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
	}

	// The menu opened after the tool was taken: the help shows by itself or not
	void AddCheckHelpOnOpen(FRun& R, bool bWant, const TCHAR* When)
	{
		AddOpenMenu(R);
		R.Steps.Add([bWant, When](FRun& Self)
		{
			Self.Check(TutorialShown(Self) == bWant, FString::Printf(TEXT("%s: the help %s by itself (want %s), step %d"), When, TutorialShown(Self) ? TEXT("shows") : TEXT("does not show"),
				bWant ? TEXT("shown") : TEXT("not shown"), IntValue(GetPopup(Self), TEXT("HelpIdx"))));
			return EStep::Next;
		});
	}

	// AA-060 point 2: the help in the menu, its Next, Prev and Done buttons; after Done it does not come back by itself (another taking of
	// the tool, a save and a load), the Help button shows it always. The player's settings file keeps Done (D21).
	void AddTutorial(FRun& R)
	{
		AddEquipTool(R);
		AddCheckHelpOnOpen(R, true, TEXT("first menu"));
		R.Steps.Add([](FRun& Self)
		{
			UAreaActionsPopupBase* Popup = GetPopup(Self);
			const int32 Count = ArrayNum(Popup, TEXT("HelpMessages"));
			Self.Check(Count >= 2 && IntValue(Popup, TEXT("HelpIdx")) == 0, FString::Printf(TEXT("help steps %d, at step %d"), Count, IntValue(Popup, TEXT("HelpIdx"))));
			PressButton(Popup, TEXT("TutorialNext"));
			const int32 AfterNext = IntValue(Popup, TEXT("HelpIdx"));
			PressButton(Popup, TEXT("TutorialPrev"));
			const int32 AfterPrev = IntValue(Popup, TEXT("HelpIdx"));
			Self.Check(AfterNext == 1 && AfterPrev == 0, FString::Printf(TEXT("Next goes to step %d (want 1), Prev back to %d (want 0)"), AfterNext, AfterPrev));
			Self.Check(!SettingsText().Contains(TEXT("HelpDone=True")), FString::Printf(TEXT("before Done the settings file has no HelpDone: '%s'"), *SettingsText()));
			for (int32 Step = 0; Step < Count && TutorialShown(Self); ++Step)
			{
				PressButton(Popup, TEXT("TutorialNext"));
			}
			Self.Check(!TutorialShown(Self), TEXT("Done on the last step closes the help"));
			Self.Check(SettingsText().Contains(TEXT("HelpDone=True")), FString::Printf(TEXT("settings file %s: '%s'"), *AreaActionsSettings::GetPath(), *SettingsText()));
			return EStep::Next;
		});
		AddCloseMenu(R);
		AddPutToolAway(R);
		AddEquipTool(R);
		AddCheckHelpOnOpen(R, false, TEXT("tool taken again"));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(PressButton(GetPopup(Self), TEXT("HelpButton")) && TutorialShown(Self) && IntValue(GetPopup(Self), TEXT("HelpIdx")) == 0, TEXT("Help shows the help from its first step"));
			return EStep::Next;
		});
		AddCloseMenu(R);
		if (!R.bMp)
		{
			AddSaveAndLoad(R, TEXT("AATest_Tutorial"));
			AddEquipTool(R);
			AddCheckHelpOnOpen(R, false, TEXT("after a save and a load"));
			AddCloseMenu(R);
			AddDeleteSave(R, TEXT("AATest_Tutorial"));
		}
	}

	// AA-060 point 5: the tool's schematic is given in every world without research, has its icon, and its recipe is made at the Crafting
	// Bench or the Equipment Workshop; checked by the server and by a client
	void AddCheckSchematic(FRun& R, const TCHAR* When)
	{
		AddChecks(R, [When](FRun& Self)
		{
			UClass* Schematic = LoadClass<UFGSchematic>(nullptr, TEXT("/AreaActions/Schematic_AreaActions.Schematic_AreaActions_C"));
			TSubclassOf<UFGRecipe> ToolRecipe = LoadClass<UFGRecipe>(nullptr, TEXT("/AreaActions/Equipment/AreaActions/Recipe_AreaActions.Recipe_AreaActions_C"));
			AFGSchematicManager* Schematics = AFGSchematicManager::Get(Self.GetWorld());
			AFGRecipeManager* Recipes = AFGRecipeManager::Get(Self.GetWorld());
			Self.Check(Schematic && Schematics && Schematics->IsSchematicPurchased(Schematic), FString::Printf(TEXT("%s: schematic %s given"), When, *GetNameSafe(Schematic)));
			Self.Check(ToolRecipe && Recipes && Recipes->IsRecipeAvailable(ToolRecipe), FString::Printf(TEXT("%s: recipe %s available"), When, *GetNameSafe(ToolRecipe)));
			const FSlateBrush Icon = Schematic ? UFGSchematic::GetItemIcon(Schematic) : FSlateBrush();
			Self.Check(Icon.GetResourceObject() != nullptr, FString::Printf(TEXT("%s: schematic icon %s"), When, *GetNameSafe(Icon.GetResourceObject())));
			TArray<FString> Producers;
			for (const TSubclassOf<UObject>& Producer : ToolRecipe ? UFGRecipe::GetProducedIn(ToolRecipe) : TArray<TSubclassOf<UObject>>())
			{
				Producers.Add(GetNameSafe(Producer));
			}
			const FString Joined = FString::Join(Producers, TEXT(", "));
			// As in the original's recipe: the Equipment Workshop
			Self.Check(Joined.Contains(TEXT("Workshop")) || Joined.Contains(TEXT("WorkBench")), FString::Printf(TEXT("%s: the tool is made in %s"), When, *Joined));
			return EStep::Next;
		});
	}

	void AddSchematic(FRun& R)
	{
		AddCheckSchematic(R, TEXT("new game"));
		if (!R.bMp)
		{
			AddSaveAndLoad(R, TEXT("AATest_Schematic"));
			AddCheckSchematic(R, TEXT("loaded save"));
			AddDeleteSave(R, TEXT("AATest_Schematic"));
		}
	}

	// AA-060 point 6: the middle mouse button on a building, an actor and a lightweight, puts the Build Gun in hand building that class
	void AddMiddleMouseOn(FRun& R, TSharedRef<FVector> Target, const TCHAR* RecipeName, const TCHAR* What)
	{
		AddEquipTool(R);
		R.Steps.Add([Target](FRun& Self)
		{
			LookAt(Self, *Target);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(3));
		R.Steps.Add([What](FRun& Self)
		{
			Self.Step(FString::Printf(TEXT("middle mouse button on the %s"), What));
			PressKey(Self, EKeys::MiddleMouseButton);
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(15));
		R.Steps.Add([RecipeName, What](FRun& Self)
		{
			AFGCharacterPlayer* Character = GetCharacter(Self);
			AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
			const UFGBuildGunStateBuild* Build = Gun ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
			const TSubclassOf<UFGRecipe> Active = Build ? Build->GetActiveRecipe() : nullptr;
			Self.Check(Gun && Gun->IsEquipped() && Active && Active->GetName().StartsWith(RecipeName), FString::Printf(TEXT("%s: Build Gun in hand %d, state %s, recipe %s (want %s)"), What,
				Gun && Gun->IsEquipped(), Gun ? *GetNameSafe(Gun->GetCurrentState()) : TEXT("-"), *GetNameSafe(Active), RecipeName));
			if (Gun && Gun->IsEquipped())
			{
				Character->ToggleBuildGun();
			}
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
	}

	void AddMiddleMouse(FRun& R)
	{
		TSharedRef<FVector> Machine = MakeShared<FVector>(), Foundation = MakeShared<FVector>();
		AddEquipTool(R);
		R.Steps.Add([Machine, Foundation](FRun& Self)
		{
			APawn* Pawn = Self.GetPlayer()->GetPawn();
			const FVector Forward = Pawn->GetActorForwardVector();
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
			FVector At;
			GroundBelow(Self.GetWorld(), Pawn->GetActorLocation() + Forward * 1500.0 - Right * 800.0, At);
			AFGBuildable* Constructor = SpawnGameBuildable(Self, TEXT("Build_ConstructorMk1"), TEXT("Recipe_ConstructorMk1"), At + FVector(0, 0, 50));
			*Machine = At + FVector(0, 0, 300);
			GroundBelow(Self.GetWorld(), Pawn->GetActorLocation() + Forward * 1500.0 + Right * 800.0, At);
			AreaActionsBuildings::FSnapshot Piece;
			Piece.Class = GameClass(TEXT("Build_Foundation_8x1_01"));
			Piece.BuiltWith = RecipeOfClass(Self, Piece.Class, TEXT("Recipe_Foundation_8x1_01"));
			const bool bFoundation = Piece.Class && AreaActionsBuildings::Spawn(Self.GetWorld(), Piece, FTransform(At + FVector(0, 0, 50)));
			*Foundation = At + FVector(0, 0, 100);
			// A new game has neither recipe yet: the middle mouse button only offers what the player can build
			if (AFGRecipeManager* Recipes = AFGRecipeManager::Get(Self.GetWorld()))
			{
				for (const TCHAR* Name : { TEXT("Recipe_ConstructorMk1"), TEXT("Recipe_Foundation_8x1_01") })
				{
					if (const TSubclassOf<UFGRecipe> Made(GameClass(Name)); Made && !Recipes->IsRecipeAvailable(Made))
					{
						Recipes->AddAvailableRecipe(Made);
					}
				}
			}
			Self.Check(Constructor && bFoundation, FString::Printf(TEXT("a Constructor and a foundation in front: %d, %d"), Constructor != nullptr, bFoundation));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(10));
		AddMiddleMouseOn(R, Machine, TEXT("Recipe_ConstructorMk1"), TEXT("Constructor (actor)"));
		AddMiddleMouseOn(R, Foundation, TEXT("Recipe_Foundation_8x1_01"), TEXT("foundation (lightweight)"));
	}

	// AA-060 point 7: the selection comes back after the tool was put away and taken again, twice
	void AddSelectionBackup(FRun& R)
	{
		TSharedRef<TArray<FVector>> Before = MakeShared<TArray<FVector>>();
		AddEquipTool(R);
		AddThreeCorners(R);
		R.Steps.Add([Before](FRun& Self)
		{
			*Before = VectorArray(GetTool(Self), TEXT("CornerLocations"));
			Self.Check(Before->Num() == 3, FString::Printf(TEXT("corners before: %d"), Before->Num()));
			return EStep::Next;
		});
		for (int32 Round = 1; Round <= 2; ++Round)
		{
			AddPutToolAway(R);
			AddEquipTool(R);
			R.Steps.Add([Before, Round](FRun& Self)
			{
				AAreaActionsEquipment* Tool = GetTool(Self);
				const TArray<FVector> After = VectorArray(Tool, TEXT("CornerLocations"));
				bool bSame = After.Num() == Before->Num();
				for (int32 Index = 0; bSame && Index < After.Num(); ++Index)
				{
					bSame = After[Index].Equals((*Before)[Index], 1.0);
				}
				int32 Live = 0;
				if (FArrayProperty* Property = FindFProperty<FArrayProperty>(Tool->GetClass(), TEXT("CornerIndicators")))
				{
					FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(Tool));
					for (int32 Index = 0; Index < Array.Num(); ++Index)
					{
						Live += IsValid(CastField<FObjectPropertyBase>(Property->Inner)->GetObjectPropertyValue(Array.GetRawPtr(Index))) ? 1 : 0;
					}
				}
				int32 Backups = 0;
				for (TActorIterator<AActor> It(Self.GetWorld()); It; ++It)
				{
					Backups += It->GetClass()->GetName().StartsWith(TEXT("AreaActionsBackup")) ? 1 : 0;
				}
				Self.Step(FString::Printf(TEXT("round %d: backup actors in the world %d"), Round, Backups));
				Self.Check(bSame, FString::Printf(TEXT("round %d: corners back, %d of %d, same places %d"), Round, After.Num(), Before->Num(), bSame));
				Self.Check(Live == 3 && ArrayNum(Tool, TEXT("AreaWalls")) == 3, FString::Printf(TEXT("round %d: corner indicators %d, walls %d (want 3, 3)"), Round, Live, ArrayNum(Tool, TEXT("AreaWalls"))));
				return EStep::Next;
			});
		}
	}

	// AA-096 (3): the tool's Blueprint WasUnEquipped while the game still has the tool equipped; the ticks after it must not run the
	// Blueprint's WasEquipped (no windows, no indicators of a tool put away)
	void AddHolsterTick(FRun& R)
	{
		AddEquipTool(R);
		R.Steps.Add([](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			Self.Check(Cast<UUserWidget>(ObjectValue(Tool, TEXT("StatusWidget"))) != nullptr, TEXT("status window while the tool is in hand"));
			CallNoParams(Tool, TEXT("WasUnEquipped"));
			Self.Step(FString::Printf(TEXT("WasUnEquipped called; the tool still equipped %d, local instigator %d"), Tool->IsEquipped(), Tool->IsLocalInstigator()));
			return EStep::Next;
		});
		R.Steps.Add(WaitFrames(30));
		R.Steps.Add([](FRun& Self)
		{
			AAreaActionsEquipment* Tool = GetTool(Self);
			const UUserWidget* Status = Cast<UUserWidget>(ObjectValue(Tool, TEXT("StatusWidget")));
			Self.Check(!Status && !TopWindow(Self), FString::Printf(TEXT("30 ticks later: status window %s, windows %s (want none)"), *GetNameSafe(Status), *GetNameSafe(TopWindow(Self))));
			return EStep::Next;
		});
		AddPutToolAway(R);
		AddEquipTool(R);
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(Cast<UUserWidget>(ObjectValue(GetTool(Self), TEXT("StatusWidget"))) != nullptr, TEXT("taken again: status window back"));
			return EStep::Next;
		});
	}

	// AA-096 (2): opening and closing a Fill preview of 300 holograms (their sounds stopped one by one) within W5
	void AddPreviewHolograms300(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		AddCopyScene(R, S, C, BuildSingle);
		AddOpenAction(R, TEXT("Fill"));
		R.Steps.Add(WaitCalm(30));
		R.Steps.Add([](FRun& Self)
		{
			Self.MaxFrameMs = 0.0;
			Self.bMeasureFrames = true;
			return EStep::Next;
		});
		AddFillWindow(R, FVector(1, 1, 0), FIntVector(31, 10, 1), FVector2D(0, 0), false, TEXT("PreviewButton"));
		R.Steps.Add(WaitFrames(10));
		R.Steps.Add([](FRun& Self)
		{
			Self.Check(PreviewHolograms(Self) == 300, FString::Printf(TEXT("preview holograms %d (want 300)"), PreviewHolograms(Self)));
			return EStep::Next;
		});
		AddCloseWithX(R, TEXT("Widget_Fill"));
		R.Steps.Add(WaitFrames(10));
		R.Steps.Add([](FRun& Self)
		{
			Self.bMeasureFrames = false;
			Self.Check(PreviewHolograms(Self) == 0, FString::Printf(TEXT("after Cancel: holograms %d"), PreviewHolograms(Self)));
			Self.Check(Self.MaxFrameMs <= 200.0, FString::Printf(TEXT("longest frame while the preview opened and closed: %.1f ms (W5: at most 200)"), Self.MaxFrameMs));
			return EStep::Next;
		});
	}

	// AA-091 (3): which buildings of the Build Gun have no meshes the preview can draw (no static mesh component, no instance meshes), so
	// their copies get the box. Lists them; nothing else.
	void AddPreviewBoxProbe(FRun& R)
	{
		R.Steps.Add([](FRun& Self)
		{
			TArray<FAssetData> Assets;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(TEXT("/Game/FactoryGame")), Assets, true);
			TSet<UClass*> Seen;
			int32 RecipeAssets = 0, BuildGunRecipes = 0;
			TArray<FString> Boxed;
			int32 Classes = 0;
			for (const FAssetData& Asset : Assets)
			{
				const FString Name = Asset.AssetName.ToString();
				FString Short = Name;
				Short.RemoveFromEnd(TEXT("_C"));
				UClass* RecipeClass = Short.StartsWith(TEXT("Recipe_")) ? LoadClass<UFGRecipe>(nullptr, *(Asset.PackageName.ToString() + TEXT(".") + Short + TEXT("_C"))) : nullptr;
				if (!RecipeClass)
				{
					continue;
				}
				++RecipeAssets;
				const bool bBuildGun = UFGRecipe::GetProducedIn(RecipeClass).ContainsByPredicate([](const TSubclassOf<UObject>& Producer) { return Producer && Producer->GetName().Contains(TEXT("BuildGun")); });
				BuildGunRecipes += bBuildGun ? 1 : 0;
				for (const FItemAmount& Product : bBuildGun ? UFGRecipe::GetProducts(RecipeClass) : TArray<FItemAmount>())
				{
					const TSubclassOf<UFGBuildingDescriptor> Descriptor(Product.ItemClass.Get());
					UClass* Class = Descriptor ? UFGBuildingDescriptor::GetBuildableClass(Descriptor).Get() : nullptr;
					if (!Class || Seen.Contains(Class) || AreaActionsBuildings::ShouldIgnore(Class))
					{
						continue;
					}
					Seen.Add(Class);
					++Classes;
					const AFGBuildable* Default = Class->GetDefaultObject<AFGBuildable>();
					int32 Meshes = 0;
					TInlineComponentArray<UStaticMeshComponent*> Native(Default);
					for (const UStaticMeshComponent* Mesh : Native)
					{
						Meshes += Mesh->GetStaticMesh() && Mesh->IsVisible() && !Mesh->IsA<UInstancedStaticMeshComponent>() ? 1 : 0;
					}
					for (UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class); Generated; Generated = Cast<UBlueprintGeneratedClass>(Generated->GetSuperClass()))
					{
						for (const USCS_Node* Node : Generated->SimpleConstructionScript ? Generated->SimpleConstructionScript->GetAllNodes() : TArray<USCS_Node*>())
						{
							const UStaticMeshComponent* Mesh = Node ? Cast<UStaticMeshComponent>(Node->ComponentTemplate) : nullptr;
							Meshes += Mesh && Mesh->GetStaticMesh() && Mesh->IsVisible() && !Mesh->IsA<UInstancedStaticMeshComponent>() ? 1 : 0;
						}
					}
					if (const UAbstractInstanceDataObject* Data = Default->GetLightweightInstanceData())
					{
						Meshes += Algo::CountIf(Data->GetInstanceData(), [](const FInstanceData& Instance) { return Instance.StaticMesh != nullptr; });
					}
					if (Meshes == 0)
					{
						// The preview's box branch draws only when the class gives a box without an actor
						AreaActionsBuildings::FSnapshot Snapshot;
						Snapshot.Class = Class;
						FBox Box;
						Boxed.Add(FString::Printf(TEXT("%s (box %d)"), *Class->GetName(), AreaActionsBuildings::GetLocalBox(Snapshot, Box) ? 1 : 0));
					}
				}
			}
			Self.Step(FString::Printf(TEXT("recipe assets %d, of the Build Gun %d"), RecipeAssets, BuildGunRecipes));
			Self.Step(FString::Printf(TEXT("Build Gun classes %d, without meshes the preview draws %d: %s"), Classes, Boxed.Num(), *FString::Join(Boxed, TEXT(", "))));
			Self.Check(Classes > 100, FString::Printf(TEXT("classes looked at: %d"), Classes));
			return EStep::Next;
		});
	}

	// AA-092 (1): "No Build Cost": the window says the cost does not come back and has OK and Cancel only; Cancel leaves the world, OK
	// dismantles with the contents in the crates (as DismantleNoCost)
	void AddDismantleNoCostWindow(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		D->bNoCost = true;
		AddHost(R, [](FRun& Self)
		{
			AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>();
			if (State)
			{
				State->SetCheatNoCost(true);
			}
			Self.Check(State && State->GetCheatNoCost(), TEXT("No Build Cost on"));
			return EStep::Next;
		});
		AddDismantleScene(R, S, D);
		AddOpenAction(R, TEXT("MassDismantle"));
		AddOnWindow(R, TEXT("Widget_MessageYesNo"), [](FRun& Self, UUserWidget* Window)
		{
			FStrProperty* Message = FindFProperty<FStrProperty>(Window->GetClass(), TEXT("Message"));
			const FString Text = Message ? Message->GetPropertyValue_InContainer(Window) : FString();
			Self.Check(Text.Contains(TEXT("No Build Cost")) && Text.Contains(TEXT("does not come back")), FString::Printf(TEXT("the window says: %s"), *Text));
			Self.Check(!Window->GetWidgetFromName(TEXT("ChoiceButton2")), TEXT("two buttons only"));
			Self.Check(PressButton(Window, TEXT("NoButton")), TEXT("Cancel pressed"));
		});
		R.Steps.Add(WaitFrames(5));
		AddChecks(R, [S, D](FRun& Self)
		{
			const int32 Left = Algo::CountIf(S->Inside, [](const TWeakObjectPtr<AFGBuildable>& Weak) { return IsValid(Weak.Get()); });
			Self.Check(Left == S->Inside.Num() && !ActiveAction(Self) && NewCrates(Self, *S).IsEmpty(), FString::Printf(TEXT("Cancel: actors %d of %d stay, no crate, no action"), Left, S->Inside.Num()));
			return EStep::Next;
		});
		AddDismantle(R, S, TEXT("YesButton"), TEXT("OK"));
		AddCheckGone(R, S, D);
		AddCheckRefundCrate(R, S, D);
		AddHost(R, [](FRun& Self)
		{
			if (AFGGameState* State = Self.GetWorld()->GetGameState<AFGGameState>())
			{
				State->SetCheatNoCost(false);
			}
			return EStep::Next;
		});
	}

	// AA-092 (2): "Cost only": the crates hold the build cost and the machines' power shards and somersloops, none of what machines,
	// containers and crates held otherwise
	void AddDismantleCostOnly(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FDismantleRef D = MakeShared<FDismantleState>();
		TSharedRef<FCounts> Upgrades = MakeShared<FCounts>();
		AddScene(R, S, 4000.0, [D, Upgrades](FRun& Self, FScene& Scene)
		{
			BuildDismantleBasic(Self, Scene, *D);
			for (const TWeakObjectPtr<AFGBuildable>& Weak : Scene.Inside)
			{
				AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Weak.Get());
				UFGInventoryComponent* Potential = Machine ? Machine->GetPotentialInventory() : nullptr;
				if (!Potential)
				{
					continue;
				}
				// A new game has unlocked no shard or somersloop slot: the slots are opened here past the game's filters (as AddWater)
				Self.Step(FString::Printf(TEXT("potential inventory of %s: %d slots"), *Machine->GetName(), Potential->GetSizeLinear()));
				if (Potential->GetSizeLinear() < 2)
				{
					Potential->Resize(2);
				}
				const FItemFilter ItemFilter = Potential->mItemFilter;
				const FFormFilter FormFilter = Potential->mFormFilter;
				Potential->mItemFilter.Unbind();
				Potential->mFormFilter.Unbind();
				int32 Slot = 0;
				for (const TCHAR* Name : { TEXT("Desc_CrystalShard"), TEXT("Desc_WAT1") })
				{
					const TSubclassOf<UFGItemDescriptor> Upgrade(GameClass(Name));
					const TSubclassOf<UFGItemDescriptor> Allowed = Potential->GetAllowedItemOnIndex(Slot);
					Potential->SetAllowedItemOnIndex(Slot, nullptr);
					const int32 Added = Upgrade ? Potential->AddStackToIndex(Slot, FInventoryStack(1, Upgrade), true) : 0;
					Potential->SetAllowedItemOnIndex(Slot, Allowed);
					Self.Step(FString::Printf(TEXT("%s into %s slot %d: %d"), Name, *Machine->GetName(), Slot, Added));
					++Slot;
				}
				Potential->mItemFilter = ItemFilter;
				Potential->mFormFilter = FormFilter;
				TArray<FInventoryStack> Stacks;
				Potential->GetInventoryStacks(Stacks);
				AddSolids(*Upgrades, Stacks);
			}
			Self.Check(Total(*Upgrades) == 2, FString::Printf(TEXT("upgrades in the Constructor: %s"), *ItemsText(*Upgrades)));
		});
		AddBoth(R, [S, D, Upgrades](FRun& Self)
		{
			if (!Self.bClient)
			{
				RecordRefund(Self, *S, *D);
				// The refund less what the buildings hold, with the upgrades back
				for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : D->Contents)
				{
					D->Expected.FindOrAdd(Entry.Key) -= Entry.Value;
				}
				for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : *Upgrades)
				{
					D->Expected.FindOrAdd(Entry.Key) += Entry.Value;
				}
				for (auto It = D->Expected.CreateIterator(); It; ++It)
				{
					if (It->Value == 0)
					{
						It.RemoveCurrent();
					}
				}
				Self.Step(FString::Printf(TEXT("cost only: expected %s (%d)"), *ItemsText(D->Expected), Total(D->Expected)));
				Send(Self, MsgExpected, Total(D->Expected));
			}
			CountWorld(Self, D->Pieces);
			D->Pieces.JobsBefore = AAreaActionsServerSubsystem::FinishedJobs;
			return EStep::Next;
		});
		AddDismantle(R, S, TEXT("ChoiceButton2"), TEXT("Cost only"));
		AddCheckGone(R, S, D);
		AddCheckRefundCrate(R, S, D);
	}

	// AA-074: a square of 4 corners clicked as the player does, seen from the player; screenshots as the tool draws it, with the walls
	// made thin and with the corner cylinders hidden, to tell which indicator draws the "X" at the corners
	void AddCornersView(FRun& R)
	{
		AddEquipTool(R);
		TSharedRef<TArray<FVector>> Targets = MakeShared<TArray<FVector>>();
		R.Steps.Add([Targets](FRun& Self)
		{
			APawn* Pawn = Self.GetPlayer()->GetPawn();
			const FVector Center = Pawn->GetActorLocation() + Pawn->GetActorForwardVector() * 1500.0;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const FVector Point = Center + FRotator(0, 45.0 + Index * 90.0, 0).RotateVector(FVector(700, 0, 0));
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
		for (int32 Index = 0; Index < 4; ++Index)
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
			R.Steps.Add([](FRun& Self)
			{
				PressKey(Self, EKeys::LeftMouseButton);
				return EStep::Next;
			});
			R.Steps.Add(WaitFrames(3));
		}
		R.Steps.Add([Targets](FRun& Self)
		{
			Self.Check(ArrayNum(GetTool(Self), TEXT("CornerLocations")) == 4, FString::Printf(TEXT("corners %d (want 4)"), ArrayNum(GetTool(Self), TEXT("CornerLocations"))));
			FVector Middle = FVector::ZeroVector;
			for (const FVector& Point : *Targets)
			{
				Middle += Point / Targets->Num();
			}
			LookAt(Self, Middle);
			for (const TCHAR* Name : { TEXT("CornerIndicators"), TEXT("AreaWalls") })
			{
				if (FArrayProperty* Property = FindFProperty<FArrayProperty>(GetTool(Self)->GetClass(), Name))
				{
					FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(GetTool(Self)));
					for (int32 Index = 0; Index < Array.Num(); ++Index)
					{
						const AActor* Actor = Cast<AActor>(CastField<FObjectPropertyBase>(Property->Inner)->GetObjectPropertyValue(Array.GetRawPtr(Index)));
						Self.Step(FString::Printf(TEXT("%s %d: %s at %s, scale %s"), Name, Index, *GetNameSafe(Actor), Actor ? *Actor->GetActorLocation().ToCompactString() : TEXT("-"),
							Actor ? *Actor->GetActorScale3D().ToCompactString() : TEXT("-")));
					}
				}
			}
			return EStep::Next;
		});
		auto Shot = [](const FString& Name)
		{
			return [Name](FRun& Self)
			{
				const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), FString::Printf(TEXT("AATest_%s.png"), *Name));
				FScreenshotRequest::RequestScreenshot(File, false, false);
				Self.Step(FString::Printf(TEXT("screenshot %s"), *File));
				return EStep::Next;
			};
		};
		auto ForEachIndicator = [](FRun& Self, const TCHAR* Name, TFunctionRef<void(AActor*)> Fn)
		{
			if (FArrayProperty* Property = FindFProperty<FArrayProperty>(GetTool(Self)->GetClass(), Name))
			{
				FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(GetTool(Self)));
				for (int32 Index = 0; Index < Array.Num(); ++Index)
				{
					if (AActor* Actor = Cast<AActor>(CastField<FObjectPropertyBase>(Property->Inner)->GetObjectPropertyValue(Array.GetRawPtr(Index))))
					{
						Fn(Actor);
					}
				}
			}
		};
		auto SetWalls = [ForEachIndicator](FRun& Self, double Thickness)
		{
			ForEachIndicator(Self, TEXT("AreaWalls"), [Thickness](AActor* Wall) { FVector Scale = Wall->GetActorScale3D(); Scale.Y = Thickness; Wall->SetActorScale3D(Scale); });
		};
		auto ShowCorners = [ForEachIndicator](FRun& Self, bool bShow)
		{
			ForEachIndicator(Self, TEXT("CornerIndicators"), [bShow](AActor* Corner) { Corner->SetActorHiddenInGame(!bShow); });
		};
		// Two places: above the area's middle looking down at the first corner, and on the ground inside the area looking at it
		for (int32 View = 0; View < 2; ++View)
		{
			R.Steps.Add([Targets, View](FRun& Self)
			{
				ACharacter* Character = GetCharacter(Self);
				FVector Middle = FVector::ZeroVector;
				for (const FVector& Point : *Targets)
				{
					Middle += Point / Targets->Num();
				}
				AAreaActionsEquipment::ApplyFlying(Character, true);
				const FVector From = View == 0 ? Middle + FVector(0, 0, 1200) : Middle + ((*Targets)[2] - Middle) * 0.5 + FVector(0, 0, 150);
				Character->SetActorLocation(From, false, nullptr, ETeleportType::TeleportPhysics);
				Self.Step(FString::Printf(TEXT("view %d from %s"), View, *From.ToCompactString()));
				return EStep::Next;
			});
			R.Steps.Add(WaitFrames(20));
			R.Steps.Add([Targets](FRun& Self) { LookAt(Self, (*Targets)[0]); return EStep::Next; });
			const FString Prefix = FString::Printf(TEXT("Corners_v%d_"), View);
			R.Steps.Add(WaitFrames(30));
			R.Steps.Add(Shot(Prefix + TEXT("1_asis")));
			R.Steps.Add(WaitFrames(20));
			R.Steps.Add([SetWalls](FRun& Self) { SetWalls(Self, 0.05); return EStep::Next; });
			R.Steps.Add(WaitFrames(30));
			R.Steps.Add(Shot(Prefix + TEXT("2_thinwalls")));
			R.Steps.Add(WaitFrames(20));
			R.Steps.Add([SetWalls, ShowCorners](FRun& Self) { SetWalls(Self, 1.0); ShowCorners(Self, false); return EStep::Next; });
			R.Steps.Add(WaitFrames(30));
			R.Steps.Add(Shot(Prefix + TEXT("3_nocorners")));
			R.Steps.Add(WaitFrames(20));
			R.Steps.Add([ShowCorners](FRun& Self) { ShowCorners(Self, true); return EStep::Next; });
		}
		R.Steps.Add([](FRun& Self) { AAreaActionsEquipment::ApplyFlying(GetCharacter(Self), false); return EStep::Next; });
		R.Steps.Add(WaitFrames(30));
	}

	// AA-091 (1), (3): the classes PreviewBoxProbe found without meshes on their class default (a ladder and foundation passthroughs build
	// their meshes when they are placed), placed for real; with AreaActions.PreviewHolograms 0 every copy is drawn by the preview itself.
	// Placed, they have no meshes the preview finds and no box either, so the preview opens without drawing them
	void AddPreviewBox(FRun& R)
	{
		FSceneRef S = MakeShared<FScene>();
		FCopyRef C = MakeShared<FCopyState>();
		TSharedRef<int32> Boxed = MakeShared<int32>(0);
		AddCopyScene(R, S, C, [Boxed](FRun& Self, FScene& Scene, FCopyState& Copy)
		{
			Copy.Base = Ground(Self, Scene.Center);
			const TCHAR* Names[][2] = { { TEXT("Build_Ladder"), TEXT("Recipe_Ladder") }, { TEXT("Build_FoundationPassthrough_Lift"), TEXT("Recipe_FoundationPassthrough_Lift") },
				{ TEXT("Build_FoundationPassthrough_Pipe"), TEXT("Recipe_FoundationPassthrough_Pipe") } };
			double X = -1500.0;
			for (const auto& Name : Names)
			{
				AFGBuildable* Actor = AddActor(Self, Scene, Copy, Name[0], Name[1], FTransform(Copy.Base + FVector(X, 0, 50)));
				X += 1500.0;
				if (!Actor)
				{
					continue;
				}
				AreaActionsBuildings::FSnapshot Snapshot;
				Snapshot.Class = Actor->GetClass();
				Snapshot.Actor = Actor;
				Snapshot.Transform = Actor->GetActorTransform();
				TArray<TPair<UStaticMesh*, FTransform>> Meshes;
				AreaActionsBuildings::GetMeshes(Self.GetWorld(), Snapshot, Meshes);
				FBox Box;
				const bool bBox = AreaActionsBuildings::GetLocalBox(Snapshot, Box);
				Self.Step(FString::Printf(TEXT("%s placed: %d meshes the preview draws, box %d %s, clearance %s"), Name[0], Meshes.Num(), bBox ? 1 : 0,
					*Box.ToString(), *Actor->GetCombinedClearanceBox().ToString()));
				*Boxed += Meshes.IsEmpty() ? 1 : 0;
			}
			AddControlStorages(Self, Scene);
		});
		R.Steps.Add([](FRun& Self)
		{
			IConsoleManager::Get().FindConsoleVariable(TEXT("AreaActions.PreviewHolograms"))->Set(0);
			return EStep::Next;
		});
		AddOpenAction(R, TEXT("Copy"));
		AddCopyWindow(R, FVector(0, 30, 0), 0.0, false, TEXT("PreviewButton"));
		R.Steps.Add(WaitFrames(10));
		R.Steps.Add([Boxed](FRun& Self)
		{
			// AA-091 (3): no game class reaches the box branch (no meshes and no box, PreviewBoxProbe), so the branch is gone
			int32 Batches = 0;
			int32 Previews = 0;
			for (TActorIterator<AAreaActionsPreview> It(Self.GetWorld()); It; ++It)
			{
				Batches += It->FindComponentByClass<ULineBatchComponent>() ? 1 : 0;
				++Previews;
			}
			Self.Check(Previews == 1 && *Boxed == 3, FString::Printf(TEXT("preview of %d buildings without meshes open: %d previews"), *Boxed, Previews));
			Self.Check(Batches == 0, FString::Printf(TEXT("no box lines in the preview: %d line batches"), Batches));
			return EStep::Next;
		});
		AddCloseWithX(R, TEXT("Widget_Copy"));
		R.Steps.Add([](FRun& Self)
		{
			IConsoleManager::Get().FindConsoleVariable(TEXT("AreaActions.PreviewHolograms"))->Set(300);
			return EStep::Next;
		});
	}

	bool BuildScenario(FRun& R)
	{
		if (R.Name == TEXT("MP_Smoke"))
		{
			// The client takes the tool and opens its menu; R10: what the host's game makes of it is counted by Watch
			FSceneRef S = MakeShared<FScene>();
			AddToolMenu(R);
			R.Steps.Add([S](FRun& Self)
			{
				PlaceScene(Self, *S, 1000.0, 500.0);
				Self.Check(SetToolArea(Self, *S), TEXT("tool area set"));
				return EStep::Next;
			});
			R.Steps.Add(WaitFrames(60));
			return true;
		}
		if (R.Scenario == TEXT("CornersView"))
		{
			AddCornersView(R);
			return true;
		}
		if (R.Scenario == TEXT("PreviewBox"))
		{
			AddPreviewBox(R);
			return true;
		}
		if (R.Scenario == TEXT("Fly"))
		{
			AddFly(R);
			return true;
		}
		if (R.Scenario == TEXT("Tutorial"))
		{
			R.StepTimeoutSec = 60.0;
			AddTutorial(R);
			return true;
		}
		if (R.Scenario == TEXT("Schematic"))
		{
			R.StepTimeoutSec = 60.0;
			AddSchematic(R);
			return true;
		}
		if (R.Scenario == TEXT("MiddleMouse"))
		{
			AddMiddleMouse(R);
			return true;
		}
		if (R.Scenario == TEXT("SelectionBackup"))
		{
			AddSelectionBackup(R);
			return true;
		}
		if (R.Scenario == TEXT("HolsterTick"))
		{
			AddHolsterTick(R);
			return true;
		}
		if (R.Scenario == TEXT("PreviewHolograms300"))
		{
			AddPreviewHolograms300(R);
			return true;
		}
		if (R.Scenario == TEXT("PreviewBoxProbe"))
		{
			AddPreviewBoxProbe(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleNoCostWindow"))
		{
			AddDismantleNoCostWindow(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleCostOnly"))
		{
			AddDismantleCostOnly(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleBasic"))
		{
			AddDismantleBasic(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleSweep"))
		{
			AddDismantleSweep(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleNoCrate"))
		{
			AddDismantleNoCrate(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleNoCost"))
		{
			AddDismantleNoCost(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleProtected"))
		{
			AddDismantleProtected(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleSaveLoad"))
		{
			R.StepTimeoutSec = 60.0;
			AddDismantleSaveLoad(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleLarge"))
		{
			R.StepTimeoutSec = 60.0;
			AddDismantleLarge(R);
			return true;
		}
		if (R.Scenario == TEXT("SaveDuringJob"))
		{
			R.StepTimeoutSec = 60.0;
			AddSaveDuringJob(R, EJobKind::SetRecipe, 301);
			AddSaveDuringJob(R, EJobKind::Clear, 302);
			AddSaveDuringJob(R, EJobKind::Dismantle, 303);
			return true;
		}
		if (R.Scenario == TEXT("CrateItemState"))
		{
			AddCrateItemState(R, false, 55.5f, 0.0);
			AddCrateItemState(R, true, 66.5f, 12000.0);
			return true;
		}
		if (R.Scenario == TEXT("CrateSplit"))
		{
			AddCrateSplit(R, false, 0.0);
			if (!R.bMp)
			{
				AddCrateSplit(R, true, 12000.0);
			}
			return true;
		}
		if (R.Scenario == TEXT("PreviewHold"))
		{
			AddPreviewHold(R);
			return true;
		}
		if (R.Scenario == TEXT("PreviewSound"))
		{
			AddPreviewSound(R);
			return true;
		}
		if (R.Scenario == TEXT("DisconnectJob"))
		{
			AddDisconnectJob(R);
			return true;
		}
		if (R.Scenario == TEXT("DisconnectPreview"))
		{
			AddDisconnectPreview(R);
			return true;
		}
		if (R.Scenario == TEXT("DisconnectCopy") || R.Scenario == TEXT("DisconnectCopyRefund"))
		{
			AddDisconnectCopy(R, R.Scenario == TEXT("DisconnectCopyRefund"));
			return true;
		}
		if (R.Scenario == TEXT("ServerSave"))
		{
			AddServerSave(R);
			return true;
		}
		if (R.Name == TEXT("ServerRestart"))
		{
			AddServerRestart(R);
			return true;
		}
		if (R.Name == TEXT("ServerBase"))
		{
			AddServerBase(R);
			return true;
		}
		if (R.Scenario == TEXT("CrateMessages"))
		{
			AddCrateMessages(R);
			return true;
		}
		if (R.Scenario == TEXT("DismantleProbe"))
		{
			R.StepTimeoutSec = 60.0;
			AddDismantleProbe(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyBasic"))
		{
			AddCopyBasic(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyRotate"))
		{
			AddCopyRotate(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyTwice"))
		{
			AddCopyTwice(R);
			return true;
		}
		if (R.Scenario == TEXT("CopySaveLoad"))
		{
			R.StepTimeoutSec = 60.0;
			AddCopySaveLoad(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyCost"))
		{
			AddCopyCost(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyCostMissing"))
		{
			AddCopyCostMissing(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyNoCost"))
		{
			AddCopyNoCost(R);
			return true;
		}
		if (R.Scenario == TEXT("CopyCostDepot"))
		{
			AddCopyCostDepot(R);
			return true;
		}
		if (R.Scenario == TEXT("FillGrid"))
		{
			AddFillGrid(R, false);
			return true;
		}
		if (R.Scenario == TEXT("FillNegative"))
		{
			AddFillGrid(R, true);
			return true;
		}
		if (R.Scenario == TEXT("Preview"))
		{
			R.StepTimeoutSec = 60.0;
			AddPreview(R);
			return true;
		}
		if (R.Scenario == TEXT("SelectLightweight"))
		{
			AddSelectLightweight(R);
			return true;
		}
		if (R.Scenario == TEXT("FillLarge"))
		{
			AddFillLarge(R);
			return true;
		}
		if (R.Scenario == TEXT("DoubleClick"))
		{
			AddDoubleClick(R, true, 0, -27000.0);
			AddDoubleClick(R, true, 3, -9000.0);
			AddDoubleClick(R, false, 0, 9000.0);
			AddDoubleClick(R, false, 3, 27000.0);
			return true;
		}
		if (R.Scenario == TEXT("SpawnProbe"))
		{
			R.StepTimeoutSec = 60.0;
			AddSpawnProbe(R);
			return true;
		}
		if (R.Scenario == TEXT("BindSignature"))
		{
			AddBindSignature(R);
			return true;
		}
		if (R.Scenario == TEXT("ToolMenuClose"))
		{
			AddToolMenuClose(R);
			return true;
		}
		if (R.Scenario == TEXT("SetRecipe"))
		{
			AddSetRecipe(R);
			return true;
		}
		if (R.Scenario == TEXT("SetRecipeNoMatch"))
		{
			AddSetRecipeUnchanged(R, Asset::IronPlate, Asset::IngotIron, TEXT("No machine in the area accepts"));
			return true;
		}
		if (R.Scenario == TEXT("SetRecipeAlready"))
		{
			AddSetRecipeUnchanged(R, Asset::IronRod, Asset::IronRod, TEXT("All machines in the area already use"));
			return true;
		}
		if (R.Scenario == TEXT("ClearInventories"))
		{
			AddClearInventories(R, false);
			return true;
		}
		if (R.Scenario == TEXT("ClearInventoriesCrate"))
		{
			AddClearInventories(R, true);
			return true;
		}
		if (R.Scenario == TEXT("SetStationCancel"))
		{
			AddSetStationCancel(R);
			return true;
		}
		if (R.Scenario == TEXT("SetStation"))
		{
			AddSetStation(R);
			return true;
		}
		if (R.Scenario == TEXT("SetStationEmpty"))
		{
			AddSetStationEmpty(R);
			return true;
		}
		if (R.Scenario == TEXT("RejectBadRequests"))
		{
			AddRejectBadRequests(R);
			return true;
		}
		if (R.Scenario == TEXT("SetRecipeLarge"))
		{
			R.StepTimeoutSec = 120.0;
			AddSetRecipeLarge(R);
			return true;
		}
		if (R.Scenario == TEXT("ToolMenu"))
		{
			AddToolMenu(R);
			return true;
		}
		if (R.Scenario == TEXT("Corners"))
		{
			AddCorners(R);
			return true;
		}
		if (R.Scenario == TEXT("CornersOrder"))
		{
			AddCornersOrder(R);
			return true;
		}
		if (R.Scenario == TEXT("SelectBuilding"))
		{
			AddSelectBuilding(R);
			return true;
		}
		if (R.Scenario == TEXT("Smoke"))
		{
			AddSmoke(R, false);
			return true;
		}
		if (R.Scenario == TEXT("SmokeFail"))
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
		// Exit from the next core tick, never from inside a world tick or timer. Two games: 2 s later, so the last message to the other
		// game leaves first
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			FPlatformMisc::RequestExit(false, TEXT("AreaActions test mode"));
			return false;
		}), R.bMp ? 2.0f : 0.0f);
	}

	// Host: a message of the client's game; its first one says which player is the client
	void OnHostPing(UAreaActionsRCO* From, int32 Token)
	{
		if (!Run || !Run->bMp || Run->bClient || Run->bEnded || !From || !(Token & FromClient))
		{
			return;
		}
		FRun& R = *Run;
		if (((Token >> 24) & 31) == MsgHello)
		{
			if (!R.bLinked)
			{
				R.Peer = From->GetOwnerPlayerController();
				R.Step(FString::Printf(TEXT("the client's game joined: %s"), *GetNameSafe(R.Peer.Get())));
			}
			Receive(R, Token);
			Send(R, MsgHello, 0);
		}
		else if (UAreaActionsRCO::Of(R.Peer.Get()) == From)
		{
			Receive(R, Token);
		}
	}

	// Two games before the first step: the client says hello every second until the host answers. False when it took too long.
	bool Link(FRun& R)
	{
		UAreaActionsRCO* Rco = R.bClient && R.World.IsValid() ? UAreaActionsRCO::Of(R.GetLocalPlayer()) : nullptr;
		if (Rco && GetCharacter(R))
		{
			if (!R.LinkHandle.IsValid())
			{
				R.LinkHandle = Rco->OnPong.AddLambda([](int32 Token)
				{
					if (Run && !Run->bEnded && (Token & FromHost))
					{
						Receive(*Run, Token);
					}
				});
			}
			if (R.Now() - R.LastHello > 1.0)
			{
				R.LastHello = R.Now();
				Send(R, MsgHello, 0);
			}
		}
		// A dedicated server's world is up long before the client's game has started and joined
		return R.Now() - R.Created < (R.bDedicated ? 420.0 : 240.0);
	}

	// AA-040 point 2: what this game holds of the mod's windows, the tool's indicators and action actors right now
	void Watch(FRun& R)
	{
		static const TCHAR* Indicators[] = { TEXT("/AreaActions/Equipment/AreaActions/Indicators/AreaActionsCorner.AreaActionsCorner_C"),
			TEXT("/AreaActions/Equipment/AreaActions/Indicators/AreaActionsWall.AreaActionsWall_C"), TEXT("/AreaActions/Equipment/AreaActions/Indicators/AreaActionsZ.AreaActionsZ_C") };
		static TMap<const UClass*, bool> Ours;
		const UWorld* World = R.GetWorld();
		const auto CountOf = [World](const UClass* Class)
		{
			TArray<UObject*> Found;
			GetObjectsOfClass(Class, Found, true, RF_ClassDefaultObject | RF_ArchetypeObject);
			int32 Count = 0;
			for (const UObject* Each : Found)
			{
				Count += IsValid(Each) && Each->GetWorld() == World ? 1 : 0;
			}
			return Count;
		};
		TArray<UObject*> Widgets;
		GetObjectsOfClass(UUserWidget::StaticClass(), Widgets, true, RF_ClassDefaultObject | RF_ArchetypeObject);
		int32 Windows = 0;
		for (const UObject* Widget : Widgets)
		{
			bool* bOurs = Ours.Find(Widget->GetClass());
			if (!bOurs)
			{
				bOurs = &Ours.Add(Widget->GetClass(), Widget->GetClass()->GetPathName().StartsWith(TEXT("/AreaActions/")));
			}
			// A widget made for a player has that player's world; the templates inside a widget class have none
			Windows += *bOurs && IsValid(Widget) && Widget->GetWorld() == World ? 1 : 0;
		}
		int32 Markers = 0;
		for (const TCHAR* Path : Indicators)
		{
			const UClass* Class = FindObject<UClass>(nullptr, Path);
			Markers += Class ? CountOf(Class) : 0;
		}
		const int32 Actions = CountOf(AAreaActionBase::StaticClass());
		// AA-089: a preview's holograms and its own actor (the instanced meshes) are the previewing player's alone
		const int32 Ghosts = CountOf(AFGHologram::StaticClass()) + CountOf(AAreaActionsPreview::StaticClass());
		if (Windows > R.MaxWindows || Markers > R.MaxIndicators || Actions > R.MaxActions || Ghosts > R.MaxGhosts)
		{
			R.Step(FString::Printf(TEXT("here now: mod windows %d, tool indicators %d, action actors %d, holograms and preview actors %d"), Windows, Markers, Actions, Ghosts));
		}
		if (Actions > R.MaxActions)
		{
			for (TActorIterator<AAreaActionBase> It(R.GetWorld()); It; ++It)
			{
				R.Step(FString::Printf(TEXT("action actor %s: role %d, remote role %d, replicates %d, owner %s"), *It->GetName(), static_cast<int32>(It->GetLocalRole()),
					static_cast<int32>(It->GetRemoteRole()), It->GetIsReplicated(), *GetNameSafe(It->GetOwner())));
			}
		}
		R.MaxGhosts = FMath::Max(R.MaxGhosts, Ghosts);
		R.MaxWindows = FMath::Max(R.MaxWindows, Windows);
		R.MaxIndicators = FMath::Max(R.MaxIndicators, Markers);
		R.MaxActions = FMath::Max(R.MaxActions, Actions);
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
			if (FApp::GetDeltaTime() > 0.1)
			{
				UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step slow frame %.1f ms at step %d"), *R.Name, FApp::GetDeltaTime() * 1000.0, R.Index + 1);
			}
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
		if (R.bMp && !R.bLinked)
		{
			if (!Link(R))
			{
				R.Check(false, TEXT("the other game did not answer in 240 s"));
				Finish(R);
				return false;
			}
			// The load settles after the two games have met
			R.Started = R.Now();
			R.StartFrame = GFrameCounter;
			return true;
		}
		if (!R.World.IsValid())
		{
			R.Check(false, TEXT("the game world is gone"));
			Finish(R);
			return false;
		}
		if (R.bMp)
		{
			Watch(R);
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

// W5 diagnosis: while a scenario measures frames, a frame over 100 ms is logged split into its phases, with the garbage collection in it
static void WatchFramePhases()
{
	static bool bWatching = false;
	if (bWatching)
	{
		return;
	}
	bWatching = true;
	struct FPhases { double Begin = 0.0, WorldStart = 0.0, PreActors = 0.0, PostActors = 0.0, GcStart = 0.0, Gc = 0.0; double Groups[TG_MAX] = {}; };
	static FPhases Phases;
	// One probe at the start of each tick group (leaked on purpose: a level that goes away unregisters them, nothing destroys them later)
	struct FProbe : FTickFunction
	{
		virtual void ExecuteTick(float, ELevelTick, ENamedThreads::Type, const FGraphEventRef&) override
		{
			if (Phases.Groups[TickGroup] == 0.0)
			{
				Phases.Groups[TickGroup] = FPlatformTime::Seconds();
			}
		}
		virtual FString DiagnosticMessage() override { return TEXT("AreaActions frame phase probe"); }
	};
	if (UWorld* World = Run ? Run->GetWorld() : nullptr)
	{
		for (const ETickingGroup Group : { TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics, TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable })
		{
			FProbe* Probe = new FProbe();
			Probe->bCanEverTick = true;
			Probe->bTickEvenWhenPaused = true;
			Probe->bHighPriority = true;
			Probe->TickGroup = Group;
			Probe->EndTickGroup = Group;
			Probe->RegisterTickFunction(World->PersistentLevel);
		}
	}
	FCoreDelegates::OnBeginFrame.AddLambda([] { Phases = FPhases(); Phases.Begin = FPlatformTime::Seconds(); });
	FWorldDelegates::OnWorldTickStart.AddLambda([](UWorld*, ELevelTick, float) { if (Phases.WorldStart == 0.0) { Phases.WorldStart = FPlatformTime::Seconds(); } });
	FWorldDelegates::OnWorldPreActorTick.AddLambda([](UWorld*, ELevelTick, float) { if (Phases.PreActors == 0.0) { Phases.PreActors = FPlatformTime::Seconds(); } });
	FWorldDelegates::OnWorldPostActorTick.AddLambda([](UWorld*, ELevelTick, float) { Phases.PostActors = FPlatformTime::Seconds(); });
	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda([] { Phases.GcStart = FPlatformTime::Seconds(); });
	FCoreUObjectDelegates::GetPostGarbageCollect().AddLambda([] { if (Phases.GcStart > 0.0) { Phases.Gc += FPlatformTime::Seconds() - Phases.GcStart; } });
	FCoreDelegates::OnEndFrame.AddLambda([]
	{
		const double End = FPlatformTime::Seconds();
		if (Run && Run->bMeasureFrames && Phases.Begin > 0.0 && End - Phases.Begin > 0.1)
		{
			const auto Ms = [](double From, double To) { return From > 0.0 && To >= From ? (To - From) * 1000.0 : -1.0; };
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step frame phases: total %.1f ms, to world tick %.1f, to actor ticks %.1f, actor ticks %.1f, after actor ticks %.1f, GC %.1f"),
				*Run->Name, (End - Phases.Begin) * 1000.0, Ms(Phases.Begin, Phases.WorldStart), Ms(Phases.WorldStart, Phases.PreActors), Ms(Phases.PreActors, Phases.PostActors),
				Ms(Phases.PostActors, End), Phases.Gc * 1000.0);
			FString Groups;
			double Last = Phases.PreActors;
			for (const ETickingGroup Group : { TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics, TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable })
			{
				Groups += FString::Printf(TEXT(" %d:%.1f"), static_cast<int32>(Group), Ms(Last, Phases.Groups[Group]));
				Last = Phases.Groups[Group] > 0.0 ? Phases.Groups[Group] : Last;
			}
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step frame phases: up to each tick group's start%s, from the last group's start to the actor ticks' end %.1f"), *Run->Name, *Groups,
				Ms(Last, Phases.PostActors));
		}
	});
}

// -AATestLoad: on until the test's own save load has been issued (JoinFromCommandLine)
static bool bAwaitTestSave = false;

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
	FString Role;
	const bool bClient = FParse::Value(FCommandLine::Get(), TEXT("AATestRole="), Role) && Role == TEXT("client");
	if (Run && Run->bClient && !Run->bLinked && World && World->GetNetMode() == NM_Client)
	{
		// The client's game passes through its main menu on the way to the host: the scenario follows it into the newest world
		Run->World = World;
		Run->Step(FString::Printf(TEXT("now in %s, net mode %d"), *World->GetMapName(), static_cast<int32>(World->GetNetMode())));
		return;
	}
	if (Run || !World || !FParse::Value(FCommandLine::Get(), TEXT("AATest="), Name) || (bClient && World->GetNetMode() != NM_Client))
	{
		return;
	}
	// A dedicated server passes through its lobby map (or the session its settings name) before the test's save is loaded: the
	// scenario starts in the game map that load makes
	const bool bDedicated = World->GetNetMode() == NM_DedicatedServer;
	if (bDedicated && (bAwaitTestSave || !World->GetMapName().Contains(TEXT("Persistent_Level"))))
	{
		UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step dedicated server in %s, waiting for the game map"), *Name, *World->GetMapName());
		return;
	}
	// Every scenario starts as a player who never used the tool (D21: the settings file of the test mode)
	AreaActionsSettings::ResetForTest();
	Run = MakeUnique<FRun>();
	Run->Name = Name;
	Run->World = World;
	Run->Started = Run->Created = FPlatformTime::Seconds();
	Run->StartFrame = GFrameCounter;
	// MP_<name>: two game instances; in MP_Host<name> the host's own player acts and the client watches
	Run->bMp = Name.StartsWith(TEXT("MP_"));
	Run->bClient = bClient;
	Run->bDedicated = bDedicated;
		Run->bPeerActs = !Name.StartsWith(TEXT("MP_Host"));
	Run->Scenario = Name;
	if (Run->bMp && !Run->Scenario.RemoveFromStart(TEXT("MP_Host")))
	{
		Run->Scenario.RemoveFromStart(TEXT("MP_"));
	}
	UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST %s step scenario in %s, net mode %d%s"), *Name, *World->GetMapName(), static_cast<int32>(World->GetNetMode()),
		Run->bMp ? (bClient ? TEXT(", the client of two games") : bDedicated ? TEXT(", the dedicated server of two games") : TEXT(", the host of two games")) : TEXT(""));
	if (Run->bMp)
	{
		Run->StepTimeoutSec = 120.0;
		static FDelegateHandle PingHandle;
		if (!bClient && !PingHandle.IsValid())
		{
			PingHandle = UAreaActionsRCO::OnPing.AddStatic(&OnHostPing);
		}
	}
	// The dedicated server's own scenarios (ServerRestart) have no player to place; MP_Host<name> needs the host's own player
	if (bDedicated && (!Run->bMp || !Run->bPeerActs))
	{
		if (Run->bMp)
		{
			Run->Check(false, TEXT("MP_Host scenarios need a host with a player, not a dedicated server"));
			Finish(*Run);
			return;
		}
	}
	else
	{
		AddPlace(*Run);
	}
	if (!BuildScenario(*Run))
	{
		Run->Check(false, TEXT("unknown scenario"));
		Finish(*Run);
		return;
	}
	if (Run->bMp)
	{
		// AA-040 point 2: the windows and indicators exist only in the game of the player who acts
		Mark(*Run, ESide::Each);
			Run->Steps.Add([](FRun& Self)
		{
			const FString Seen = FString::Printf(TEXT("mod windows %d, tool indicators %d, action actors %d, holograms and preview actors %d"), Self.MaxWindows, Self.MaxIndicators,
				Self.MaxActions, Self.MaxGhosts);
			// A dedicated server has no player: after the client left nobody acted there either
			if (Self.Acts() && !Self.bDedicated)
			{
				// Flying and the tutorial use the menu alone (no area, so no indicators); the schematic opens nothing of the mod
				const bool bShowsNothing = Self.Scenario == TEXT("Schematic");
				Self.Check(bShowsNothing || Self.MaxIndicators > 0 || Self.MaxWindows > 0, FString::Printf(TEXT("this game's player acted: here at most %s"), *Seen));
			}
			else
			{
				Self.Check(Self.MaxWindows == 0 && Self.MaxIndicators == 0 && Self.MaxActions == 0 && Self.MaxGhosts == 0,
					FString::Printf(TEXT("the other game's player acted: here at most %s (want 0)"), *Seen));
			}
			// AA-040 point 3: the players stand apart, so "next to the player" names one of them
			const APawn* Own = Self.GetLocalPlayer() ? Self.GetLocalPlayer()->GetPawn() : nullptr;
			const APawn* Theirs = Self.Peer.IsValid() ? Self.Peer->GetPawn() : nullptr;
			if (!Self.bClient && Own && Theirs)
			{
				const double Apart = FVector::Dist(Own->GetActorLocation(), Theirs->GetActorLocation());
				Self.Check(Apart >= 3000.0, FString::Printf(TEXT("the two players stand %.0f cm apart (at least 3000): host %s, client %s"), Apart,
					*Own->GetActorLocation().ToCompactString(), *Theirs->GetActorLocation().ToCompactString()));
			}
			return EStep::Next;
		});
		ApplySides(*Run);
	}
	WatchFramePhases();
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
}

#if !UE_SERVER
namespace
{
	// The game asks a server in its list for its state only while something listens to that server, as the Server Manager's window does.
	// A plain C++ listener (the server target has no client module to derive a UCLASS from); its object is the game instance, and it
	// ignores what it hears.
	class FJoinListener : public IFGServerStateListener
	{
	public:
		TWeakObjectPtr<UObject> Owner;
		virtual UObject* _getUObject() const override { return Owner.Get(); }
		virtual void NativeOnServerStateChanged(UFGServerObject*, EServerState) override {}
		virtual void NativeOnServerGameStateChanged(UFGServerObject*, const FServerGameState&) override {}
		virtual void NativeOnServerAPIStateChanged(UFGServerObject*, EFGServerAPIState) override {}
		virtual void NativeOnServerAuthenticationChanged(UFGServerObject*, const FServerAuthenticationToken&) override {}
		virtual void NativeOnServerRenamed(UFGServerObject*, const FString&) override {}
		virtual void NativeOnServerComplexStateChanged(UFGServerObject*, EServerComplexState) override {}
		virtual void NativeOnNewServerConsoleOutput(UFGServerObject*, const FString&) override {}
	};
	FJoinListener JoinListener;
}
#endif

void FAreaActionsTests::JoinFromCommandLine()
{
	// -AATestLoad=<save>: a dedicated server starts in its lobby map, or in the session its settings name; the test loads its own save
	// once (a hard travel, as the server's API does), and the scenario starts only in that world
	FString LoadSave;
	if (IsRunningDedicatedServer() && FParse::Value(FCommandLine::Get(), TEXT("AATestLoad="), LoadSave))
	{
		bAwaitTestSave = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([LoadSave, From = TWeakObjectPtr<UWorld>(), Keep = FString(), bIssued = false](float) mutable
		{
			if (!GEngine)
			{
				return true;
			}
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* World = Context.World();
				if (Context.WorldType != EWorldType::Game || !World || !World->HasBegunPlay())
				{
					continue;
				}
				if (!bIssued)
				{
					bIssued = true;
					From = World;
#if UE_SERVER
					// The load makes the test's session the one the server loads at its next start (its settings file); the session it
					// had is written back once the test's world is up. -AATestAutoLoadSession=<name> writes that name instead.
					if (const UFGServerSubsystem* Server = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UFGServerSubsystem>() : nullptr)
					{
						Keep = Server->GetAutoLoadSessionName();
					}
					FParse::Value(FCommandLine::Get(), TEXT("AATestAutoLoadSession="), Keep);
#endif
					UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST load: %s from %s"), *LoadSave, *World->GetMapName());
					bAwaitTestSave = false;
					UGameplayStatics::OpenLevel(World, FName(TEXT("/Game/FactoryGame/Map/GameLevel01/Persistent_Level")), true, FString::Printf(TEXT("?loadgame=%s"), *LoadSave));
					return true;
				}
				if (World == From.Get() || !World->GetMapName().Contains(TEXT("Persistent_Level")))
				{
					return true;
				}
#if UE_SERVER
				const UFGServerSubsystem* Settings = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UFGServerSubsystem>() : nullptr;
				if (Settings && !Keep.IsEmpty())
				{
					Settings->SetAutoLoadSessionName(Keep);
					UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST load: the server's auto-load session is %s again"), *Settings->GetAutoLoadSessionName());
				}
#endif
				return false;
			}
			return true;
		}), 2.f);
	}
#if !UE_SERVER
	FString Address;
	if (!FParse::Value(FCommandLine::Get(), TEXT("AATestJoin="), Address))
	{
		return;
	}
	FString Host = Address;
	FString PortText;
	int32 Port = 7777;
	if (Address.Split(TEXT(":"), &Host, &PortText))
	{
		LexFromString(Port, *PortText);
	}
	// What a player does in the Server Manager: the server is activated (its state is polled), the client logs in without a password,
	// then joins. One step every 5 s from the main menu, so a join under way is left alone; done once this game is in the server's world.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Host, Port, LastSec = 0.0, Activated = TWeakObjectPtr<UFGServerObject>()](float) mutable
	{
		if (!GEngine)
		{
			return true;
		}
		UWorld* Menu = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (Context.WorldType != EWorldType::Game || !World || !World->HasBegunPlay())
			{
				continue;
			}
			// While joining, the menu world reports NM_Client too; its context holds the pending game
			if (World->GetNetMode() == NM_Client && !Context.PendingNetGame)
			{
				UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST join %s:%d: in the server's world %s"), *Host, Port, *World->GetMapName());
				return false;
			}
			Menu = World->GetNetMode() == NM_Standalone && !Context.PendingNetGame ? World : Menu;
		}
		const double Now = FPlatformTime::Seconds();
		if (!Menu || Now - LastSec < 5.0)
		{
			return true;
		}
		LastSec = Now;
		UFGServerManager* Manager = Menu->GetGameInstance() ? Menu->GetGameInstance()->GetSubsystem<UFGServerManager>() : nullptr;
		UFGServerObject* Server = Manager ? Manager->GetServerByHostname(Host, Port) : nullptr;
		if (!Server)
		{
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST join %s:%d is not in the game's Server Manager: add it there once and accept its certificate"), *Host, Port);
			return true;
		}
		const FString StateName = UEnum::GetValueAsString(Server->GetComplexState());
		if (Activated.Get() != Server)
		{
			Activated = Server;
			JoinListener.Owner = Menu->GetGameInstance();
			TScriptInterface<IFGServerStateListener> Listener;
			Listener.SetObject(Menu->GetGameInstance());
			Listener.SetInterface(&JoinListener);
			Server->RegisterStateListener(Listener);
			Server->OnServerActivated();
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST join %s:%d: server activated, state %s"), *Host, Port, *StateName);
			return true;
		}
		const EServerComplexState State = Server->GetComplexState();
		const TCHAR* Did = TEXT("waiting");
		if (State == EServerComplexState::GameRunning_Connected_Client || State == EServerComplexState::GameRunning_Connected_Admin)
		{
			Did = Server->Join() ? TEXT("joining") : TEXT("join refused");
		}
		else if (State == EServerComplexState::GameRunning_ConnectionFailed || State == EServerComplexState::NoGameRunning_ConnectionFailed)
		{
			Server->AttemptServerAPIConnection();
			Did = TEXT("connecting to the server API again");
		}
		else if (State == EServerComplexState::GameRunning_Connected_NonAuthenticated || State == EServerComplexState::GameRunning_NotConnected_CompatibleVersion)
		{
			Server->Authenticate(EPrivilegeLevel::Client, true);
			Did = TEXT("logging in as a client");
		}
		UE_LOG(LogAreaActionsCpp, Display, TEXT("AATEST join %s:%d: %s, server state %s"), *Host, Port, Did, *StateName);
		return true;
	}), 1.f);
#endif
}

#endif

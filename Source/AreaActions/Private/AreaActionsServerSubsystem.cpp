#include "AreaActionsServerSubsystem.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "FGPlayerController.h"
#include "Engine/NetConnection.h"
#include "EngineUtils.h"
#include "FGBuildableSubsystem.h"
#include "FGCrate.h"
#include "HAL/IConsoleManager.h"
#include "Subsystem/SubsystemActorManager.h"

static TAutoConsoleVariable<float> CVarServerBudgetMs(TEXT("AreaActions.ServerBudgetMs"), 2.0f,
	TEXT("Time per frame the server spends on area actions, for all players together (ms); 0 = one building per frame"));

int32 AAreaActionsServerSubsystem::FinishedJobs = 0;
int32 AAreaActionsServerSubsystem::ResultsWithoutPlayer = 0;

void FAreaActionsJob::Finish(UWorld* World)
{
	if (Result.Code == EAreaActionsResultCode::Ok && Result.Buildings == 0)
	{
		Result.Code = EAreaActionsResultCode::NothingInArea;
	}
}

bool FAreaActionsJob::HasPlayer() const
{
	const UAreaActionsRCO* Owner = Rco.Get();
	const APlayerController* Player = Owner ? Owner->GetOwnerPlayerController() : nullptr;
	// A remote player whose connection is gone or closing has left; the host's own player has none and is always here
	const UNetConnection* Connection = IsValid(Player) ? Player->GetNetConnection() : nullptr;
	return IsValid(Player) && (Player->IsLocalController() || (Connection && Connection->GetConnectionState() == USOCK_Open));
}

AFGCharacterPlayer* FAreaActionsJob::GetPresentCharacter() const
{
	return HasPlayer() ? Character.Get() : nullptr;
}

AAreaActionsServerSubsystem::AAreaActionsServerSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

AAreaActionsServerSubsystem* AAreaActionsServerSubsystem::Get(UWorld* World)
{
	USubsystemActorManager* Manager = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
	return Manager ? Manager->GetSubsystemActor<AAreaActionsServerSubsystem>() : nullptr;
}

void AAreaActionsServerSubsystem::Init()
{
	Super::Init();
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Server subsystem Init"));
}

void AAreaActionsServerSubsystem::EndPlay(const EEndPlayReason::Type Reason)
{
	// Nothing carries over into the next load. A job cut off here loses nothing: every job keeps the world whole after each building
	// (items go into crates as they leave, AA-078; a copy is paid in the frame it is built, D22), so any save holds a consistent world
	Jobs.Reset();
	Super::EndPlay(Reason);
}

bool AAreaActionsServerSubsystem::HasJobFor(const UAreaActionsRCO* Rco) const
{
	return Jobs.ContainsByPredicate([Rco](const TUniquePtr<FAreaActionsJob>& Job) { return Job->Rco.Get() == Rco; });
}

void AAreaActionsServerSubsystem::Enqueue(TUniquePtr<FAreaActionsJob> Job)
{
	Jobs.Add(MoveTemp(Job));
}

void AAreaActionsServerSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Jobs.IsEmpty())
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	const double Share = FMath::Max(0.0f, CVarServerBudgetMs.GetValueOnGameThread()) / 1000.0 / Jobs.Num();
	for (int32 Index = 0; Index < Jobs.Num();)
	{
		// A job runs to its end even when its player has left (AA-095): stopping half way would leave half a copy, or items taken
		// out of machines with no crate yet
		FAreaActionsJob& Job = *Jobs[Index];
		const double SliceStart = FPlatformTime::Seconds();
		const bool bDone = Job.Step(GetWorld(), FMath::Max(SliceStart, Start) + Share);
		++Job.Frames;
		Job.MaxSliceMs = FMath::Max(Job.MaxSliceMs, (FPlatformTime::Seconds() - SliceStart) * 1000.0);
		if (!bDone)
		{
			++Index;
			continue;
		}
		Job.Finish(GetWorld());
		UE_LOG(LogAreaActionsCpp, Display, TEXT("AAJOB %s items=%d frames=%d maxslice=%.2f code=%d buildings=%d"),
			Job.GetName(), Job.GetItems(), Job.Frames, Job.MaxSliceMs, static_cast<int32>(Job.Result.Code), Job.Result.Buildings);
		++FinishedJobs;
		if (Job.HasPlayer())
		{
			Job.Rco->ClientResult(Job.RequestId, Job.Result);
		}
		else
		{
			// The player left while the job ran: its RCO outlives the connection until garbage collection, so the result has nobody to go to
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AAJOB %s request %d: the player left, no result sent"), Job.GetName(), Job.RequestId);
			++ResultsWithoutPlayer;
		}
		Jobs.RemoveAt(Index);
	}
}

bool FAreaActionsJob::Step(UWorld* World, double EndTime)
{
	FAreaActionsJob& Job = *this;
	if (!Job.bCollected)
	{
		if (Job.Cursor == 0 && Job.WantsCrates())
		{
			for (TActorIterator<AFGCrate> It(World); It; ++It)
			{
				if (IsValid(*It) && Job.Area.Contains(It->GetActorLocation()))
				{
					Job.Found.Add(*It);
				}
			}
		}
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
		// shortcut: a cursor over the live list; a building removed during the collection can shift one unvisited entry past it.
		// Upgrade to a snapshot when actions have to see every building while others dismantle.
		static const TArray<AFGBuildable*> None;
		const TArray<AFGBuildable*>& All = Buildables ? Buildables->GetAllBuildablesRef() : None;
		while (Job.Cursor < All.Num())
		{
			AFGBuildable* Buildable = All[Job.Cursor++];
			if (IsValid(Buildable) && Job.Matches(Buildable) && Job.Area.Contains(Buildable->GetActorLocation()))
			{
				Job.Found.Add(Buildable);
			}
			if ((Job.Cursor & 255) == 0 && FPlatformTime::Seconds() > EndTime)
			{
				return false;
			}
		}
		Job.bCollected = true;
	}
	while (Job.ApplyIndex < Job.Found.Num())
	{
		if (AActor* Actor = Job.Found[Job.ApplyIndex++].Get(); IsValid(Actor))
		{
			Job.Apply(Actor);
		}
		if (FPlatformTime::Seconds() > EndTime)
		{
			return Job.ApplyIndex >= Job.Found.Num();
		}
	}
	return true;
}

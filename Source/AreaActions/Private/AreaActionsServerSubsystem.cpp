#include "AreaActionsServerSubsystem.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "EngineUtils.h"
#include "FGBuildableSubsystem.h"
#include "FGCrate.h"
#include "HAL/IConsoleManager.h"
#include "Subsystem/SubsystemActorManager.h"

static TAutoConsoleVariable<float> CVarServerBudgetMs(TEXT("AreaActions.ServerBudgetMs"), 2.0f,
	TEXT("Time per frame the server spends on area actions, for all players together (ms)"));

int32 AAreaActionsServerSubsystem::FinishedJobs = 0;

void FAreaActionsJob::Finish(UWorld* World)
{
	if (Result.Code == EAreaActionsResultCode::Ok && Result.Buildings == 0)
	{
		Result.Code = EAreaActionsResultCode::NothingInArea;
	}
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
	// Nothing carries over into the next load. shortcut: a Copy or Fill cut off here keeps the cost of its unbuilt copies; the world
	// goes with it unless a save is made in the same moment. Finish the jobs here when a server's own shutdown save shows the gap.
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
	const double Share = FMath::Max(0.1, CVarServerBudgetMs.GetValueOnGameThread()) / 1000.0 / Jobs.Num();
	for (int32 Index = 0; Index < Jobs.Num();)
	{
		FAreaActionsJob& Job = *Jobs[Index];
		UAreaActionsRCO* Rco = Job.Rco.Get();
		if (!Rco)
		{
			// The player left: the job ends between two buildings; Finish still hands over what was already taken (SetRecipe's crate)
			Job.Finish(GetWorld());
			UE_LOG(LogAreaActionsCpp, Display, TEXT("AAJOB %s cancelled, the player left, buildings=%d"), Job.GetName(), Job.Result.Buildings);
			Jobs.RemoveAt(Index);
			continue;
		}
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
		Rco->ClientResult(Job.RequestId, Job.Result);
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

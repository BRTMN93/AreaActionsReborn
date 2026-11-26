#include "AreaActionsWorldModule.h"
#include "AreaActionBase.h"
#include "AreaActionsLog.h"
#include "Module/WorldModuleManager.h"
#include "AreaActionsServerSubsystem.h"
#include "AreaActionsTests.h"

uint8 UAreaActionsWorldModule::PhasesSeen = 0;

UAreaActionsWorldModule::UAreaActionsWorldModule()
{
	// Spawned by SML in CONSTRUCTION
	ModSubsystems.Add(AAreaActionsServerSubsystem::StaticClass());
}

UAreaActionsWorldModule* UAreaActionsWorldModule::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UWorldModuleManager* Manager = World ? World->GetSubsystem<UWorldModuleManager>() : nullptr;
	return Manager ? Cast<UAreaActionsWorldModule>(Manager->FindModule(TEXT("AreaActionsReborn"))) : nullptr;
}

void UAreaActionsWorldModule::AddAction(TSubclassOf<AAreaActionBase> Action)
{
	if (Action)
	{
		AvailableActions.AddUnique(Action);
	}
}

void UAreaActionsWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// SML registers ModSubsystems and mSchematics in here, and runs the Blueprint child's graph
	Super::DispatchLifecycleEvent(Phase);
	PhasesSeen |= 1 << static_cast<uint8>(Phase);
	const UWorld* World = GetWorld();
	UE_LOG(LogAreaActionsCpp, Display, TEXT("World module %s, net mode %d"), *LifecyclePhaseToString(Phase), World ? static_cast<int32>(World->GetNetMode()) : -1);

#if AA_WITH_TESTS
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		FAreaActionsTests::StartFromCommandLine(GetWorld());
	}
#endif
}

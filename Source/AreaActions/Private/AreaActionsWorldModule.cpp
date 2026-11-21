#include "AreaActionsWorldModule.h"
#include "AreaActionsLog.h"
#include "AreaActionsServerSubsystem.h"
#include "AreaActionsTests.h"

uint8 UAreaActionsWorldModule::PhasesSeen = 0;

UAreaActionsWorldModule::UAreaActionsWorldModule()
{
	// Spawned by SML in CONSTRUCTION
	ModSubsystems.Add(AAreaActionsServerSubsystem::StaticClass());
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

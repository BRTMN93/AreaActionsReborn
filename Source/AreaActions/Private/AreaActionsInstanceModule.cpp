#include "AreaActionsInstanceModule.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"

uint8 UAreaActionsInstanceModule::PhasesSeen = 0;

UAreaActionsInstanceModule::UAreaActionsInstanceModule()
{
	bRootModule = true;
	RemoteCallObjects.Add(UAreaActionsRCO::StaticClass());
}

void UAreaActionsInstanceModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// SML registers the module's arrays (the RCO) in here
	Super::DispatchLifecycleEvent(Phase);
	PhasesSeen |= 1 << static_cast<uint8>(Phase);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Instance module %s"), *LifecyclePhaseToString(Phase));
}

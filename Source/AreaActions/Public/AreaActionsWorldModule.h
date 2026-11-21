#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "AreaActionsWorldModule.generated.h"

// Parent of the Blueprint root AreaActions_GameWorldModule, which keeps bRootModule (never set here: two roots are a Fatal error)
UCLASS()
class AREAACTIONS_API UAreaActionsWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	UAreaActionsWorldModule();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

	// Phases seen in this process, one bit per ELifecyclePhase value (the test mode checks them)
	static uint8 PhasesSeen;
};

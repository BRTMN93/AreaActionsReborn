#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "AreaActionsInstanceModule.generated.h"

// The mod's game instance root. Pure C++: bRootModule is set here and there is no Blueprint child (a flagged child as well would be a
// second root, which SML stops with a Fatal error).
UCLASS()
class AREAACTIONSREBORN_API UAreaActionsInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	UAreaActionsInstanceModule();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

	// Phases seen in this process, one bit per ELifecyclePhase value (the test mode checks them)
	static uint8 PhasesSeen;
};

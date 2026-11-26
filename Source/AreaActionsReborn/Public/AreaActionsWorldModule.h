#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "AreaActionsWorldModule.generated.h"

class AAreaActionBase;

// Parent of the Blueprint root AreaActions_GameWorldModule, which keeps bRootModule (never set here: two roots are a Fatal error)
UCLASS()
class AREAACTIONSREBORN_API UAreaActionsWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	UAreaActionsWorldModule();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

	// This world's module, on the server and on clients
	static UAreaActionsWorldModule* Get(const UObject* WorldContext);

	// The tool's menu, in this order; set in the Blueprint child
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Area Actions")
	TArray<TSubclassOf<AAreaActionBase>> AvailableActions;

	// For other mods: one more action in the menu (the original's ActionsSubsystem.AddAction)
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void AddAction(TSubclassOf<AAreaActionBase> Action);

	// Phases seen in this process, one bit per ELifecyclePhase value (the test mode checks them)
	static uint8 PhasesSeen;
};

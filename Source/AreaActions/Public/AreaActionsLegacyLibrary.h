#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AreaActionsLegacyLibrary.generated.h"

// Parent of the Blueprint library SpawnActorWithTemplate until Copy and Fill move to C++ (AA-020). The pins keep the Blueprint's names,
// including its hidden world context pin, so the existing call nodes bind to this version.
UCLASS()
class AREAACTIONS_API UAreaActionsLegacyLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Classes the copy skips: belts, wires, tracks, switches, poles, pipes, hypertubes and their parts, extractors
	UFUNCTION(BlueprintPure, Category = "Area Actions", meta = (WorldContext = "__WorldContext"))
	static void ShouldIgnore(AActor* Actor, UObject* __WorldContext, bool& Ignore);
};

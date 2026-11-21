#pragma once

#include "CoreMinimal.h"
#include "Equipment/FGEquipment.h"
#include "AreaActionsEquipment.generated.h"

class AFGBuildable;

// Parent of the tool Blueprint Equip_AreaActions. Functions here keep the name and the pins of the Blueprint functions they replace, so
// the Blueprint's call nodes bind to them once the Blueprint's own version is removed.
UCLASS(Abstract)
class AREAACTIONS_API AAreaActionsEquipment : public AFGEquipment
{
	GENERATED_BODY()

public:
	// The 8 corners of the building's box in the world: its clearance box, or without one, the bounds of its class default and of its
	// lightweight instance meshes. Empty for classes the copy skips that have no clearance (as the original did).
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void GetActorCorners(AFGBuildable* Buildable, TArray<FVector>& Corners);

	// The same box in the building's local space; false when there is none
	static bool GetLocalBox(const AFGBuildable* Buildable, FBox& OutBox);
};

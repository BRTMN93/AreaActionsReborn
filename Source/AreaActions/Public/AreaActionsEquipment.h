#pragma once

#include "CoreMinimal.h"
#include "AreaActionsTypes.h"
#include "Equipment/FGEquipment.h"
#include "AreaActionsEquipment.generated.h"

class AAreaActionBase;
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

	// The actions in the tool's menu: the world module's list (both on the server and on clients)
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void GetAvailableActions(TArray<TSubclassOf<AAreaActionBase>>& Actions);

	// The selection as a request carries it, read from the Blueprint's CornerLocations, MinZ and MaxZ; false when they are missing
	bool GetArea(FAreaActionsArea& Out) const;
};

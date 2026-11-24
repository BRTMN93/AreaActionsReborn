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
	// The Blueprint's WasEquipped and WasUnEquipped make and remove the tool's windows, indicators and input mode. The game calls both for
	// every player's tool, also on the server and on the other clients: there only the game's own part runs, the Blueprint's is skipped.
	// Their Tick runs only for a tool this game's own player holds. A WasEquipped that came before the character was the local one is
	// made up for on the next tick.
	virtual void ProcessEvent(UFunction* Function, void* Parameters) override;

	// The Blueprint's InitOnServer(Action) is a "Run on Server" event that spawns the action's actor of the class it is given. The action
	// with its windows belongs to the player's own game and the server takes no class from a client, so here the event runs where it is
	// called and never goes over the network; one that still arrives from a client is dropped (ProcessEvent).
	virtual int32 GetFunctionCallspace(UFunction* Function, FFrame* Stack) override;

	// The 8 corners of the building's box in the world: its clearance box, or without one, the bounds of its class default and of its
	// lightweight instance meshes. Empty for classes the copy skips that have no clearance (as the original did).
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void GetActorCorners(AFGBuildable* Buildable, TArray<FVector>& Corners);

	// The same box in the building's local space; false when there is none
	static bool GetLocalBox(const AFGBuildable* Buildable, FBox& OutBox);

	// The actions in the tool's menu: the world module's list (both on the server and on clients)
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void GetAvailableActions(TArray<TSubclassOf<AAreaActionBase>>& Actions);

	// The building under the mouse becomes the area: its 4 corners and its height, actors and lightweights alike (the original's
	// SelectBuilding missed lightweights)
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void SelectBuilding();

	// The Blueprint's RaycastMouseWithRange past the tool's own indicators; false when nothing is in range
	bool RaycastMouse(FHitResult& Out);

	// The selection as a request carries it, read from the Blueprint's CornerLocations, MinZ and MaxZ; false when they are missing
	bool GetArea(FAreaActionsArea& Out) const;

protected:
	virtual void WasEquipped_Implementation() override;

private:
	// The Blueprint's WasEquipped ran here (the tool is this game's own player's), so its WasUnEquipped has to run as well
	bool bEquippedLocally = false;
	// The Blueprint's WasEquipped runs late: the game's own part of it has run already
	bool bSkipNativeEquip = false;
	bool bRejectLogged = false;
};

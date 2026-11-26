#pragma once

#include "CoreMinimal.h"
#include "AreaActionsServerSubsystem.h"

class UFGRecipe;

// The server jobs of the simple actions. The RCO has checked the request before it makes one.
namespace AreaActionsJobs
{
	// Manufacturers whose class the recipe lists get it; their solid items go to return crates at the player (D8), fluids are lost (D14),
	// machines that already use the recipe are left alone (D12)
	TUniquePtr<FAreaActionsJob> MakeSetRecipe(TSubclassOf<UFGRecipe> Recipe);
	// Every inventory of the buildings and crates in the area, except the power shard and somersloop slots (D11); with bToCrate the solid
	// items go to return crates at the player, fluids are lost
	TUniquePtr<FAreaActionsJob> MakeClearInventories(bool bToCrate);
	// Truck stations (docking stations) and freight platforms
	TUniquePtr<FAreaActionsJob> MakeSetStationMode(bool bLoad);
	// Copy and Fill: copies of the area's buildings where the placement puts them, except onto an equal building; with bUseCost the build
	// cost comes from the player's inventory, then the storage containers within ContainerDistance cm of the player, then the dimensional
	// depot (D17, D18): the whole cost must be there at the start (else nothing is built), each copy is paid in the frame it is built (D22)
	TUniquePtr<FAreaActionsJob> MakePlaceCopies(const FAreaActionsPlacement& Placement, bool bUseCost, double ContainerDistance);
	// MassDismantle: every building, lightweight and crate in the area the game lets anyone dismantle; the refund goes into return crates
	// at the player, fluids are lost
	TUniquePtr<FAreaActionsJob> MakeDismantle(EAreaActionsDismantleRefund Refund);
	// The test mode: 1 = return crates find no room next to the player, 2 = no crate can be placed at all (the messages, AA-085); 0 = off
	void SetCratesFailForTest(int32 Mode);
	// The test mode: every Nth copy of a Copy or Fill counts as refused by the game (a refused copy costs nothing); 0 = off
	void SetRefuseCopiesForTest(int32 EveryNth);
	// Most copies one request may build (AreaActions.MaxSpawnPerRequest)
	int32 GetMaxSpawn();
}

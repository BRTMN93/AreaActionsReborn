#pragma once

#include "CoreMinimal.h"
#include "AreaActionsServerSubsystem.h"

class UFGRecipe;

// The server jobs of the simple actions. The RCO has checked the request before it makes one.
namespace AreaActionsJobs
{
	// Manufacturers whose class the recipe lists get it; their solid items go to one crate at the player (D8), fluids are lost (D14),
	// machines that already use the recipe are left alone (D12)
	TUniquePtr<FAreaActionsJob> MakeSetRecipe(TSubclassOf<UFGRecipe> Recipe);
	// Every inventory of the buildings and crates in the area, except the power shard and somersloop slots (D11)
	TUniquePtr<FAreaActionsJob> MakeClearInventories();
	// Truck stations (docking stations) and freight platforms
	TUniquePtr<FAreaActionsJob> MakeSetStationMode(bool bLoad);
}

#include "AreaActionsJobs.h"
#include "AreaActionsLog.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGCharacterPlayer.h"
#include "FGCrate.h"
#include "FGInventoryComponent.h"
#include "FGItemPickup_Spawnable.h"
#include "FGRecipe.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	using FReturnedItems = TMap<TSubclassOf<UFGItemDescriptor>, int32>;

	struct FTaken
	{
		int32 Solids = 0;
		// Liquids and gases, in inventory units (1000 per m3)
		int32 Fluid = 0;
	};

	// Counts the inventory's contents and empties it; the solid stacks also go into Returned when it is given (fluids cannot go
	// into a crate)
	FTaken TakeAll(UFGInventoryComponent* Inventory, FReturnedItems* Returned)
	{
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks);
		FTaken Taken;
		for (const FInventoryStack& Stack : Stacks)
		{
			if (!Stack.HasItems())
			{
				continue;
			}
			if (UFGItemDescriptor::GetForm(Stack.Item.GetItemClass()) != EResourceForm::RF_SOLID)
			{
				Taken.Fluid += Stack.NumItems;
				continue;
			}
			Taken.Solids += Stack.NumItems;
			if (Returned)
			{
				Returned->FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
			}
		}
		Inventory->Empty();
		return Taken;
	}

	// One crate with the items next to the player; when the player left or there is no room there, where the last changed building
	// stands, rather than lose them
	void SpawnReturnCrate(UWorld* World, const FReturnedItems& Returned, AFGCharacterPlayer* Player, const FVector& Fallback, const TCHAR* JobName)
	{
		if (Returned.IsEmpty())
		{
			return;
		}
		TArray<FInventoryStack> Stacks;
		int32 Items = 0;
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Returned)
		{
			const int32 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(Entry.Key));
			for (int32 Left = Entry.Value; Left > 0; Left -= StackSize)
			{
				Stacks.Emplace(FMath::Min(Left, StackSize), Entry.Key);
			}
			Items += Entry.Value;
		}
		FSpawnInventoryCrateParameters Parameters;
		Parameters.bAllowCrateMerging = false;
		if (Player)
		{
			Parameters.ActorsToIgnore.Add(Player);
		}
		const AFGCrate* Crate = Player ? AFGItemPickup_Spawnable::SpawnInventoryCrate(World, Stacks, Player->GetActorLocation(), Parameters) : nullptr;
		if (!Crate)
		{
			Crate = AFGItemPickup_Spawnable::SpawnInventoryCrate(World, Stacks, Fallback, Parameters);
		}
		if (!Crate)
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("%s could not spawn the crate: %d items in %d stacks lost"), JobName, Items, Stacks.Num());
			return;
		}
		UE_LOG(LogAreaActionsCpp, Display, TEXT("%s crate %s with %d items in %d stacks"), JobName, *GetNameSafe(Crate), Items, Stacks.Num());
	}

	class FSetRecipeJob final : public FAreaActionsJob
	{
	public:
		explicit FSetRecipeJob(TSubclassOf<UFGRecipe> InRecipe) : Recipe(InRecipe)
		{
			for (const TSubclassOf<UObject>& Class : UFGRecipe::GetProducedIn(Recipe))
			{
				ProducedIn.Add(Class.Get());
			}
		}

		virtual const TCHAR* GetName() const override { return TEXT("SetRecipe"); }

		// The original's test: the machine's own class is in the recipe's list
		virtual bool Matches(const AActor* Actor) const override
		{
			return Actor->IsA<AFGBuildableManufacturer>() && ProducedIn.Contains(Actor->GetClass());
		}

		virtual void Apply(AActor* Actor) override
		{
			AFGBuildableManufacturer* Machine = CastChecked<AFGBuildableManufacturer>(Actor);
			if (Machine->GetCurrentRecipe() == Recipe)
			{
				++AlreadySet;
				return;
			}
			for (UFGInventoryComponent* Inventory : { Machine->GetInputInventory(), Machine->GetOutputInventory() })
			{
				if (Inventory)
				{
					Result.Items += TakeAll(Inventory, &Returned).Solids;
				}
			}
			Machine->SetRecipe(Recipe);
			LastMachine = Machine->GetActorLocation();
			++Result.Buildings;
			Result.AddCount(Machine->GetClass());
		}

		virtual void Finish(UWorld* World) override
		{
			if (Result.Buildings == 0 && AlreadySet > 0)
			{
				Result.Code = EAreaActionsResultCode::AlreadySet;
			}
			FAreaActionsJob::Finish(World);
			SpawnReturnCrate(World, Returned, Character.Get(), LastMachine, GetName());
		}

	private:
		TSubclassOf<UFGRecipe> Recipe;
		TArray<UClass*> ProducedIn;
		FReturnedItems Returned;
		FVector LastMachine = FVector::ZeroVector;
		int32 AlreadySet = 0;
	};

	class FClearInventoriesJob final : public FAreaActionsJob
	{
	public:
		explicit FClearInventoriesJob(bool bInToCrate) : bToCrate(bInToCrate) {}

		virtual const TCHAR* GetName() const override { return TEXT("ClearInventories"); }
		virtual bool Matches(const AActor* Actor) const override { return Actor->FindComponentByClass<UFGInventoryComponent>() != nullptr; }
		virtual bool WantsCrates() const override { return true; }

		virtual void Apply(AActor* Actor) override
		{
			// Power shards and somersloops are upgrades, not contents (D11)
			const AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Actor);
			const UFGInventoryComponent* Upgrades = Factory ? Factory->GetPotentialInventory() : nullptr;
			TInlineComponentArray<UFGInventoryComponent*> Inventories(Actor);
			FTaken Taken;
			for (UFGInventoryComponent* Inventory : Inventories)
			{
				if (Inventory && Inventory != Upgrades)
				{
					const FTaken One = TakeAll(Inventory, bToCrate ? &Returned : nullptr);
					Taken.Solids += One.Solids;
					Taken.Fluid += One.Fluid;
				}
			}
			// Only buildings something was taken from count, as the player sees it (AA-079)
			if (Taken.Solids > 0 || Taken.Fluid > 0)
			{
				Result.Items += Taken.Solids;
				Result.Fluid += Taken.Fluid;
				LastBuilding = Actor->GetActorLocation();
				++Result.Buildings;
				Result.AddCount(Actor->GetClass());
			}
		}

		virtual void Finish(UWorld* World) override
		{
			FAreaActionsJob::Finish(World);
			SpawnReturnCrate(World, Returned, Character.Get(), LastBuilding, GetName());
		}

	private:
		bool bToCrate;
		FReturnedItems Returned;
		FVector LastBuilding = FVector::ZeroVector;
	};

	class FSetStationModeJob final : public FAreaActionsJob
	{
	public:
		explicit FSetStationModeJob(bool bInLoad) : bLoad(bInLoad) {}

		virtual const TCHAR* GetName() const override { return TEXT("SetStationMode"); }

		virtual bool Matches(const AActor* Actor) const override
		{
			return Actor->IsA<AFGBuildableDockingStation>() || Actor->IsA<AFGBuildableTrainPlatformCargo>();
		}

		virtual void Apply(AActor* Actor) override
		{
			if (AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(Actor))
			{
				Station->SetIsInLoadMode(bLoad);
			}
			else
			{
				CastChecked<AFGBuildableTrainPlatformCargo>(Actor)->SetIsInLoadMode(bLoad);
			}
			++Result.Buildings;
			Result.AddCount(Actor->GetClass());
		}

	private:
		bool bLoad;
	};
}

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakeSetRecipe(TSubclassOf<UFGRecipe> Recipe)
{
	return MakeUnique<FSetRecipeJob>(Recipe);
}

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakeClearInventories(bool bToCrate)
{
	return MakeUnique<FClearInventoriesJob>(bToCrate);
}

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakeSetStationMode(bool bLoad)
{
	return MakeUnique<FSetStationModeJob>(bLoad);
}

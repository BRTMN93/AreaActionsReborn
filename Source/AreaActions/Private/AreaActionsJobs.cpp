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
				if (!Inventory)
				{
					continue;
				}
				TArray<FInventoryStack> Stacks;
				Inventory->GetInventoryStacks(Stacks);
				for (const FInventoryStack& Stack : Stacks)
				{
					if (Stack.HasItems() && UFGItemDescriptor::GetForm(Stack.Item.GetItemClass()) == EResourceForm::RF_SOLID)
					{
						Returned.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
						Result.Items += Stack.NumItems;
					}
				}
				Inventory->Empty();
			}
			Machine->SetRecipe(Recipe);
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
			if (Returned.IsEmpty())
			{
				return;
			}
			TArray<FInventoryStack> Stacks;
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Returned)
			{
				const int32 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(Entry.Key));
				for (int32 Left = Entry.Value; Left > 0; Left -= StackSize)
				{
					Stacks.Emplace(FMath::Min(Left, StackSize), Entry.Key);
				}
			}
			AFGCharacterPlayer* Player = Character.Get();
			// The player left the world between the check and now: the items stay where the last machine stood rather than vanish
			const FVector Where = Player ? Player->GetActorLocation() : Found.Last().IsValid() ? Found.Last()->GetActorLocation() : FVector::ZeroVector;
			FSpawnInventoryCrateParameters Parameters;
			Parameters.bAllowCrateMerging = false;
			if (Player)
			{
				Parameters.ActorsToIgnore.Add(Player);
			}
			const AFGCrate* Crate = AFGItemPickup_Spawnable::SpawnInventoryCrate(World, Stacks, Where, Parameters);
			UE_LOG(LogAreaActionsCpp, Display, TEXT("SetRecipe crate %s with %d items in %d stacks"), *GetNameSafe(Crate), Result.Items, Stacks.Num());
		}

	private:
		TSubclassOf<UFGRecipe> Recipe;
		TArray<UClass*> ProducedIn;
		TMap<TSubclassOf<UFGItemDescriptor>, int32> Returned;
		int32 AlreadySet = 0;
	};

	class FClearInventoriesJob final : public FAreaActionsJob
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("ClearInventories"); }
		virtual bool Matches(const AActor* Actor) const override { return true; }
		virtual bool WantsCrates() const override { return true; }

		virtual void Apply(AActor* Actor) override
		{
			// Power shards and somersloops are upgrades, not contents (D11)
			const AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Actor);
			const UFGInventoryComponent* Upgrades = Factory ? Factory->GetPotentialInventory() : nullptr;
			TInlineComponentArray<UFGInventoryComponent*> Inventories(Actor);
			bool bAny = false;
			for (UFGInventoryComponent* Inventory : Inventories)
			{
				if (Inventory && Inventory != Upgrades)
				{
					bAny = true;
					Result.Items += Inventory->GetNumItems(nullptr);
					Inventory->Empty();
				}
			}
			if (bAny)
			{
				++Result.Buildings;
				Result.AddCount(Actor->GetClass());
			}
		}
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

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakeClearInventories()
{
	return MakeUnique<FClearInventoriesJob>();
}

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakeSetStationMode(bool bLoad)
{
	return MakeUnique<FSetStationModeJob>(bLoad);
}

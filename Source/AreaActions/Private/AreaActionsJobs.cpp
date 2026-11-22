#include "AreaActionsJobs.h"
#include "AreaActionsBuildings.h"
#include "AreaActionsLog.h"
#include "Buildables/FGBuildableStorage.h"
#include "FGBuildableSubsystem.h"
#include "FGCentralStorageSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGCharacterPlayer.h"
#include "FGCrate.h"
#include "FGInventoryComponent.h"
#include "FGItemPickup_Spawnable.h"
#include "FGRecipe.h"
#include "Resources/FGItemDescriptor.h"

static TAutoConsoleVariable<int32> CVarMaxSpawnPerRequest(TEXT("AreaActions.MaxSpawnPerRequest"), 10000,
	TEXT("Most buildings one Copy or Fill may build; a larger request is refused before anything changes"));

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

	// The build cost's sources in the order the player's items go first (D18)
	class FCostSources
	{
	public:
		FCostSources(UWorld* World, AFGCharacterPlayer* Player, double Distance)
		{
			if (Player && Player->GetInventory())
			{
				Inventories.Add(Player->GetInventory());
			}
			if (Player && Distance > 0.0)
			{
				const FVector At = Player->GetActorLocation();
				TArray<AFGBuildableStorage*> Storages;
				if (const AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World))
				{
					Buildables->GetTypedBuildable<AFGBuildableStorage>(Storages);
				}
				Storages.RemoveAll([&At, Distance](AFGBuildableStorage* Storage)
				{
					return !IsValid(Storage) || !Storage->GetStorageInventory() || FVector::Dist(Storage->GetActorLocation(), At) > Distance;
				});
				// The nearest first, so a container farther away keeps its items when a nearer one has enough
				Storages.Sort([&At](const AFGBuildableStorage& A, const AFGBuildableStorage& B) { return FVector::DistSquared(A.GetActorLocation(), At) < FVector::DistSquared(B.GetActorLocation(), At); });
				for (AFGBuildableStorage* Storage : Storages)
				{
					Inventories.Add(Storage->GetStorageInventory());
				}
			}
			Depot = AFGCentralStorageSubsystem::Get(World);
		}

		int32 Count(TSubclassOf<UFGItemDescriptor> Item) const
		{
			int64 Total = Depot ? Depot->GetNumItemsFromCentralStorage(Item) : 0;
			for (const UFGInventoryComponent* Inventory : Inventories)
			{
				Total += Inventory->GetNumItems(Item);
			}
			return static_cast<int32>(FMath::Min<int64>(Total, MAX_int32));
		}

		void Take(TSubclassOf<UFGItemDescriptor> Item, int32 Amount)
		{
			for (UFGInventoryComponent* Inventory : Inventories)
			{
				const int32 Here = FMath::Min(Amount, Inventory->GetNumItems(Item));
				if (Here > 0)
				{
					Inventory->Remove(Item, Here);
					Amount -= Here;
				}
			}
			if (Amount > 0 && Depot)
			{
				Amount -= Depot->TryRemoveItemsFromCentralStorage(Item, Amount);
			}
			if (Amount > 0)
			{
				UE_LOG(LogAreaActionsCpp, Error, TEXT("PlaceCopies: %d %s counted but not found when taken"), Amount, *GetNameSafe(Item));
			}
		}

	private:
		TArray<UFGInventoryComponent*> Inventories;
		AFGCentralStorageSubsystem* Depot = nullptr;
	};

	class FPlaceCopiesJob final : public FAreaActionsJob
	{
	public:
		FPlaceCopiesJob(const FAreaActionsPlacement& InPlacement, bool bInUseCost, double InDistance)
			: Placement(InPlacement), bUseCost(bInUseCost), ContainerDistance(InDistance) {}

		virtual const TCHAR* GetName() const override { return Placement.bFill ? TEXT("Fill") : TEXT("Copy"); }
		virtual int32 GetItems() const override { return Copies.Num(); }

		virtual bool Step(UWorld* World, double EndTime) override
		{
			if (!bCollected)
			{
				if (!Collector.Step(World, Area, Buildings, EndTime))
				{
					return false;
				}
				bCollected = true;
				if (!Prepare(World))
				{
					return true;
				}
			}
			while (Next < Copies.Num())
			{
				const AreaActionsBuildings::FPlanned& Copy = Copies[Next++];
				const AreaActionsBuildings::FSnapshot& Source = Buildings[Copy.Source];
				if (AreaActionsBuildings::Spawn(World, Source, Copy.Transform))
				{
					++Result.Buildings;
					Result.AddCount(Source.Class);
				}
				else
				{
					++Failed;
					if (bCostTaken)
					{
						AreaActionsBuildings::AddCost(World, Source, Refund);
					}
				}
				if (FPlatformTime::Seconds() > EndTime)
				{
					return Next >= Copies.Num();
				}
			}
			return true;
		}

		virtual void Finish(UWorld* World) override
		{
			// An area whose copies all stand already is a success with 0 buildings (CopyTwice), not an empty area
			if (Result.Code == EAreaActionsResultCode::Ok && Result.Buildings == 0 && bNoSource)
			{
				Result.Code = EAreaActionsResultCode::NothingInArea;
			}
			if (Failed > 0)
			{
				UE_LOG(LogAreaActionsCpp, Warning, TEXT("%s: the game refused %d copies"), GetName(), Failed);
			}
			// A job ended early (the player left) gives back the cost of the copies it did not get to
			if (bCostTaken && Next < Copies.Num())
			{
				UE_LOG(LogAreaActionsCpp, Display, TEXT("%s ended after %d of %d copies"), GetName(), Next, Copies.Num());
				for (; Next < Copies.Num(); ++Next)
				{
					AreaActionsBuildings::AddCost(World, Buildings[Copies[Next].Source], Refund);
				}
			}
			GiveBack(World);
		}

	private:
		// Plans the copies, drops those onto an equal building, checks and takes the cost; false when nothing is to be built
		bool Prepare(UWorld* World)
		{
			int32 SourceCount = 0;
			TSet<UClass*> Classes;
			for (const AreaActionsBuildings::FSnapshot& Building : Buildings)
			{
				if (!Building.bIgnored)
				{
					++SourceCount;
					Classes.Add(Building.Class);
				}
			}
			if (SourceCount == 0)
			{
				bNoSource = true;
				return false;
			}
			// The count before the plan, in doubles: a request can ask for more than an int holds
			double Asked = SourceCount;
			if (Placement.bFill)
			{
				Asked *= FMath::Max(1.0, static_cast<double>(FMath::Abs(Placement.Amount.X)) * FMath::Abs(Placement.Amount.Y) * FMath::Abs(Placement.Amount.Z)) - 1.0;
				if (Placement.Amount.X == 0 || Placement.Amount.Y == 0 || Placement.Amount.Z == 0)
				{
					Asked = 0.0;
				}
			}
			const int32 Limit = AreaActionsJobs::GetMaxSpawn();
			if (Asked > Limit)
			{
				Result.Code = EAreaActionsResultCode::TooMany;
				Result.Buildings = static_cast<int32>(FMath::Min<double>(Asked, MAX_int32));
				Result.Items = Limit;
				return false;
			}
			const AreaActionsBuildings::FFrame Frame = AreaActionsBuildings::ComputeFrame(Buildings);
			TArray<AreaActionsBuildings::FPlanned> Planned;
			AreaActionsBuildings::Plan(Buildings, Frame, Placement, Limit, Planned);
			// shortcut: the standing buildings of the copied classes are read in one frame; fine to some 100k buildings, split it when
			// bigger saves show long frames
			AreaActionsBuildings::FExisting Existing;
			Existing.Build(World, Classes);
			for (const AreaActionsBuildings::FPlanned& Copy : Planned)
			{
				UClass* Class = Buildings[Copy.Source].Class;
				if (!Existing.Contains(Class, Copy.Transform))
				{
					Existing.Add(Class, Copy.Transform);
					Copies.Add(Copy);
				}
			}
			const AFGCharacterPlayer* Player = Character.Get();
			const UFGInventoryComponent* Inventory = Player ? Player->GetInventory() : nullptr;
			if (Copies.IsEmpty() || !bUseCost || (Inventory && Inventory->GetNoBuildCost()))
			{
				return !Copies.IsEmpty();
			}
			AreaActionsBuildings::FItemCounts Cost;
			for (const AreaActionsBuildings::FPlanned& Copy : Copies)
			{
				AreaActionsBuildings::AddCost(World, Buildings[Copy.Source], Cost);
			}
			FCostSources From(World, Character.Get(), ContainerDistance);
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Cost)
			{
				if (const int32 Have = From.Count(Entry.Key); Have < Entry.Value && Result.Missing.Num() < FAreaActionsResult::MaxCounts)
				{
					Result.Missing.Add({ Entry.Key.Get(), Entry.Value - Have });
				}
			}
			if (!Result.Missing.IsEmpty())
			{
				Result.Code = EAreaActionsResultCode::MissingItems;
				Copies.Reset();
				return false;
			}
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Cost)
			{
				From.Take(Entry.Key, Entry.Value);
				Result.Items += Entry.Value;
			}
			bCostTaken = true;
			return true;
		}

		// The cost of the copies the game refused goes back to the player, what does not fit into a crate next to them
		void GiveBack(UWorld* World)
		{
			if (Refund.IsEmpty())
			{
				return;
			}
			AFGCharacterPlayer* Player = Character.Get();
			UFGInventoryComponent* Inventory = Player ? Player->GetInventory() : nullptr;
			FReturnedItems Left;
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Refund)
			{
				Result.Items -= Entry.Value;
				const int32 Added = Inventory ? Inventory->AddStack(FInventoryStack(Entry.Value, Entry.Key), true) : 0;
				if (Entry.Value > Added)
				{
					Left.Add(Entry.Key, Entry.Value - Added);
				}
			}
			SpawnReturnCrate(World, Left, Player, Area.Polygon.IsEmpty() ? FVector::ZeroVector : FVector(Area.Polygon[0], 0.0), GetName());
		}

		FAreaActionsPlacement Placement;
		bool bUseCost;
		double ContainerDistance;
		AreaActionsBuildings::FCollector Collector;
		TArray<AreaActionsBuildings::FSnapshot> Buildings;
		TArray<AreaActionsBuildings::FPlanned> Copies;
		AreaActionsBuildings::FItemCounts Refund;
		int32 Next = 0;
		int32 Failed = 0;
		bool bCostTaken = false;
		bool bNoSource = false;
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

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakePlaceCopies(const FAreaActionsPlacement& Placement, bool bUseCost, double ContainerDistance)
{
	return MakeUnique<FPlaceCopiesJob>(Placement, bUseCost, ContainerDistance);
}

int32 AreaActionsJobs::GetMaxSpawn()
{
	return FMath::Max(1, CVarMaxSpawnPerRequest.GetValueOnGameThread());
}

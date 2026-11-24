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
#include "EngineUtils.h"
#include "Algo/Count.h"
#include "Buildables/FGBuildableWire.h"
#include "FGCircuitConnectionComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGItemPickup_Spawnable.h"
#include "FGRecipe.h"
#include "Resources/FGItemDescriptor.h"

static TAutoConsoleVariable<int32> CVarMaxSpawnPerRequest(TEXT("AreaActions.MaxSpawnPerRequest"), 10000,
	TEXT("Most buildings one Copy or Fill may build; a larger request is refused before anything changes"));

namespace
{
	struct FTaken
	{
		int32 Solids = 0;
		// Liquids and gases, in inventory units (1000 per m3)
		int32 Fluid = 0;
	};

	// The return crates of one job. Stacks go in the moment they leave a building, as they are (their item state too), so a save made
	// while the job runs finds every item either still in its building or in a crate (AA-078). A crate takes at most MaxStacks stacks
	// (AA-085). At the end the crates are packed again (AA-093): as few crates as the stacks need, each item type whole in one crate and
	// the stacks sorted by type; only a type with more than MaxStacks stacks of its own spans crates.
	class FReturnCrates
	{
	public:
		// 200 stacks keep a crate's window and the replication of its inventory small: about four Industrial Storage Containers
		static constexpr int32 MaxStacks = 200;

		explicit FReturnCrates(const TCHAR* InJob) : Job(InJob) {}

		// A solid stack into the current crate or a new one next to the player; when the player left or there is no room there, where
		// Fallback is (the building the items came from), rather than lose them. Returns the items placed; the rest is counted as lost
		int32 Add(UWorld* World, const FInventoryStack& Stack, AFGCharacterPlayer* Player, const FVector& Fallback)
		{
			LastWorld = World;
			LastPlayer = Player;
			LastFallback = Fallback;
			FInventoryStack Left = Stack;
			while (Left.HasItems())
			{
				if (AFGCrate* Crate = Current.Get())
				{
					Crate->SetAllowAddingItems(true);
					const int32 Added = Crate->GetInventory()->AddStack(Left, true);
					Crate->SetAllowAddingItems(false);
					Left.NumItems -= Added;
					Items += Added;
					if (Added > 0)
					{
						continue;
					}
				}
				// A crate that took nothing would be followed by another forever
				if (!Spawn(World, Left, Player, Fallback))
				{
					Lost += Left.NumItems;
					return Stack.NumItems - Left.NumItems;
				}
			}
			return Stack.NumItems;
		}

		// Packs the crates again, shrinks them to the stacks they hold; the counts go into the result
		void Finish(FAreaActionsResult& Result)
		{
			Repack();
			for (const TWeakObjectPtr<AFGCrate>& Weak : All)
			{
				UFGInventoryComponent* Inventory = Weak.IsValid() ? Weak->GetInventory() : nullptr;
				if (!Inventory)
				{
					continue;
				}
				// Only the empty slots after the last stack go: a player may have taken items from the middle meanwhile
				int32 Used = 0;
				for (int32 Index = 0; Index < Inventory->GetSizeLinear(); ++Index)
				{
					Used = Inventory->IsSomethingOnIndex(Index) ? Index + 1 : Used;
				}
				if (Used > 0 && Used < Inventory->GetSizeLinear())
				{
					Weak->SetAllowAddingItems(true);
					Inventory->Resize(Used);
					Weak->SetAllowAddingItems(false);
				}
			}
			Away = Algo::CountIf(All, [this](const TWeakObjectPtr<AFGCrate>& Weak) { return AwayCrates.Contains(Weak); });
			Result.Crates = All.Num();
			Result.CratesAway = Away;
			Result.Lost = Lost;
			if (Lost > 0)
			{
				UE_LOG(LogAreaActionsCpp, Error, TEXT("%s could not place a crate: %d items lost"), Job, Lost);
			}
			if (!All.IsEmpty())
			{
				UE_LOG(LogAreaActionsCpp, Display, TEXT("%s crates %d (%d away from the player) with %d items"), Job, All.Num(), Away, Items);
			}
		}

		// The test mode's failures (AA-085, the messages): 1 = no room next to the player, 2 = no crate anywhere
		static int32 FailForTest;

	private:
		// All stacks out of the crates and back in by first-fit-decreasing: a type's whole crates first, then the rest of each type,
		// the largest first, into the first crate with room for all of it. Item states stay (stacks with a state are moved as they are)
		void Repack()
		{
			TArray<AFGCrate*> Crates;
			TMap<UClass*, TArray<FInventoryStack>> ByType;
			for (const TWeakObjectPtr<AFGCrate>& Weak : All)
			{
				AFGCrate* Crate = Weak.Get();
				UFGInventoryComponent* Inventory = Crate ? Crate->GetInventory() : nullptr;
				if (!Inventory)
				{
					continue;
				}
				Crates.Add(Crate);
				TArray<FInventoryStack> Stacks;
				Inventory->GetInventoryStacks(Stacks);
				for (const FInventoryStack& Stack : Stacks)
				{
					if (Stack.HasItems())
					{
						ByType.FindOrAdd(Stack.Item.GetItemClass()).Add(Stack);
					}
				}
				Crate->SetAllowAddingItems(true);
				Inventory->Empty();
				Inventory->Resize(MaxStacks);
				Crate->SetAllowAddingItems(false);
			}
			if (Crates.IsEmpty())
			{
				return;
			}
			ByType.KeySort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
			TArray<TArray<FInventoryStack>> Bins;
			TArray<TArray<FInventoryStack>> Rests;
			for (TPair<UClass*, TArray<FInventoryStack>>& Entry : ByType)
			{
				TArray<FInventoryStack> Stacks = MergeStacks(Entry.Key, Entry.Value);
				while (Stacks.Num() >= MaxStacks)
				{
					Bins.Emplace(Stacks.GetData(), MaxStacks);
					Stacks.RemoveAt(0, MaxStacks, EAllowShrinking::No);
				}
				if (!Stacks.IsEmpty())
				{
					Rests.Add(MoveTemp(Stacks));
				}
			}
			const int32 WholeBins = Bins.Num();
			Rests.StableSort([](const TArray<FInventoryStack>& A, const TArray<FInventoryStack>& B) { return A.Num() > B.Num(); });
			for (TArray<FInventoryStack>& Rest : Rests)
			{
				int32 Into = INDEX_NONE;
				for (int32 Index = WholeBins; Index < Bins.Num() && Into == INDEX_NONE; ++Index)
				{
					Into = Bins[Index].Num() + Rest.Num() <= MaxStacks ? Index : INDEX_NONE;
				}
				if (Into == INDEX_NONE)
				{
					Bins.Emplace();
					Into = Bins.Num() - 1;
				}
				Bins[Into].Append(Rest);
			}
			TArray<TWeakObjectPtr<AFGCrate>> Kept;
			for (int32 Index = 0; Index < Bins.Num(); ++Index)
			{
				TArray<FInventoryStack>& Bin = Bins[Index];
				Bin.StableSort([](const FInventoryStack& A, const FInventoryStack& B) { return GetNameSafe(A.Item.GetItemClass()) < GetNameSafe(B.Item.GetItemClass()); });
				AFGCrate* Crate = Index < Crates.Num() ? Crates[Index] : nullptr;
				int32 From = 0;
				if (!Crate)
				{
					bool bAway = false;
					Crate = NewCrate(LastWorld.Get(), Bin[0], LastPlayer.Get(), LastFallback, bAway);
					if (!Crate)
					{
						// No crate beyond the ones there were: the stacks go wherever the kept crates have room. They always fit, the old
						// crates held them all before and merging only lowers the number of stacks; anything left would be lost
						for (FInventoryStack Stack : Bin)
						{
							for (int32 Other = 0; Other < Crates.Num() && Stack.HasItems(); ++Other)
							{
								Crates[Other]->SetAllowAddingItems(true);
								Stack.NumItems -= Crates[Other]->GetInventory()->AddStack(Stack, true);
								Crates[Other]->SetAllowAddingItems(false);
							}
							Lost += Stack.NumItems;
							Items -= Stack.NumItems;
						}
						continue;
					}
					From = 1;
					if (bAway)
					{
						AwayCrates.Add(Crate);
					}
				}
				Crate->SetAllowAddingItems(true);
				Crate->GetInventory()->Resize(MaxStacks);
				for (int32 Stack = From; Stack < Bin.Num(); ++Stack)
				{
					const int32 Added = Crate->GetInventory()->AddStack(Bin[Stack], true);
					Lost += Bin[Stack].NumItems - Added;
					Items -= Bin[Stack].NumItems - Added;
				}
				Crate->SetAllowAddingItems(false);
				Kept.Add(Crate);
			}
			for (int32 Index = Bins.Num(); Index < Crates.Num(); ++Index)
			{
				Crates[Index]->Destroy();
			}
			All = MoveTemp(Kept);
			Current = nullptr;
		}

		// A type's stacks without an item state as full stacks of its stack size; stacks with a state stay as they are
		static TArray<FInventoryStack> MergeStacks(UClass* Type, const TArray<FInventoryStack>& Stacks)
		{
			TArray<FInventoryStack> Out;
			int32 Count = 0;
			for (const FInventoryStack& Stack : Stacks)
			{
				if (Stack.Item.HasState())
				{
					Out.Add(Stack);
				}
				else
				{
					Count += Stack.NumItems;
				}
			}
			const int32 Size = FMath::Max(1, UFGItemDescriptor::GetStackSize(TSubclassOf<UFGItemDescriptor>(Type)));
			for (; Count > 0; Count -= Size)
			{
				Out.Emplace(FMath::Min(Count, Size), TSubclassOf<UFGItemDescriptor>(Type));
			}
			return Out;
		}

		// A crate with First in it next to the player, or by Fallback when there is no room there; rows of five, 1.5 m apart, so the
		// game does not have to look for room for each
		AFGCrate* NewCrate(UWorld* World, const FInventoryStack& First, AFGCharacterPlayer* Player, const FVector& Fallback, bool& bAway)
		{
			if (!World || FailForTest == 2)
			{
				return nullptr;
			}
			FSpawnInventoryCrateParameters Parameters;
			Parameters.bAllowCrateMerging = false;
			if (Player)
			{
				Parameters.ActorsToIgnore.Add(Player);
			}
			static constexpr int32 Columns[] = { 0, 1, -1, 2, -2 };
			const int32 Place = All.Num() + Extra++;
			const FVector Offset(Columns[Place % 5] * 150.0, (Place / 5 + 1) * 150.0, 0.0);
			AFGCrate* Crate = Player && FailForTest != 1 ? AFGItemPickup_Spawnable::SpawnInventoryCrate(World, { First }, Player->GetActorLocation() + Offset, Parameters) : nullptr;
			bAway = false;
			if (!Crate)
			{
				Crate = AFGItemPickup_Spawnable::SpawnInventoryCrate(World, { First }, Fallback + Offset, Parameters);
				bAway = true;
			}
			if (Crate && (!Crate->GetInventory() || Crate->GetInventory()->GetNumItems(nullptr) != First.NumItems))
			{
				Crate->Destroy();
				return nullptr;
			}
			return Crate;
		}

		// Takes what the new crate holds out of Left; false when there is no crate or it took nothing
		bool Spawn(UWorld* World, FInventoryStack& Left, AFGCharacterPlayer* Player, const FVector& Fallback)
		{
			// The first stack within its stack size, the rest goes in after the crate has room
			FInventoryStack First = Left;
			First.NumItems = FMath::Min(Left.NumItems, FMath::Max(1, UFGItemDescriptor::GetStackSize(Left.Item.GetItemClass())));
			bool bAway = false;
			AFGCrate* Crate = NewCrate(World, First, Player, Fallback, bAway);
			if (!Crate)
			{
				return false;
			}
			if (bAway)
			{
				AwayCrates.Add(Crate);
			}
			Items += First.NumItems;
			Left.NumItems -= First.NumItems;
			Crate->SetAllowAddingItems(true);
			Crate->GetInventory()->Resize(MaxStacks);
			Crate->SetAllowAddingItems(false);
			Current = Crate;
			All.Add(Crate);
			return true;
		}

		const TCHAR* Job;
		TWeakObjectPtr<AFGCrate> Current;
		TArray<TWeakObjectPtr<AFGCrate>> All;
		TSet<TWeakObjectPtr<AFGCrate>> AwayCrates;
		// Where the last stack came from, for crates the packing at the end needs beyond the ones there are
		TWeakObjectPtr<UWorld> LastWorld;
		TWeakObjectPtr<AFGCharacterPlayer> LastPlayer;
		FVector LastFallback = FVector::ZeroVector;
		int32 Extra = 0;
		int32 Items = 0;
		int32 Lost = 0;
		int32 Away = 0;
	};

	int32 FReturnCrates::FailForTest = 0;

	// Counts the inventory's contents and empties it; the solid stacks go into Crates when it is given (fluids cannot go into a crate)
	FTaken TakeAll(UFGInventoryComponent* Inventory, FReturnCrates* Crates, AFGCharacterPlayer* Player, const AActor* From)
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
			if (Crates)
			{
				Crates->Add(From->GetWorld(), Stack, Player, From->GetActorLocation());
			}
		}
		Inventory->Empty();
		return Taken;
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
					Result.Items += TakeAll(Inventory, &Crates, Character.Get(), Machine).Solids;
				}
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
			Crates.Finish(Result);
		}

	private:
		TSubclassOf<UFGRecipe> Recipe;
		TArray<UClass*> ProducedIn;
		FReturnCrates Crates{ TEXT("SetRecipe") };
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
					const FTaken One = TakeAll(Inventory, bToCrate ? &Crates : nullptr, Character.Get(), Actor);
					Taken.Solids += One.Solids;
					Taken.Fluid += One.Fluid;
				}
			}
			// Only buildings something was taken from count, as the player sees it (AA-079)
			if (Taken.Solids > 0 || Taken.Fluid > 0)
			{
				Result.Items += Taken.Solids;
				Result.Fluid += Taken.Fluid;
				++Result.Buildings;
				Result.AddCount(Actor->GetClass());
			}
		}

		virtual void Finish(UWorld* World) override
		{
			FAreaActionsJob::Finish(World);
			Crates.Finish(Result);
		}

	private:
		bool bToCrate;
		FReturnCrates Crates{ TEXT("ClearInventories") };
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
			// The game's own windows change the mode in the player's game as well; a change made here reaches the clients only when the
			// building is woken up for the network (a Freight Platform kept its old mode in a client's game)
			Actor->ForceNetUpdate();
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
			const FVector Fallback = Area.Polygon.IsEmpty() ? FVector::ZeroVector : FVector(Area.Polygon[0], 0.0);
			FReturnCrates Crates(GetName());
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Entry : Refund)
			{
				Result.Items -= Entry.Value;
				const int32 Added = Inventory ? Inventory->AddStack(FInventoryStack(Entry.Value, Entry.Key), true) : 0;
				if (Entry.Value > Added)
				{
					Crates.Add(World, FInventoryStack(Entry.Value - Added, Entry.Key), Player, Fallback);
				}
			}
			Crates.Finish(Result);
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

	// MassDismantle (AA-030) by the game's own rules (P5): every building, lightweight and crate in the area; CanDismantle decides for
	// actors (a lightweight has no actor to ask), an
	// actor's refund is its GetDismantleRefund (build cost and contents, the contents only with No Build Cost), a lightweight's comes from
	// its class. With bToCrate the refund goes into the return crates as each building goes; fluids are lost.
	class FDismantleJob final : public FAreaActionsJob
	{
	public:
		explicit FDismantleJob(bool bInToCrate) : bToCrate(bInToCrate) {}

		virtual const TCHAR* GetName() const override { return TEXT("MassDismantle"); }
		virtual int32 GetItems() const override { return FoundCrates.Num() + Buildings.Num(); }

		virtual bool Step(UWorld* World, double EndTime) override
		{
			if (!bCollected)
			{
				if (!bStarted)
				{
					bStarted = true;
					// Crates are not buildables; the crates this job makes come later and are not among them
					for (TActorIterator<AFGCrate> It(World); It; ++It)
					{
						if (IsValid(*It) && Area.Contains(It->GetActorLocation()))
						{
							FoundCrates.Add(*It);
						}
					}
					const AFGCharacterPlayer* Player = Character.Get();
					const UFGInventoryComponent* Inventory = Player ? Player->GetInventory() : nullptr;
					bNoCost = Inventory && Inventory->GetNoBuildCost();
				}
				if (!Collector.Step(World, Area, Buildings, EndTime))
				{
					return false;
				}
				bCollected = true;
			}
			const int32 Total = FoundCrates.Num() + Buildings.Num();
			while (Next < Total)
			{
				const int32 Index = Next++;
				if (Index < FoundCrates.Num())
				{
					TakeCrate(World, FoundCrates[Index].Get());
				}
				else if (const AreaActionsBuildings::FSnapshot& Building = Buildings[Index - FoundCrates.Num()]; Building.LightweightIndex != INDEX_NONE)
				{
					DismantleLightweight(World, Building);
				}
				else
				{
					DismantleActor(World, Building.Actor.Get());
				}
				if (FPlatformTime::Seconds() > EndTime)
				{
					return false;
				}
			}
			return Sweep(World, EndTime);
		}

		virtual void Finish(UWorld* World) override
		{
			FAreaActionsJob::Finish(World);
			// What the game still refuses after the last sweep stays; each counted once
			TMap<TSubclassOf<UObject>, int32> Left;
			for (const TWeakObjectPtr<AFGBuildable>& Weak : Refused)
			{
				if (Weak.IsValid() && !Weak->GetIsDismantled())
				{
					++Result.Skipped;
					Left.FindOrAdd(Weak->GetClass())++;
				}
			}
			Crates.Finish(Result);
			// For a check by hand (AA-093 pt 4): what was dismantled per class and the refund split into build cost and contents
			const auto Text = [](const auto& Counts)
			{
				TArray<FString> Parts;
				for (const auto& Entry : Counts)
				{
					if (Entry.Value != 0)
					{
						Parts.Add(FString::Printf(TEXT("%s %d"), *GetNameSafe(Entry.Key.Get()), Entry.Value));
					}
				}
				Parts.Sort();
				return FString::Join(Parts, TEXT(", "));
			};
			UE_LOG(LogAreaActionsCpp, Display, TEXT("MassDismantle buildings: %s"), *Text(Classes));
			UE_LOG(LogAreaActionsCpp, Display, TEXT("MassDismantle refund cost: %s"), *Text(RefundCost));
			UE_LOG(LogAreaActionsCpp, Display, TEXT("MassDismantle refund contents: %s"), *Text(RefundContents));
			UE_LOG(LogAreaActionsCpp, Display, TEXT("MassDismantle sweeps %d, left because the game refuses: %s"), Sweeps, *Text(Left));
		}

	private:
		// After the first pass the area is collected again and what is still there is dismantled, until a sweep finds nothing more the
		// game lets go (at most MaxSweeps). The game refuses some buildings while another one hangs on them (a pole under a belt, a
		// part of a bigger building) and a dismantled actor stays about 0.4 s, so each sweep waits SweepWait first
		bool Sweep(UWorld* World, double EndTime)
		{
			static constexpr int32 MaxSweeps = 5;
			static constexpr double SweepWait = 0.6;
			while (true)
			{
				if (!bSweeping)
				{
					if (Sweeps >= MaxSweeps)
					{
						return true;
					}
					if (SweepFrom == 0.0)
					{
						SweepFrom = FPlatformTime::Seconds() + SweepWait;
					}
					if (FPlatformTime::Seconds() < SweepFrom)
					{
						return false;
					}
					bSweeping = true;
					bSwept = false;
					SweepCollector = AreaActionsBuildings::FCollector();
					SweepBuildings.Reset();
					SweepNext = 0;
					SweepBefore = Result.Buildings;
					++Sweeps;
				}
				if (!bSwept)
				{
					if (!SweepCollector.Step(World, Area, SweepBuildings, EndTime))
					{
						return false;
					}
					bSwept = true;
				}
				while (SweepNext < SweepBuildings.Num())
				{
					const AreaActionsBuildings::FSnapshot& Building = SweepBuildings[SweepNext++];
					if (Building.LightweightIndex != INDEX_NONE)
					{
						DismantleLightweight(World, Building);
					}
					else
					{
						DismantleActor(World, Building.Actor.Get());
					}
					if (FPlatformTime::Seconds() > EndTime)
					{
						return false;
					}
				}
				bSweeping = false;
				SweepFrom = 0.0;
				if (Result.Buildings == SweepBefore)
				{
					return true;
				}
			}
		}

		// Solid stacks into a log tally
		static void Tally(AreaActionsBuildings::FItemCounts& Out, const TArray<FInventoryStack>& Stacks, int32 Sign = 1)
		{
			for (const FInventoryStack& Stack : Stacks)
			{
				if (Stack.HasItems() && UFGItemDescriptor::GetForm(Stack.Item.GetItemClass()) == EResourceForm::RF_SOLID)
				{
					Out.FindOrAdd(Stack.Item.GetItemClass()) += Sign * Stack.NumItems;
				}
			}
		}

		void Refund(UWorld* World, const TArray<FInventoryStack>& Stacks, const FVector& From)
		{
			for (const FInventoryStack& Stack : Stacks)
			{
				if (!Stack.HasItems())
				{
					continue;
				}
				if (UFGItemDescriptor::GetForm(Stack.Item.GetItemClass()) != EResourceForm::RF_SOLID)
				{
					Result.Fluid += Stack.NumItems;
					continue;
				}
				// Only what reached a crate was refunded; the rest is in the result's Lost
				Result.Items += Crates.Add(World, Stack, Character.Get(), From);
			}
		}

		void TakeCrate(UWorld* World, AFGCrate* Crate)
		{
			if (!IsValid(Crate))
			{
				return;
			}
			if (bToCrate && Crate->GetInventory())
			{
				TArray<FInventoryStack> Stacks;
				Crate->GetInventory()->GetInventoryStacks(Stacks);
				Tally(RefundContents, Stacks);
				Refund(World, Stacks, Crate->GetActorLocation());
			}
			Classes.FindOrAdd(Crate->GetClass())++;
			Result.AddCount(Crate->GetClass());
			++Result.Buildings;
			Crate->Destroy();
		}

		// The actor and what the game dismantles along with it: GetChildDismantleActors and the wires on its power connections, which the
		// game destroys with it, so they are refunded first
		void DismantleActor(UWorld* World, AFGBuildable* Actor)
		{
			if (!IsValid(Actor) || Actor->GetIsDismantled() || Done.Contains(Actor))
			{
				return;
			}
			if (!IFGDismantleInterface::Execute_CanDismantle(Actor))
			{
				// Asked again by the next sweep; Finish counts what is still refused
				Refused.Add(Actor);
				return;
			}
			TArray<AActor*> Children;
			IFGDismantleInterface::Execute_GetChildDismantleActors(Actor, Children);
			TInlineComponentArray<UFGCircuitConnectionComponent*> Connections(Actor);
			for (const UFGCircuitConnectionComponent* Connection : Connections)
			{
				TArray<AFGBuildableWire*> Wires;
				Connection->GetWires(Wires);
				for (AFGBuildableWire* Wire : Wires)
				{
					if (IsValid(Wire) && !Wire->GetIsDismantled() && !Done.Contains(Wire) && IFGDismantleInterface::Execute_CanDismantle(Wire))
					{
						DismantleOne(World, Wire);
					}
				}
			}
			DismantleOne(World, Actor);
			for (AActor* Child : Children)
			{
				const AFGBuildable* Buildable = Cast<AFGBuildable>(Child);
				if (IsValid(Child) && Child->Implements<UFGDismantleInterface>() && !Done.Contains(Child) && !(Buildable && Buildable->GetIsDismantled()))
				{
					if (IFGDismantleInterface::Execute_CanDismantle(Child))
					{
						DismantleOne(World, Child);
					}
					else if (AFGBuildable* Refuses = Cast<AFGBuildable>(Child))
					{
						Refused.Add(Refuses);
					}
					else
					{
						++Result.Skipped;
					}
				}
			}
		}

		void DismantleOne(UWorld* World, AActor* Actor)
		{
			Done.Add(Actor);
			if (bToCrate)
			{
				TArray<FInventoryStack> Stacks, Contents;
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Stacks, bNoCost);
				IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Contents, true);
				Tally(RefundCost, Stacks);
				Tally(RefundCost, Contents, -1);
				Tally(RefundContents, Contents);
				Refund(World, Stacks, Actor->GetActorLocation());
			}
			Classes.FindOrAdd(Actor->GetClass())++;
			Result.AddCount(Actor->GetClass());
			++Result.Buildings;
			IFGDismantleInterface::Execute_Dismantle(Actor);
		}

		void DismantleLightweight(UWorld* World, const AreaActionsBuildings::FSnapshot& Building)
		{
			AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
			const FRuntimeBuildableInstanceData* Data = Lightweights ? Lightweights->GetRuntimeDataForBuildableClassAndIndex(Building.Class, Building.LightweightIndex) : nullptr;
			// The slot may have been freed and filled with another building since the collection
			if (!Data || !Data->IsValid() || !Data->Transform.Equals(Building.Transform, 0.01))
			{
				return;
			}
			if (bToCrate && !bNoCost)
			{
				TArray<FInventoryStack> Stacks;
				Building.Class->GetDefaultObject<AFGBuildable>()->GetLightweightBuildableDismantleRefundReturns(World, AreaActionsBuildings::GetRecipe(World, Building), Data->TypeSpecificData, Stacks);
				Tally(RefundCost, Stacks);
				Refund(World, Stacks, Building.Transform.GetLocation());
			}
			Classes.FindOrAdd(Building.Class)++;
			Result.AddCount(Building.Class);
			++Result.Buildings;
			Lightweights->RemoveByInstanceIndex(Building.Class, Building.LightweightIndex);
		}

		bool bToCrate;
		bool bNoCost = false;
		bool bStarted = false;
		AreaActionsBuildings::FCollector Collector;
		TArray<AreaActionsBuildings::FSnapshot> Buildings;
		TArray<TWeakObjectPtr<AFGCrate>> FoundCrates;
		TSet<TWeakObjectPtr<AActor>> Done;
		int32 Next = 0;
		TSet<TWeakObjectPtr<AFGBuildable>> Refused;
		// The sweeps after the first pass
		AreaActionsBuildings::FCollector SweepCollector;
		TArray<AreaActionsBuildings::FSnapshot> SweepBuildings;
		int32 SweepNext = 0;
		int32 SweepBefore = 0;
		int32 Sweeps = 0;
		double SweepFrom = 0.0;
		bool bSweeping = false;
		bool bSwept = false;
		// The log of the refund (Finish)
		TMap<TSubclassOf<UObject>, int32> Classes;
		AreaActionsBuildings::FItemCounts RefundCost;
		AreaActionsBuildings::FItemCounts RefundContents;
		FReturnCrates Crates{ TEXT("MassDismantle") };
	};
}

TUniquePtr<FAreaActionsJob> AreaActionsJobs::MakeDismantle(bool bToCrate)
{
	return MakeUnique<FDismantleJob>(bToCrate);
}

void AreaActionsJobs::SetCratesFailForTest(int32 Mode)
{
	FReturnCrates::FailForTest = Mode;
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

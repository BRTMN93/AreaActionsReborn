#pragma once

#include "CoreMinimal.h"
#include "AreaActionsTypes.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "FGDynamicStruct.h"
#include "FGFactoryColoringTypes.h"

class AFGBuildable;
class UFGItemDescriptor;
class UFGRecipe;
class UStaticMesh;

// Buildings in an area, actors and lightweights alike, and the copy engine Copy and Fill share. The client runs the same code for the
// preview as the server for the real copies, so both turn the player's numbers into the same transforms.
namespace AreaActionsBuildings
{
	// One building as it stood when it was found
	struct FSnapshot
	{
		TSubclassOf<AFGBuildable> Class;
		FTransform Transform;
		TSubclassOf<UFGRecipe> BuiltWith;
		FFactoryCustomizationData Customization;
		// What a lightweight needs beyond its transform (a beam's length)
		FFGDynamicStruct TypeData;
		// The actor, or none for a lightweight (then LightweightIndex in the subsystem's array of the class)
		TWeakObjectPtr<AFGBuildable> Actor;
		int32 LightweightIndex = INDEX_NONE;
		// A class the copy skips (belts, wires, ...): it counts only for the middle of the area, as in the original
		bool bIgnored = false;
		// The state of an actor the copy carries over
		TSubclassOf<UFGRecipe> MachineRecipe;
		TArray<FSplitterSortRule> SortRules;
	};

	// The area's own axes: its reference point (the middle of the buildings' places, at their lowest corner), its most common building
	// rotation (modulo 90 degrees, as the original) and, for Fill, the pattern's size rounded to half metres
	struct FFrame
	{
		FVector Position = FVector::ZeroVector;
		double Yaw = 0.0;
		FVector PatternSize = FVector::ZeroVector;
		double MaxZ = 0.0;
	};

	struct FPlanned
	{
		int32 Source = INDEX_NONE;
		FTransform Transform;
	};

	using FItemCounts = TMap<TSubclassOf<UFGItemDescriptor>, int32>;

	// Belts, wires, tracks, switches, poles, pipes, hypertubes and their parts, extractors: the original's list with the successors of
	// the classes the game removed
	bool ShouldIgnore(const UClass* Class);
	// Classes the game keeps as lightweight instances, not actors
	bool IsLightweightClass(const UClass* Class);
	// The building's box in its own space: its clearance box or, without one, the bounds of its meshes; false when there is none
	bool GetLocalBox(const FSnapshot& Building, FBox& Out);
	// The same for a lightweight found by a hit (class and index in the subsystem)
	bool GetLightweight(UWorld* World, UClass* Class, int32 Index, FSnapshot& Out);
	// The building a trace hit: an actor, or a lightweight through its instance handle; false for anything else
	bool FromHit(UWorld* World, const FHitResult& Hit, FSnapshot& Out);
	// The 8 corners of the building's box in the world, the top face first and going round (as the tool's GetActorCorners); empty without a box
	void GetCorners(const FSnapshot& Building, TArray<FVector>& Out);

	// Collects the buildings whose place is in the area, a part per call; true when done
	struct FCollector
	{
		int32 ActorCursor = 0;
		TArray<TSubclassOf<AFGBuildable>> Classes;
		int32 ClassCursor = 0;
		int32 IndexCursor = 0;
		bool bActorsDone = false;
		bool bClassesRead = false;

		bool Step(UWorld* World, const FAreaActionsArea& Area, TArray<FSnapshot>& Out, double EndTime);
	};
	void CollectAll(UWorld* World, const FAreaActionsArea& Area, TArray<FSnapshot>& Out);

	FFrame ComputeFrame(const TArray<FSnapshot>& Buildings);
	// The copies the placement asks for, in order; stops after Limit (then Out has Limit + 1 entries)
	void Plan(const TArray<FSnapshot>& Buildings, const FFrame& Frame, const FAreaActionsPlacement& Placement, int64 Limit, TArray<FPlanned>& Out);
	// Fill: the steps along one axis, the pattern's own (0) included, as the original counted them
	void FillSteps(int32 Amount, TArray<int32>& Out);

	// The buildings already standing, by class and place, so a copy onto an equal building is skipped (the original's AreActorsSame)
	class FExisting
	{
	public:
		void Build(UWorld* World, const TSet<UClass*>& Classes);
		bool Contains(const UClass* Class, const FTransform& Transform) const;
		void Add(const UClass* Class, const FTransform& Transform);

	private:
		TMap<const UClass*, TMultiMap<FIntVector, FTransform>> Places;
	};

	// The items one copy of the building costs: its recipe's ingredients, times the length for beams
	void AddCost(UWorld* World, const FSnapshot& Building, FItemCounts& InOut);
	// Builds one copy on the server; false when the game refused it
	bool Spawn(UWorld* World, const FSnapshot& Building, const FTransform& Transform);
	// The meshes the building shows, relative to its transform, for the preview
	void GetMeshes(UWorld* World, const FSnapshot& Building, TArray<TPair<UStaticMesh*, FTransform>>& Out);
}

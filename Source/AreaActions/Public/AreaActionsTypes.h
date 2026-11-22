#pragma once

#include "CoreMinimal.h"
#include "AreaActionsTypes.generated.h"

// The selected area: a polygon in XY and a height range. Comes from the client, so the server checks it before any use.
USTRUCT(BlueprintType)
struct AREAACTIONS_API FAreaActionsArea
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	TArray<FVector2D> Polygon;

	// No limit is sent as -/+UE_BIG_NUMBER
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	double MinZ = -UE_BIG_NUMBER;

	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	double MaxZ = UE_BIG_NUMBER;

	// The original's rule: the point is in the polygon (ray casting, edges as the original counted them) and MinZ <= Z <= MaxZ
	bool Contains(const FVector& Point) const;
};

UENUM(BlueprintType)
enum class EAreaActionsResultCode : uint8
{
	Ok,
	// The server refused the request (bad data, no tool in hand); the world did not change
	Rejected,
	// The player already has an action running
	Busy,
	// Nothing in the area the action works on
	NothingInArea,
	// Every building the action works on already has the asked state (SetRecipe, decision D12)
	AlreadySet,
	// Copy and Fill: the player has not got the build cost; Missing lists what lacks, nothing was built or taken
	MissingItems,
	// Copy and Fill: more buildings than AreaActions.MaxSpawnPerRequest; Buildings is the count asked, Items the limit
	TooMany,
};

USTRUCT(BlueprintType)
struct AREAACTIONS_API FAreaActionsClassCount
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	TObjectPtr<UClass> Class = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	int32 Count = 0;
};

// The server's answer. Kept small: an RPC carries at most 64 KB, so the counts per class stop at MaxCounts.
USTRUCT(BlueprintType)
struct AREAACTIONS_API FAreaActionsResult
{
	GENERATED_BODY()

	static constexpr int32 MaxCounts = 32;

	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	EAreaActionsResultCode Code = EAreaActionsResultCode::Ok;

	// Buildings the action changed
	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	int32 Buildings = 0;

	// Solid items the action moved or removed
	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	int32 Items = 0;

	// Fluids the action removed, in the game's inventory units (1000 per m3)
	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	int32 Fluid = 0;

	// Changed buildings per class
	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	TArray<FAreaActionsClassCount> Counts;

	// Items missing for the build cost (Copy, Fill), at most MaxCounts kinds
	UPROPERTY(BlueprintReadOnly, Category = "Area Actions")
	TArray<FAreaActionsClassCount> Missing;

	void AddCount(UClass* Class);
};

// Where Copy and Fill put the copies: the player's numbers, not transforms. Both sides turn them into transforms with the same code
// (AreaActionsBuildings), from the buildings each side finds in the area; the server checks every number before use.
USTRUCT(BlueprintType)
struct AREAACTIONS_API FAreaActionsPlacement
{
	GENERATED_BODY()

	// Fill (a grid of copies) instead of Copy (one copy moved and turned)
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	bool bFill = false;

	// Copy: the move in cm, along the area's own axes (its most common building rotation)
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	FVector Delta = FVector::ZeroVector;

	// Copy: the turn around Z in degrees, round the rotation centre
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	double Yaw = 0.0;

	// Copy: the rotation centre in cm from the area's reference point (the middle of the buildings, at their lowest point), world axes
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	FVector RotationCenter = FVector::ZeroVector;

	// Fill: the gap between copies in cm, per axis of the area
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	FVector Border = FVector::ZeroVector;

	// Fill: copies per axis, the pattern itself included; negative fills the other way
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	FIntVector Amount = FIntVector(1, 1, 1);

	// Fill: the rise in cm per step along X and along Y
	UPROPERTY(BlueprintReadWrite, Category = "Area Actions")
	FVector2D Ramp = FVector2D::ZeroVector;
};

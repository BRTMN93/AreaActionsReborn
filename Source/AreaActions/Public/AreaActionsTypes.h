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

	void AddCount(UClass* Class);
};

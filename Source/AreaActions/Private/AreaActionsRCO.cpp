#include "AreaActionsRCO.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsJobs.h"
#include "AreaActionsLog.h"
#include "AreaActionsServerSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGPlayerController.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

static TAutoConsoleVariable<int32> CVarMaxCorners(TEXT("AreaActions.MaxCorners"), 256, TEXT("Most corners an area may have"));
static TAutoConsoleVariable<float> CVarMaxDistance(TEXT("AreaActions.MaxDistance"), 0.0f,
	TEXT("Farthest an area's nearest corner may be from the player (cm); 0 = no limit, as the original's \"whole map\" needs"));

// The original's ActionsSettings.MaxContainerDistance (10 km), the end of the window's slider
static TAutoConsoleVariable<float> CVarMaxContainerDistance(TEXT("AreaActions.MaxContainerDistance"), 1000000.0f,
	TEXT("Farthest a storage container may be from the player to pay a Copy or Fill (cm)"));

namespace
{
	// Corners farther than this from the map's origin are not real input (the playable map ends well within it)
	constexpr double MaxCoordinate = 1.0e6;
	// Fill: copies per axis and gaps, ramps (cm) beyond these are not real input
	constexpr int32 MaxAmount = 1000000;
	constexpr double MaxGap = 1.0e5;

	bool IsBounded(const FVector& Value, double Limit)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z) && Value.GetAbsMax() <= Limit;
	}
}

int32 UAreaActionsRCO::RejectCount = 0;

void UAreaActionsRCO::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UAreaActionsRCO, bDummy);
}

UAreaActionsRCO* UAreaActionsRCO::Of(APlayerController* Controller)
{
	AFGPlayerController* Player = Cast<AFGPlayerController>(Controller);
	return Player ? Player->GetRemoteCallObjectOfClass<UAreaActionsRCO>() : nullptr;
}

void UAreaActionsRCO::ServerPing_Implementation(int32 Token)
{
	UE_LOG(LogAreaActionsCpp, Display, TEXT("ServerPing %d"), Token);
	ClientPong(Token);
}

void UAreaActionsRCO::ClientPong_Implementation(int32 Token)
{
	UE_LOG(LogAreaActionsCpp, Display, TEXT("ClientPong %d"), Token);
	OnPong.Broadcast(Token);
}

void UAreaActionsRCO::Reject(int32 RequestId, const TCHAR* Action, EAreaActionsResultCode Code, const FString& Why)
{
	const AFGPlayerController* Player = GetOwnerPlayerController();
	const APlayerState* State = Player ? Player->PlayerState : nullptr;
	UE_LOG(LogAreaActionsCpp, Warning, TEXT("reject %s %s: %s"), State ? *State->GetPlayerName() : TEXT("<no player>"), Action, *Why);
	++RejectCount;
	FAreaActionsResult Result;
	Result.Code = Code;
	ClientResult(RequestId, Result);
}

bool UAreaActionsRCO::ValidateRequest(int32 RequestId, FAreaActionsArea& Area, const TCHAR* Action)
{
	// 1. The player: alive, with the tool in hand (the tool is unlocked and theirs)
	AFGPlayerController* Player = GetOwnerPlayerController();
	AFGCharacterPlayer* Character = Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
	if (!Character || !Character->IsAliveAndWell())
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Rejected, TEXT("no living character"));
		return false;
	}
	if (!Character->GetActiveEquipments().ContainsByPredicate([](const AFGEquipment* Equipment) { return Cast<AAreaActionsEquipment>(Equipment) != nullptr; }))
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Rejected, TEXT("the tool is not in hand"));
		return false;
	}
	// 2. The area
	const int32 MaxCorners = FMath::Max(3, CVarMaxCorners.GetValueOnGameThread());
	if (Area.Polygon.Num() < 3 || Area.Polygon.Num() > MaxCorners)
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Rejected, FString::Printf(TEXT("%d corners, allowed 3 to %d"), Area.Polygon.Num(), MaxCorners));
		return false;
	}
	for (const FVector2D& Corner : Area.Polygon)
	{
		if (!FMath::IsFinite(Corner.X) || !FMath::IsFinite(Corner.Y) || FMath::Abs(Corner.X) > MaxCoordinate || FMath::Abs(Corner.Y) > MaxCoordinate)
		{
			Reject(RequestId, Action, EAreaActionsResultCode::Rejected, FString::Printf(TEXT("corner %s outside the map"), *Corner.ToString()));
			return false;
		}
	}
	if (FMath::IsNaN(Area.MinZ) || FMath::IsNaN(Area.MaxZ))
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Rejected, TEXT("height limit is NaN"));
		return false;
	}
	Area.MinZ = FMath::Clamp(Area.MinZ, -UE_BIG_NUMBER, UE_BIG_NUMBER);
	Area.MaxZ = FMath::Clamp(Area.MaxZ, -UE_BIG_NUMBER, UE_BIG_NUMBER);
	if (Area.MinZ > Area.MaxZ)
	{
		Swap(Area.MinZ, Area.MaxZ);
	}
	// 3. Reach, when the server's admin sets one
	if (const double MaxDistance = CVarMaxDistance.GetValueOnGameThread(); MaxDistance > 0.0)
	{
		const FVector2D At(Character->GetActorLocation());
		double Nearest = TNumericLimits<double>::Max();
		for (const FVector2D& Corner : Area.Polygon)
		{
			Nearest = FMath::Min(Nearest, FVector2D::Distance(At, Corner));
		}
		if (Nearest > MaxDistance)
		{
			Reject(RequestId, Action, EAreaActionsResultCode::Rejected, FString::Printf(TEXT("area %.0f cm away, limit %.0f"), Nearest, MaxDistance));
			return false;
		}
	}
	// 4. One job per player
	AAreaActionsServerSubsystem* Server = AAreaActionsServerSubsystem::Get(GetWorld());
	if (!Server)
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Rejected, TEXT("no server subsystem"));
		return false;
	}
	if (Server->HasJobFor(this))
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Busy, TEXT("another action is running"));
		return false;
	}
	return true;
}

void UAreaActionsRCO::Start(int32 RequestId, const FAreaActionsArea& Area, TUniquePtr<FAreaActionsJob> Job)
{
	Job->RequestId = RequestId;
	Job->Area = Area;
	Job->Rco = this;
	const AFGPlayerController* Player = GetOwnerPlayerController();
	Job->Character = Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("AAJOB %s queued, request %d, %d corners, Z %.0f..%.0f"), Job->GetName(), RequestId, Area.Polygon.Num(), Area.MinZ, Area.MaxZ);
	AAreaActionsServerSubsystem::Get(GetWorld())->Enqueue(MoveTemp(Job));
}

void UAreaActionsRCO::ServerSetRecipe_Implementation(int32 RequestId, const FAreaActionsArea& InArea, UClass* Recipe)
{
	FAreaActionsArea Area = InArea;
	if (!ValidateRequest(RequestId, Area, TEXT("SetRecipe")))
	{
		return;
	}
	// The class comes from the network as any UClass
	const AFGRecipeManager* Recipes = AFGRecipeManager::Get(GetWorld());
	if (!Recipe || !Recipe->IsChildOf(UFGRecipe::StaticClass()) || !Recipes || !Recipes->IsRecipeAvailable(Recipe))
	{
		Reject(RequestId, TEXT("SetRecipe"), EAreaActionsResultCode::Rejected, FString::Printf(TEXT("%s is not an available recipe"), *GetNameSafe(Recipe)));
		return;
	}
	Start(RequestId, Area, AreaActionsJobs::MakeSetRecipe(Recipe));
}

void UAreaActionsRCO::ServerClearInventories_Implementation(int32 RequestId, const FAreaActionsArea& InArea, bool bToCrate)
{
	FAreaActionsArea Area = InArea;
	if (ValidateRequest(RequestId, Area, TEXT("ClearInventories")))
	{
		Start(RequestId, Area, AreaActionsJobs::MakeClearInventories(bToCrate));
	}
}

void UAreaActionsRCO::ServerSetStationMode_Implementation(int32 RequestId, const FAreaActionsArea& InArea, bool bLoad)
{
	FAreaActionsArea Area = InArea;
	if (ValidateRequest(RequestId, Area, TEXT("SetStationMode")))
	{
		Start(RequestId, Area, AreaActionsJobs::MakeSetStationMode(bLoad));
	}
}

void UAreaActionsRCO::ServerPlaceCopies_Implementation(int32 RequestId, const FAreaActionsArea& InArea, const FAreaActionsPlacement& InPlacement, bool bUseBuildCost,
	float ContainerDistance)
{
	FAreaActionsPlacement Placement = InPlacement;
	const TCHAR* Action = Placement.bFill ? TEXT("Fill") : TEXT("Copy");
	FAreaActionsArea Area = InArea;
	if (!ValidateRequest(RequestId, Area, Action))
	{
		return;
	}
	const bool bNumbers = IsBounded(Placement.Delta, 2.0 * MaxCoordinate) && IsBounded(Placement.RotationCenter, 2.0 * MaxCoordinate) && FMath::IsFinite(Placement.Yaw)
		&& IsBounded(Placement.Border, MaxGap) && IsBounded(FVector(Placement.Ramp, 0.0), MaxGap)
		&& FMath::Abs(Placement.Amount.X) <= MaxAmount && FMath::Abs(Placement.Amount.Y) <= MaxAmount && FMath::Abs(Placement.Amount.Z) <= MaxAmount;
	if (!bNumbers)
	{
		Reject(RequestId, Action, EAreaActionsResultCode::Rejected, FString::Printf(TEXT("placement out of range: delta %s, centre %s, yaw %f, gap %s, amount %s, ramp %s"),
			*Placement.Delta.ToString(), *Placement.RotationCenter.ToString(), Placement.Yaw, *Placement.Border.ToString(), *Placement.Amount.ToString(), *Placement.Ramp.ToString()));
		return;
	}
	Placement.Yaw = FMath::Fmod(Placement.Yaw, 360.0);
	const double Distance = FMath::IsFinite(ContainerDistance) ? FMath::Clamp<double>(ContainerDistance, 0.0, FMath::Max(0.0f, CVarMaxContainerDistance.GetValueOnGameThread())) : 0.0;
	Start(RequestId, Area, AreaActionsJobs::MakePlaceCopies(Placement, bUseBuildCost, Distance));
}

void UAreaActionsRCO::ClientResult_Implementation(int32 RequestId, const FAreaActionsResult& Result)
{
	UE_LOG(LogAreaActionsCpp, Display, TEXT("ClientResult %d: code %d, buildings %d, items %d"), RequestId, static_cast<int32>(Result.Code), Result.Buildings, Result.Items);
	OnResult.Broadcast(RequestId, Result);
}

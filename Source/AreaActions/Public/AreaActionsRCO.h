#pragma once

#include "CoreMinimal.h"
#include "AreaActionsTypes.h"
#include "FGRemoteCallObject.h"
#include "AreaActionsRCO.generated.h"

class APlayerController;
class FAreaActionsJob;

// One for every player. Every change of the world goes through it, also in a game for one player; the server checks all it gets.
// Server calls carry the area and the action's parameters only; the server answers each with ClientResult and the same RequestId.
UCLASS()
class AREAACTIONS_API UAreaActionsRCO : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// The player's own, or null
	static UAreaActionsRCO* Of(APlayerController* Controller);

	// Round trip check: the server answers with the same token
	UFUNCTION(Server, Reliable)
	void ServerPing(int32 Token);
	UFUNCTION(Client, Reliable)
	void ClientPong(int32 Token);
	TMulticastDelegate<void(int32 Token)> OnPong;

	// Recipe is any class the client sends; the server accepts recipes only
	UFUNCTION(Server, Reliable)
	void ServerSetRecipe(int32 RequestId, const FAreaActionsArea& Area, UClass* Recipe);
	UFUNCTION(Server, Reliable)
	void ServerClearInventories(int32 RequestId, const FAreaActionsArea& Area, bool bToCrate);
	UFUNCTION(Server, Reliable)
	void ServerSetStationMode(int32 RequestId, const FAreaActionsArea& Area, bool bLoad);

	UFUNCTION(Client, Reliable)
	void ClientResult(int32 RequestId, const FAreaActionsResult& Result);
	TMulticastDelegate<void(int32 RequestId, const FAreaActionsResult& Result)> OnResult;

	// Requests refused in this process (the test mode checks them)
	static int32 RejectCount;

private:
	// The checks every request passes (player, tool, area, one job at a time); on failure the client gets Rejected or Busy
	bool ValidateRequest(int32 RequestId, FAreaActionsArea& Area, const TCHAR* Action);
	void Reject(int32 RequestId, const TCHAR* Action, EAreaActionsResultCode Code, const FString& Why);
	void Start(int32 RequestId, const FAreaActionsArea& Area, TUniquePtr<FAreaActionsJob> Job);

	// An RCO needs one replicated property to replicate at all
	UPROPERTY(Replicated)
	bool bDummy = true;
};

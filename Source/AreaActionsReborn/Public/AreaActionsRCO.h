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
class AREAACTIONSREBORN_API UAreaActionsRCO : public UFGRemoteCallObject
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
	// Server side: a player's ping arrived (the test mode's two game instances talk through the ping and its answer)
	static TMulticastDelegate<void(UAreaActionsRCO* From, int32 Token)> OnPing;

	// Recipe is any class the client sends; the server accepts recipes only
	UFUNCTION(Server, Reliable)
	void ServerSetRecipe(int32 RequestId, const FAreaActionsArea& Area, UClass* Recipe);
	UFUNCTION(Server, Reliable)
	void ServerClearInventories(int32 RequestId, const FAreaActionsArea& Area, bool bToCrate);
	UFUNCTION(Server, Reliable)
	void ServerSetStationMode(int32 RequestId, const FAreaActionsArea& Area, bool bLoad);
	// Copy and Fill; the server finds the buildings in the area itself and turns the numbers into places with the client's code.
	// ContainerDistance (cm) is capped by AreaActions.MaxContainerDistance.
	UFUNCTION(Server, Reliable)
	void ServerPlaceCopies(int32 RequestId, const FAreaActionsArea& Area, const FAreaActionsPlacement& Placement, bool bUseBuildCost, float ContainerDistance);

	// MassDismantle; a refund goes into return crates at the player. Refund is any byte the client sends; the server checks it and reads
	// "No Build Cost" itself
	UFUNCTION(Server, Reliable)
	void ServerDismantle(int32 RequestId, const FAreaActionsArea& Area, EAreaActionsDismantleRefund Refund);

	// Flying on or off for the player's character (AA-060); on only with the tool in hand. The client switches its own character at once,
	// the server's copy here, so the server takes the moves the client sends
	UFUNCTION(Server, Reliable)
	void ServerSetFlying(bool bFly);
	// A refused ServerSetFlying sends the server's state back, so a client that switched itself already walks again (AA-101)
	UFUNCTION(Client, Reliable)
	void ClientSetFlying(bool bFly);

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

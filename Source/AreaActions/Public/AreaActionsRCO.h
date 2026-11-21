#pragma once

#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "AreaActionsRCO.generated.h"

class APlayerController;

// One for every player. Every change of the world goes through it, also in a game for one player; the server checks all it gets.
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

private:
	// An RCO needs one replicated property to replicate at all
	UPROPERTY(Replicated)
	bool bDummy = true;
};

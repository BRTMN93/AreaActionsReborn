#include "AreaActionsRCO.h"
#include "AreaActionsLog.h"
#include "FGPlayerController.h"
#include "Net/UnrealNetwork.h"

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

#include "AreaActionsServerSubsystem.h"
#include "AreaActionsLog.h"
#include "Subsystem/SubsystemActorManager.h"

AAreaActionsServerSubsystem::AAreaActionsServerSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
}

AAreaActionsServerSubsystem* AAreaActionsServerSubsystem::Get(UWorld* World)
{
	USubsystemActorManager* Manager = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
	return Manager ? Manager->GetSubsystemActor<AAreaActionsServerSubsystem>() : nullptr;
}

void AAreaActionsServerSubsystem::Init()
{
	Super::Init();
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Server subsystem Init"));
}

#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "AreaActionsServerSubsystem.generated.h"

// The server's side of the actions: the job queue with a time budget comes with the first action (AA-010)
UCLASS()
class AREAACTIONS_API AAreaActionsServerSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AAreaActionsServerSubsystem();
	static AAreaActionsServerSubsystem* Get(UWorld* World);

protected:
	virtual void Init() override;
};

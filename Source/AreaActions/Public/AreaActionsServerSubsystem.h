#pragma once

#include "CoreMinimal.h"
#include "AreaActionsTypes.h"
#include "Subsystem/ModSubsystem.h"
#include "AreaActionsServerSubsystem.generated.h"

class AFGCharacterPlayer;
class UAreaActionsRCO;

// One action request on the server, run in slices: first it collects the buildings in the area, then it changes them one by one.
// A job keeps weak pointers only and checks every building right before it uses it.
class AREAACTIONS_API FAreaActionsJob
{
public:
	virtual ~FAreaActionsJob() = default;

	// Name in the AAJOB line
	virtual const TCHAR* GetName() const = 0;
	// Cheap class test; the area test comes after it
	virtual bool Matches(const AActor* Actor) const = 0;
	// Crates (not buildables) are collected as well
	virtual bool WantsCrates() const { return false; }
	virtual void Apply(AActor* Actor) = 0;
	// After the last building; the result's code is decided here
	virtual void Finish(UWorld* World);

	int32 RequestId = 0;
	FAreaActionsArea Area;
	TWeakObjectPtr<UAreaActionsRCO> Rco;
	TWeakObjectPtr<AFGCharacterPlayer> Character;
	FAreaActionsResult Result;

	// Runner state
	int32 Cursor = 0;
	bool bCollected = false;
	TArray<TWeakObjectPtr<AActor>> Found;
	int32 ApplyIndex = 0;
	int32 Frames = 0;
	double MaxSliceMs = 0.0;
};

// The server's side of the actions: the job queue, run in Tick within AreaActions.ServerBudgetMs per frame for all players together
UCLASS()
class AREAACTIONS_API AAreaActionsServerSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AAreaActionsServerSubsystem();
	static AAreaActionsServerSubsystem* Get(UWorld* World);

	bool HasJobFor(const UAreaActionsRCO* Rco) const;
	void Enqueue(TUniquePtr<FAreaActionsJob> Job);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void Init() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	// Runs the job until the time is up; true when it is done
	bool Step(FAreaActionsJob& Job, double EndTime);

	TArray<TUniquePtr<FAreaActionsJob>> Jobs;
};

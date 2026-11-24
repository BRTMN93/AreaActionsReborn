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
	// Runs the job until EndTime; true when it is done. By default it collects the actors Matches takes, then calls Apply for each
	virtual bool Step(UWorld* World, double EndTime);
	// Cheap class test; the area test comes after it
	virtual bool Matches(const AActor* Actor) const { return false; }
	// Crates (not buildables) are collected as well
	virtual bool WantsCrates() const { return false; }
	virtual void Apply(AActor* Actor) {}
	// After the last building; the result's code is decided here
	virtual void Finish(UWorld* World);
	// The count in the AAJOB line
	virtual int32 GetItems() const { return Found.Num(); }

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
	// Jobs waiting or running (the test mode waits for none)
	int32 NumJobs() const { return Jobs.Num(); }

	virtual void Tick(float DeltaSeconds) override;

	// Jobs finished in this process, one per AAJOB result line (the test mode counts them)
	static int32 FinishedJobs;

protected:
	virtual void Init() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	TArray<TUniquePtr<FAreaActionsJob>> Jobs;
};

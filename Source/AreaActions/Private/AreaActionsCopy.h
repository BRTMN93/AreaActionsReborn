#pragma once

#include "CoreMinimal.h"
#include "AreaActionBase.h"
#include "AreaActionsBuildings.h"
#include "GameFramework/Actor.h"
#include "AreaActionsCopy.generated.h"

class AFGHologram;
class UInstancedStaticMeshComponent;
class UStaticMesh;

// The preview of Copy and Fill on the player's machine: ghosts, not buildings. The first AreaActions.PreviewHolograms copies are the game's
// own holograms (as the build gun shows them, decision D20); the rest, and buildings whose hologram cannot show their data (a beam's
// length) or has children, are instanced meshes in the game's valid placement material. Nothing of it is
// saved or replicated.
UCLASS(NotPlaceable, Transient)
class AREAACTIONS_API AAreaActionsPreview : public AActor
{
	GENERATED_BODY()

public:
	AAreaActionsPreview();

	void Show(const TArray<AreaActionsBuildings::FSnapshot>& Buildings, const TArray<AreaActionsBuildings::FPlanned>& Copies);
	void Clear();
	int32 GetGhosts() const { return Ghosts; }
	int32 GetHolograms() const { return Holograms.Num(); }

	// Ghosts of all previews in this process (the test mode counts them)
	static int32 LiveGhosts;
	// The custom depth stencil of the game's valid hologram, which gives it its colour
	static uint8 GetValidStencil();

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY()
	TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>> Meshes;
	UPROPERTY()
	TArray<TObjectPtr<AFGHologram>> Holograms;
	int32 Ghosts = 0;
};

// Copy and Fill on the player's machine: the window's numbers become a placement; the preview and the server turn it into the same places
// from the buildings each finds in the area (AreaActionsBuildings)
UCLASS(Abstract)
class AREAACTIONS_API UAreaActionPlaceCopies : public UAreaAction
{
	GENERATED_BODY()

public:
	virtual void Start() override;
	virtual void OnResult(const FAreaActionsResult& Result) override;
	virtual void End() override;

	UFUNCTION()
	void Canceled();

	// For the test mode
	UUserWidget* GetWindow() const { return Window; }
	const AAreaActionsPreview* GetPreview() const { return Preview; }
	const AreaActionsBuildings::FFrame& GetFrame() const { return Frame; }
	// The copies the preview shows for this placement (the same plan as the server's, also without the copies onto equal buildings)
	void PlanCopies(const FAreaActionsPlacement& Placement, TArray<AreaActionsBuildings::FPlanned>& Out) const;
	void ShowPreview(const FAreaActionsPlacement& Placement);
	void Send(const FAreaActionsPlacement& Placement, bool bUseCost);

protected:
	virtual bool IsFill() const { return false; }
	// The window's own events; true when they are bound
	virtual bool BindWindow() { return false; }
	// The window again after a pick in the world, with the mouse for the window
	void ShowWindow();
	bool GetWindowFlag(const TCHAR* Name) const;
	void SetWindowFlag(const TCHAR* Name, bool bValue);
	// Calls a function of the window by name with parameters set by name; false when it has not got them
	bool CallWindow(FName Function, TFunctionRef<bool(UFunction*, uint8*)> SetParams, TFunctionRef<void(UFunction*, const uint8*)> ReadParams);
	// The tool's ActionsSettings subsystem actor, which keeps "Use build cost" and the container distance between actions
	AActor* GetSettings() const;
	// The player's settings file into that actor (Send writes them back)
	void LoadSettings() const;

	TArray<AreaActionsBuildings::FSnapshot> Buildings;
	AreaActionsBuildings::FFrame Frame;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> Window;
	UPROPERTY(Transient)
	TObjectPtr<AAreaActionsPreview> Preview;
};

UCLASS()
class AREAACTIONS_API UAreaActionCopy : public UAreaActionPlaceCopies
{
	GENERATED_BODY()

public:
	virtual void OnClick(bool bRight) override;
	virtual void End() override;

	// Widget_Copy's OkClicked(Delta, DeltaRotation, UseCost) and Preview(Delta, DeltaRotation), Delta in cm
	UFUNCTION()
	void Ok(FVector Delta, FRotator DeltaRotation, bool UseCost);
	UFUNCTION()
	void PreviewClicked(FVector Delta, FRotator DeltaRotation);

	FAreaActionsPlacement MakePlacement(const FVector& Delta, const FRotator& DeltaRotation) const;

protected:
	virtual bool BindWindow() override;

private:
	// The rotation centre from the reference point, picked on a building of the area
	FVector RotationCenter = FVector::ZeroVector;
	UPROPERTY(Transient)
	TObjectPtr<AActor> RotationMarker;
	bool bPreviewing = false;
	FVector PreviewDelta = FVector::ZeroVector;
	FRotator PreviewRotation = FRotator::ZeroRotator;
};

UCLASS()
class AREAACTIONS_API UAreaActionFill : public UAreaActionPlaceCopies
{
	GENERATED_BODY()

public:
	virtual void Start() override;
	virtual void OnClick(bool bRight) override;

	// Widget_Fill's OkClicked(Border, Amount, Ramp, UseCost), Preview(Border, Amount, Ramp) and UpdateIndicatorRamp(); Border and Ramp in cm
	UFUNCTION()
	void Ok(FVector Border, FIntVector Amount, FVector Ramp, bool UseCost);
	UFUNCTION()
	void PreviewClicked(FVector Border, FIntVector Amount, FVector Ramp);
	UFUNCTION()
	void UpdateRamp();

	static FAreaActionsPlacement MakePlacement(const FVector& Border, const FIntVector& Amount, const FVector& Ramp);

protected:
	virtual bool IsFill() const override { return true; }
	virtual bool BindWindow() override;

private:
	// Widget_Fill.GetData(); false when the window has not got it
	bool GetData(FVector& Border, FIntVector& Amount, FVector& Ramp);
};

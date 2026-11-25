#pragma once

#include "CoreMinimal.h"
#include "AreaActionsUI.h"
#include "AreaActionsWindows.generated.h"

class UFGRecipe;
class UProgressBar;
class USlider;
class UTextBlock;
class UVerticalBox;
class UHorizontalBox;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAreaActionsWindowEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAreaActionsRecipeSelected, TSubclassOf<UFGRecipe>, Recipe);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FAreaActionsCopyOk, FVector, Delta, FRotator, DeltaRotation, bool, UseCost);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAreaActionsCopyPreview, FVector, Delta, FRotator, DeltaRotation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FAreaActionsFillOk, FVector, Border, FIntVector, Amount, FVector, Ramp, bool, UseCost);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FAreaActionsFillPreview, FVector, Border, FIntVector, Amount, FVector, Ramp);

// SetRecipe's window (was Widget_SetRecipe): the game's search box over the recipes the factory machines make, as the game's list
// buttons with the product's icon. The dispatchers keep the old window's names, so the action binds them as before.
UCLASS()
class AREAACTIONS_API UAreaActionsRecipeWindow : public UAreaActionsWindow
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsRecipeSelected OnRecipeSelected;
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsWindowEvent WidgetCanceled;

	// Widget_InputBox.OnTextChanged(Text)
	UFUNCTION()
	void SearchChanged(FText Text);
	// Widget_ListButton.OnClicked(Index, ListButton)
	UFUNCTION()
	void RecipeClicked(int32 Index, UUserWidget* Button);

	// The recipe buttons in list order (test mode)
	const TMap<TObjectPtr<UUserWidget>, TSubclassOf<UFGRecipe>>& GetRecipeButtons() const { return Recipes; }
	UVerticalBox* GetList() const { return List; }

protected:
	virtual UWidget* BuildContent() override;
	virtual FText GetWindowTitle() const override;
	virtual FVector2D GetWindowSize() const override { return FVector2D(820.0, 760.0); }
	virtual void Canceled() override { WidgetCanceled.Broadcast(); }

private:
	UPROPERTY(Transient)
	TMap<TObjectPtr<UUserWidget>, TSubclassOf<UFGRecipe>> Recipes;
	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> List;
};

// What Copy's and Fill's windows share (were Widget_Copy, Widget_Fill): number fields in the game's input boxes, "Use build cost" as the
// game's toggle, the container distance as a slider with its field, the game's buttons; Hide and the pick buttons hide the window while
// the player clicks into the world.
UCLASS(Abstract)
class AREAACTIONS_API UAreaActionsPlaceWindow : public UAreaActionsWindow
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsWindowEvent WidgetCanceled;

	// A field's value as the player typed it (min, max and the default applied, as the old window's number boxes did)
	double GetNumber(FName Field) const;
	void SetNumber(FName Field, double Value);
	bool GetUseCost() const;
	void SetUseCost(bool bUse);

	// Widget_InputBox.OnTextComitted(Text, CommitMethod)
	UFUNCTION()
	void FieldCommitted(FText Text, uint8 CommitMethod);
	// BPW_ToggleButton.OnToggled(isOn)
	UFUNCTION()
	void CostToggled(bool bOn);
	UFUNCTION()
	void DistanceSlid(float Value);
	UFUNCTION()
	void HideClicked();

protected:
	struct FField
	{
		TObjectPtr<UUserWidget> Box;
		bool bFloat = true;
		double Default = 0.0;
		double Min = -1.0e9;
		double Max = 1.0e9;
		bool bAllowZero = true;
	};

	virtual void Canceled() override { WidgetCanceled.Broadcast(); }
	// A labelled field (the label in the axis's colour when it is an axis)
	UWidget* MakeField(FName Name, const FText& Label, const FLinearColor& LabelColor, const FField& Field);
	UWidget* MakeCostAndDistance();
	UHorizontalBox* MakeButtonRow();
	// The window hides while the player clicks into the world; the action shows it again
	void HideForPick();
	virtual void FieldChanged(FName Field) {}
	void Normalize(FName Field);
	AActor* GetSettings() const;

	TMap<FName, FField> Fields;
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> CostToggle;
	UPROPERTY(Transient)
	TObjectPtr<USlider> DistanceSlider;
	UPROPERTY(Transient)
	TObjectPtr<UProgressBar> DistanceBar;
};

// Copy's settings: offset in metres, rotation in degrees, paste to a clicked place, rotation centre, preview, OK
UCLASS()
class AREAACTIONS_API UAreaActionsCopyWindow : public UAreaActionsPlaceWindow
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsCopyOk OkClicked;
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsCopyPreview Preview;
	// The action reads these after a click into the world
	UPROPERTY(Transient, BlueprintReadWrite, Category = "Area Actions")
	bool IsSelectingPasteTo = false;
	UPROPERTY(Transient, BlueprintReadWrite, Category = "Area Actions")
	bool IsSelectingRotationPoint = false;

	// Delta in cm, the rotation's yaw in degrees
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void GetData(FVector& Delta, FRotator& DeltaRotation) const;
	// Delta in metres, as the fields show it
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void SetData(FVector Delta, FRotator DeltaRotation);

	UFUNCTION()
	void OkPressed();
	UFUNCTION()
	void PreviewPressed();
	UFUNCTION()
	void PasteToPressed();
	UFUNCTION()
	void RotationCenterPressed();

protected:
	virtual UWidget* BuildContent() override;
	virtual FText GetWindowTitle() const override;
	virtual FVector2D GetWindowSize() const override { return FVector2D(900.0, 620.0); }
};

// Fill's settings: copies along each axis (with direction), the gap between them, the ramp, fill to a clicked place, preview, OK
UCLASS()
class AREAACTIONS_API UAreaActionsFillWindow : public UAreaActionsPlaceWindow
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsFillOk OkClicked;
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsFillPreview Preview;
	UPROPERTY(BlueprintAssignable, Category = "Area Actions")
	FAreaActionsWindowEvent UpdateIndicatorRamp;
	UPROPERTY(Transient, BlueprintReadWrite, Category = "Area Actions")
	bool IsSelectingFillTo = false;

	// Border and Ramp in cm
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void GetData(FVector& Border, FIntVector& Amount, FVector& Ramp) const;
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void SetData(FIntVector Amount);

	UFUNCTION()
	void OkPressed();
	UFUNCTION()
	void PreviewPressed();
	UFUNCTION()
	void FillToPressed();

protected:
	virtual UWidget* BuildContent() override;
	virtual FText GetWindowTitle() const override;
	virtual FVector2D GetWindowSize() const override { return FVector2D(900.0, 660.0); }
	virtual void FieldChanged(FName Field) override;
};

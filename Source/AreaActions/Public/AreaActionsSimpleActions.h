#pragma once

#include "CoreMinimal.h"
#include "AreaActionBase.h"
#include "AreaActionsSimpleActions.generated.h"

class UFGRecipe;
class UUserWidget;

// The recipe list (Widget_SetRecipe), then the request; the result names the machines changed per class
UCLASS()
class AREAACTIONS_API UAreaActionSetRecipe : public UAreaAction
{
	GENERATED_BODY()

public:
	virtual void Start() override;
	virtual void OnResult(const FAreaActionsResult& Result) override;

	// Widget_SetRecipe.OnRecipeSelected(Recipe)
	UFUNCTION()
	void Selected(TSubclassOf<UFGRecipe> InRecipe);
	// Widget_SetRecipe.WidgetCanceled()
	UFUNCTION()
	void Canceled();

private:
	TSubclassOf<UFGRecipe> Recipe;
};

// A yes/no question, then the request
UCLASS()
class AREAACTIONS_API UAreaActionClearInventories : public UAreaAction
{
	GENERATED_BODY()

public:
	virtual void Start() override;
	virtual void OnResult(const FAreaActionsResult& Result) override;

	UFUNCTION()
	void Confirm();
	UFUNCTION()
	void Cancel();
};

// "Should the stations load?": yes loads, no (and closing the window, as in the original) unloads
UCLASS()
class AREAACTIONS_API UAreaActionSetStationMode : public UAreaAction
{
	GENERATED_BODY()

public:
	virtual void Start() override;
	virtual void OnResult(const FAreaActionsResult& Result) override;

	UFUNCTION()
	void Load();
	UFUNCTION()
	void Unload();

private:
	void Send(bool bInLoad);
	bool bLoad = false;
};

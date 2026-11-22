#pragma once

#include "CoreMinimal.h"
#include "AreaActionsTypes.h"
#include "GameFramework/Actor.h"
#include "AreaActionBase.generated.h"

class AAreaActionBase;
class AAreaActionsEquipment;
class UAreaActionsRCO;
class UUserWidget;

// The C++ side of one action on the player's machine: its windows, its request to the server and the answer.
UCLASS(Abstract)
class AREAACTIONS_API UAreaAction : public UObject
{
	GENERATED_BODY()

public:
	AAreaActionBase* GetShell() const;
	// The area is read; open the first window or send the request
	virtual void Start() {}
	// The server's answer to the request sent through the shell
	virtual void OnResult(const FAreaActionsResult& Result) {}
};

// Parent of the Blueprint BaseAction. The tool spawns an action's Blueprint on the player's machine (Equip_AreaActions.InitAction); with
// NativeAction set, this class runs it in C++ and the Blueprint only holds data. Without it (Copy, Fill, MassDismantle until they move to
// C++) the Blueprint's own graph runs as before. Names here differ from the Blueprint's members (Title, Description, Player,
// AreaActionsEquip, Init, Run, Done, Finished), which stay in the Blueprint.
UCLASS(Abstract)
class AREAACTIONS_API AAreaActionBase : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Area Actions")
	TSubclassOf<UAreaAction> NativeAction;

	// The action's own window (SetRecipe: the recipe list); the message windows are shared
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Area Actions")
	TSubclassOf<UUserWidget> ParamsWidgetClass;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Area Actions")
	TSubclassOf<UUserWidget> MessageOkClass;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Area Actions")
	TSubclassOf<UUserWidget> MessageYesNoClass;

	// Opens a window of the tool: interact object = the tool, Title and Message set when the window has them, added to the tool's stack
	UUserWidget* OpenWindow(TSubclassOf<UUserWidget> Class, const FText& Title = FText::GetEmpty(), const FText& Message = FText::GetEmpty());
	// Binds a Blueprint event dispatcher of the window to a function of Target by name; false (and an error) when either is missing
	static bool BindWindowEvent(UUserWidget* Window, FName Dispatcher, UObject* Target, FName Function);
	// A message with OK; OK ends the action
	void ShowMessage(const FText& Title, const FText& Message);
	// A message for a result code that is not Ok; false when Code is Ok
	bool ShowFailure(EAreaActionsResultCode Code);

	// A new request id; the answer with this id comes to the action's OnResult
	int32 BeginRequest();
	UAreaActionsRCO* GetRco() const;
	AAreaActionsEquipment* GetEquipment() const;
	const FAreaActionsArea& GetArea() const { return Area; }
	bool IsActive() const { return !bFinished && IsValid(this); }

	// Ends the action through the Blueprint's Finished, so the tool destroys it and takes the next one
	UFUNCTION()
	void Finish();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void StartNative();
	void HandleResult(int32 Id, const FAreaActionsResult& Result);

	UPROPERTY(Transient)
	TObjectPtr<UAreaAction> Action;

	FAreaActionsArea Area;
	int32 RequestId = INDEX_NONE;
	FDelegateHandle ResultHandle;
	bool bFinished = false;
};

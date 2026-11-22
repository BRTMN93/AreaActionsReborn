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
	// Binds a Blueprint event dispatcher of the window to a function of Target by name; false (and an error) when either is missing or
	// their parameters differ
	static bool BindWindowEvent(UUserWidget* Window, FName Dispatcher, UObject* Target, FName Function);
	// A question with two named answers: the yes/no window with its buttons renamed, each button calls a function of Target by name.
	// Closing the window (X, or the tool put away) without an answer ends the action. False (and an error) when the window cannot open.
	bool OpenChoice(const FText& Title, const FText& Message, const FText& First, const FText& Second, UObject* Target, FName OnFirst, FName OnSecond);
	// A message with OK; OK ends the action
	void ShowMessage(const FText& Title, const FText& Message);
	// A message for a result code that is not Ok; false when Code is Ok
	bool ShowFailure(EAreaActionsResultCode Code);

	// A new request id; the answer with this id comes to the action's OnResult. An action sends one request: INDEX_NONE after the first
	int32 BeginRequest();
	UAreaActionsRCO* GetRco() const;
	AAreaActionsEquipment* GetEquipment() const;
	const FAreaActionsArea& GetArea() const { return Area; }
	bool IsActive() const { return !bFinished && IsValid(this); }

	// Ends the action through the Blueprint's Finished, so the tool destroys it and takes the next one
	UFUNCTION()
	void Finish();

	// The yes/no window's NoClicked, which it also fires for X and for its second button: the cancel waits one tick, so a button that
	// sends a request in the same click wins
	UFUNCTION()
	void ChoiceClosed();

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
	// Requests begun so far, and that count when the open choice window appeared
	int32 RequestCount = 0;
	int32 RequestCountAtChoice = 0;
	FDelegateHandle ResultHandle;
	bool bFinished = false;
};

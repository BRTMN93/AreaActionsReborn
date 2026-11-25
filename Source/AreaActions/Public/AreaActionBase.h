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
	// The shell while it runs; null once it ended. The window's handlers run also when the tool is put away (the windows close after the
	// action is gone); they do nothing then
	AAreaActionBase* GetActiveShell() const;
	// The area is read; open the first window or send the request
	virtual void Start() {}
	// The server's answer to the request sent through the shell
	virtual void OnResult(const FAreaActionsResult& Result) {}
	// A click into the world while the action runs (the tool forwards both buttons)
	virtual void OnClick(bool bRight) {}
	// The action ends (finished, cancelled or the tool put away): what it put into the world goes
	virtual void End() {}
};

// Parent of the Blueprint BaseAction. The tool spawns an action's Blueprint on the player's machine (Equip_AreaActions.InitAction); with
// NativeAction set, this class runs it in C++ and the Blueprint only holds data. Without it (MassDismantle until it moves to C++) the
// Blueprint's own graph runs as before. Names here differ from the Blueprint's members (Title, Description, Player, AreaActionsEquip, Init,
// Run, Done, Finished), which stay in the Blueprint, except OnLeftClick and OnRightClick, which replace BaseAction's own events.
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
	// The same with two or more answers; past the window's two buttons, more are made like its first
	struct FChoiceAnswer { FText Label; FName Function; };
	bool OpenChoice(const FText& Title, const FText& Message, const TArray<FChoiceAnswer>& Answers, UObject* Target);
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

	// The tool's clicks while this action is active (Equip_AreaActions calls them on its ActiveAction)
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void OnLeftClick();
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void OnRightClick();

	// The C++ action, for the test mode
	UAreaAction* GetAction() const { return Action; }

	// Ends the action through the Blueprint's Finished, so the tool destroys it and takes the next one
	UFUNCTION()
	void Finish();

	// The yes/no window's NoClicked, which it also fires for X and for its second button: the cancel waits one tick, so a button that
	// sends a request in the same click wins
	UFUNCTION()
	void ChoiceClosed();

	// A button the choice window got from OpenChoice closes the window
	UFUNCTION()
	void CloseChoice();

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
	TWeakObjectPtr<UUserWidget> ChoiceWindow;
	FDelegateHandle ResultHandle;
	bool bFinished = false;
};

#pragma once

#include "CoreMinimal.h"
#include "AreaActionsUI.h"
#include "AreaActionsPopupBase.generated.h"

class UBorder;
class UTextBlock;

// Parent of the tool's menu Widget_AreaActionPopup, built here from the game's widgets (D23): Fly and Help buttons, the selection modes
// and the actions as the game's list buttons, the help steps under them. The Blueprint keeps its dispatchers (BuildStatusSelected,
// ActionSelected), which the tool binds, and the help's texts (HelpMessages, NextText, DoneText, the *MessagesIdx lists).
UCLASS(Abstract)
class AREAACTIONS_API UAreaActionsPopupBase : public UAreaActionsWindow
{
	GENERATED_BODY()

public:
	// "Area Actions v<version from the .uplugin>"
	UFUNCTION(BlueprintPure, Category = "Area Actions")
	FText GetTitle() const;

	// The help from its first step (the tool calls it when the help shows by itself; the Help button too)
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void ShowHelp();

	// The help step on screen, INDEX_NONE when the help is closed
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Area Actions")
	int32 TutorialStep = INDEX_NONE;

	UFUNCTION()
	void FlyClicked();
	UFUNCTION()
	void HelpClicked();
	UFUNCTION()
	void PrevClicked();
	UFUNCTION()
	void NextClicked();
	// Widget_ListButton.OnClicked(Index, ListButton)
	UFUNCTION()
	void StateClicked(int32 Index, UUserWidget* Button);
	UFUNCTION()
	void ActionClicked(int32 Index, UUserWidget* Button);

protected:
	virtual UWidget* BuildContent() override;
	virtual FText GetWindowTitle() const override { return GetTitle(); }
	virtual FVector2D GetWindowSize() const override { return FVector2D(960.0, 820.0); }

private:
	void UpdateTutorial();
	void UpdateFlyText();
	// One of the Blueprint's dispatchers with its one parameter
	void Broadcast(FName Dispatcher, TFunctionRef<void(FProperty*, void*)> SetParam);

	UPROPERTY(Transient)
	TMap<TObjectPtr<UUserWidget>, uint8> StateButtons;
	UPROPERTY(Transient)
	TMap<TObjectPtr<UUserWidget>, TObjectPtr<UClass>> ActionButtons;
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> FlyWidget;
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> NextWidget;
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> PrevWidget;
	UPROPERTY(Transient)
	TObjectPtr<UBorder> TutorialBox;
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TutorialTextBlock;
	// The parts the help points at, outlined while their step shows: fly, help, selection modes, actions
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> Marks;
	bool bActed = false;
};

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "FGPopupWidgetContent.h"
#include "UI/FGInteractWidget.h"
#include "AreaActionsUI.generated.h"

class UPanelWidget;
class UTextBlock;
class UVerticalBox;
class UWidgetTree;
class UTexture2D;

// The mod's windows are built in C++ from the game's own widgets (AA-084, decision D23): the window frame, its backgrounds, the
// standard buttons, the list buttons, the search box and the toggle. The starter project holds only stubs of them; the packaged game
// loads the real ones by path.
namespace AreaActionsUI
{
	// Game widget classes, loaded by path
	UClass* GameClass(const TCHAR* Path);
	extern const TCHAR* const FramePath;
	extern const TCHAR* const ContentBackgroundPath;
	extern const TCHAR* const AlternateBackgroundPath;
	extern const TCHAR* const StandardButtonPath;
	extern const TCHAR* const ListButtonPath;
	extern const TCHAR* const ScrollboxPath;
	extern const TCHAR* const InputBoxPath;
	extern const TCHAR* const TogglePath;

	// The game's orange (input border, selection) and its dark greys
	extern const FLinearColor Orange;
	extern const FLinearColor Dark;
	extern const FLinearColor Darker;

	// The game's UI font (DescriptionText) with a face ("Default", "Medium", "Bold")
	FSlateFontInfo Font(float Size, FName Face = TEXT("Default"));

	// Blueprint variables of a game widget, set before it is constructed (its PreConstruct reads them)
	bool SetText(UObject* Object, FName Name, const FText& Value);
	bool SetBool(UObject* Object, FName Name, bool bValue);
	bool SetObject(UObject* Object, FName Name, UObject* Value);
	bool SetByte(UObject* Object, FName Name, uint8 Value);
	bool GetBool(const UObject* Object, FName Name);
	// A Blueprint event dispatcher of a widget, bound to a UFUNCTION of Target by name (no signature check: the handlers take the
	// dispatcher's parameters)
	bool Bind(UObject* Widget, FName Dispatcher, UObject* Target, FName Function);
	// Calls a Blueprint function without parameters, or with one bool or object parameter
	bool Call(UObject* Object, FName Function);
	bool CallBool(UObject* Object, FName Function, bool bValue);

	UUserWidget* MakeGame(UWidgetTree* Tree, const TCHAR* Path, FName Name = NAME_None);
	UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, float Size, FName Face = TEXT("Default"), const FLinearColor& Color = FLinearColor::White, FName Name = NAME_None);
	// Widget_StandardButton with its label; its OnClicked calls Function of Target
	UUserWidget* MakeButton(UWidgetTree* Tree, FName Name, const FText& Text, UObject* Target, FName Function, bool bOrange = false);
	void SetButtonText(UUserWidget* Button, const FText& Text);
	void SetButtonEnabled(UUserWidget* Button, bool bEnabled);
	// Widget_ListButton: icon (may be null), title, a line under it; OnClicked(Index, ListButton) calls Function of Target
	UUserWidget* MakeListButton(UWidgetTree* Tree, FName Name, const FText& Title, const FText& Description, UTexture2D* Icon, UPanelWidget* List,
		UObject* Target, FName Function);
	// Widget_Scrollbox with a vertical box as its content
	UUserWidget* MakeScroll(UWidgetTree* Tree, UVerticalBox*& OutBox, FName Name = NAME_None);
	// The game's section heading: bold text over a thin line
	UWidget* MakeHeading(UWidgetTree* Tree, const FText& Text);

	// Simulates the click of a game button (its OnClicked dispatcher, as its inner button sends it); for the test mode
	bool Click(UUserWidget* Button);
}

// A window of the tool in the game's look: the game's window frame (title, close button), its content background and the content
// BuildContent makes. Opened through the tool's window stack (Equip_AreaActions.AddWidget), closed through it (RemoveWidget).
UCLASS(Abstract)
class AREAACTIONSREBORN_API UAreaActionsWindow : public UFGInteractWidget
{
	GENERATED_BODY()

public:
	UAreaActionsWindow(const FObjectInitializer& ObjectInitializer);

	// Through the tool's window stack (back to the game's input once the last window closed)
	UFUNCTION()
	void CloseWindow();

	// The frame's X and Escape
	UFUNCTION()
	void CloseClicked();

	UUserWidget* GetFrame() const { return Frame; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual FReply NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
	virtual void NativeDestruct() override;

	// The window's content (inside the frame, on its background) and its size in layout units
	virtual UWidget* BuildContent() { return nullptr; }
	virtual FText GetWindowTitle() const { return FText::GetEmpty(); }
	virtual FVector2D GetWindowSize() const { return FVector2D(900.0, 600.0); }
	// Closed without an answer (X, Escape, the tool put away)
	virtual void Canceled() {}

	// The window gave its answer: closing it no longer counts as cancel
	bool bHasResult = false;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> Frame;
};

// The tool's message window Equip_AreaActions makes (Widget_MessageOk: "At least 3 corners needed"): it shows the game's popup with its
// Title and Message (Blueprint strings) and leaves the tool's stack at once.
UCLASS(Abstract)
class AREAACTIONSREBORN_API UAreaActionsMessageShim : public UFGInteractWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;
};

// The selection state under the crosshair (Widget_AreaBuildingStatus), in the look of the game's key hints
UCLASS(Abstract)
class AREAACTIONSREBORN_API UAreaActionsStatusWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Text;
	FString Shown;
};

// A question in the game's popup: its message and one game button per answer, plus Cancel unless an answer cancels. The popup's own
// close (Escape) counts as Cancel.
UCLASS()
class AREAACTIONSREBORN_API UAreaActionsChoiceContent : public UFGPopupWidgetContent
{
	GENERATED_BODY()

public:
	// bCancel: the answer Escape (or the popup closed by the game) runs
	struct FAnswer { FText Label; FName Name; TFunction<void()> Run; bool bCancel = false; };
	void Setup(const FText& InMessage, TArray<FAnswer>&& InAnswers);

	// The popup closed (an answer, Cancel or Escape)
	UFUNCTION()
	void PopupClosed(bool bConfirmed);
	// A game button's OnClickedIncludeSelf(buttonWidget)
	UFUNCTION()
	void AnswerClicked(UUserWidget* Button);

	// Opens it in the game's popup queue for Player
	static UAreaActionsChoiceContent* Open(APlayerController* Player, const FText& Title, const FText& Message, TArray<FAnswer>&& Answers);
	// Closes the popup that shows this content, when it is the one on screen
	void Close();

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Area Actions")
	FString Message;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	TArray<FAnswer> Answers;
	TArray<TObjectPtr<UUserWidget>> Buttons;
	// The answer that runs once the popup has closed
	int32 Picked = INDEX_NONE;
	bool bDone = false;
};

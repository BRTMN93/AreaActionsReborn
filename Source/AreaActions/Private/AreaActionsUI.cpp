#include "AreaActionsUI.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsLog.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "FGBlueprintFunctionLibrary.h"
#include "GameFramework/PlayerController.h"
#include "UI/FGPopupWidget.h"

#define LOCTEXT_NAMESPACE "AreaActions"

namespace AreaActionsUI
{
	const TCHAR* const FramePath = TEXT("/Game/FactoryGame/Interface/UI/Widget_Window_DarkMode.Widget_Window_DarkMode_C");
	const TCHAR* const ContentBackgroundPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/Widget_Window_ContentBackground_DarkMode.Widget_Window_ContentBackground_DarkMode_C");
	const TCHAR* const AlternateBackgroundPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/Widget_Window_AlternateContentBackground_DarkMode.Widget_Window_AlternateContentBackground_DarkMode_C");
	const TCHAR* const StandardButtonPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/-Shared/Widget_StandardButton.Widget_StandardButton_C");
	const TCHAR* const ListButtonPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/-Shared/Widget_ListButton.Widget_ListButton_C");
	const TCHAR* const ScrollboxPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/-Shared/Widget_Scrollbox.Widget_Scrollbox_C");
	const TCHAR* const InputBoxPath = TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_InputBox.Widget_InputBox_C");
	const TCHAR* const TogglePath = TEXT("/Game/FactoryGame/Interface/BPW_ToggleButton.BPW_ToggleButton_C");

	// Colours read from the game's widgets (UIProbe): the input box's border, the list button's text, the window's header
	const FLinearColor Orange(0.783538f, 0.291771f, 0.059511f, 1.0f);
	const FLinearColor Dark(0.095307f, 0.095307f, 0.095307f, 1.0f);
	const FLinearColor Darker(0.05f, 0.05f, 0.05f, 1.0f);

	UClass* GameClass(const TCHAR* Path)
	{
		UClass* Class = LoadClass<UUserWidget>(nullptr, Path);
		if (!Class)
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("The game's widget %s is missing"), Path);
		}
		return Class;
	}

	FSlateFontInfo Font(float Size, FName Face)
	{
		static const TCHAR* Path = TEXT("/Game/FactoryGame/Interface/Font/DescriptionText.DescriptionText");
		FSlateFontInfo Info(LoadObject<UObject>(nullptr, Path), FMath::RoundToInt(Size), Face);
		return Info;
	}

	template <typename TProperty>
	TProperty* Find(const UObject* Object, FName Name)
	{
		TProperty* Property = Object ? FindFProperty<TProperty>(Object->GetClass(), Name) : nullptr;
		if (Object && !Property)
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("%s has no %s"), *Object->GetClass()->GetName(), *Name.ToString());
		}
		return Property;
	}

	bool SetText(UObject* Object, FName Name, const FText& Value)
	{
		FTextProperty* Property = Find<FTextProperty>(Object, Name);
		return Property ? (Property->SetPropertyValue_InContainer(Object, Value), true) : false;
	}

	bool SetBool(UObject* Object, FName Name, bool bValue)
	{
		FBoolProperty* Property = Find<FBoolProperty>(Object, Name);
		return Property ? (Property->SetPropertyValue_InContainer(Object, bValue), true) : false;
	}

	bool GetBool(const UObject* Object, FName Name)
	{
		const FBoolProperty* Property = Find<FBoolProperty>(Object, Name);
		return Property && Property->GetPropertyValue_InContainer(Object);
	}

	bool SetObject(UObject* Object, FName Name, UObject* Value)
	{
		FObjectPropertyBase* Property = Find<FObjectPropertyBase>(Object, Name);
		return Property ? (Property->SetObjectPropertyValue_InContainer(Object, Value), true) : false;
	}

	bool SetByte(UObject* Object, FName Name, uint8 Value)
	{
		if (FByteProperty* Byte = Object ? FindFProperty<FByteProperty>(Object->GetClass(), Name) : nullptr)
		{
			Byte->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		if (FEnumProperty* Enum = Find<FEnumProperty>(Object, Name))
		{
			Enum->GetUnderlyingProperty()->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Object), static_cast<int64>(Value));
			return true;
		}
		return false;
	}

	bool Bind(UObject* Widget, FName Dispatcher, UObject* Target, FName Function)
	{
		FMulticastDelegateProperty* Property = Find<FMulticastDelegateProperty>(Widget, Dispatcher);
		if (!Property || !Target || !Target->FindFunction(Function))
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("Cannot bind %s.%s to %s.%s"), *GetNameSafe(Widget), *Dispatcher.ToString(), *GetNameSafe(Target), *Function.ToString());
			return false;
		}
		FScriptDelegate Delegate;
		Delegate.BindUFunction(Target, Function);
		Property->AddDelegate(MoveTemp(Delegate), Widget);
		return true;
	}

	bool Call(UObject* Object, FName Function)
	{
		UFunction* Found = Object ? Object->FindFunction(Function) : nullptr;
		if (!Found || Found->ParmsSize != 0)
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("%s has no %s()"), *GetNameSafe(Object), *Function.ToString());
			return false;
		}
		Object->ProcessEvent(Found, nullptr);
		return true;
	}

	bool CallBool(UObject* Object, FName Function, bool bValue)
	{
		UFunction* Found = Object ? Object->FindFunction(Function) : nullptr;
		if (!Found)
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("%s has no %s"), *GetNameSafe(Object), *Function.ToString());
			return false;
		}
		TArray<uint8> Params;
		Params.SetNumZeroed(FMath::Max<int32>(1, Found->ParmsSize));
		for (TFieldIterator<FProperty> It(Found); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params.GetData());
			if (FBoolProperty* Flag = CastField<FBoolProperty>(*It))
			{
				Flag->SetPropertyValue_InContainer(Params.GetData(), bValue);
				bValue = false;
			}
		}
		Object->ProcessEvent(Found, Params.GetData());
		for (TFieldIterator<FProperty> It(Found); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params.GetData());
		}
		return true;
	}

	UUserWidget* MakeGame(UWidgetTree* Tree, const TCHAR* Path, FName Name)
	{
		UClass* Class = GameClass(Path);
		return Class ? Tree->ConstructWidget<UUserWidget>(Class, Name) : nullptr;
	}

	UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, float Size, FName Face, const FLinearColor& Color, FName Name)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Block->SetText(Text);
		Block->SetFont(Font(Size, Face));
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetAutoWrapText(true);
		return Block;
	}

	UUserWidget* MakeButton(UWidgetTree* Tree, FName Name, const FText& Text, UObject* Target, FName Function, bool bOrange)
	{
		UUserWidget* Button = MakeGame(Tree, StandardButtonPath, Name);
		if (!Button)
		{
			return nullptr;
		}
		SetText(Button, TEXT("mText"), Text);
		SetBool(Button, TEXT("mIsOrangeButton"), bOrange);
		if (Target)
		{
			Bind(Button, TEXT("OnClicked"), Target, Function);
		}
		return Button;
	}

	void SetButtonText(UUserWidget* Button, const FText& Text)
	{
		UFunction* Function = Button ? Button->FindFunction(TEXT("SetText")) : nullptr;
		FTextProperty* Param = Function ? CastField<FTextProperty>(Function->ChildProperties) : nullptr;
		if (!Param)
		{
			SetText(Button, TEXT("mText"), Text);
			return;
		}
		TArray<uint8> Params;
		Params.SetNumZeroed(Function->ParmsSize);
		Param->InitializeValue_InContainer(Params.GetData());
		Param->SetPropertyValue_InContainer(Params.GetData(), Text);
		Button->ProcessEvent(Function, Params.GetData());
		Param->DestroyValue_InContainer(Params.GetData());
	}

	void SetButtonEnabled(UUserWidget* Button, bool bEnabled)
	{
		if (Button)
		{
			CallBool(Button, TEXT("SetIsDisabled"), !bEnabled);
		}
	}

	UUserWidget* MakeListButton(UWidgetTree* Tree, FName Name, const FText& Title, const FText& Description, UTexture2D* Icon, UPanelWidget* List,
		UObject* Target, FName Function)
	{
		UUserWidget* Button = MakeGame(Tree, ListButtonPath, Name);
		if (!Button)
		{
			return nullptr;
		}
		SetText(Button, TEXT("mTitle"), Title);
		SetText(Button, TEXT("mAdditionalInfo"), Description);
		SetObject(Button, TEXT("mIcon"), Icon);
		SetByte(Button, TEXT("IconVisibility"), static_cast<uint8>(Icon ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed));
		SetObject(Button, TEXT("mListWidget"), List);
		Bind(Button, TEXT("OnClicked"), Target, Function);
		return Button;
	}

	UUserWidget* MakeScroll(UWidgetTree* Tree, UVerticalBox*& OutBox, FName Name)
	{
		OutBox = Tree->ConstructWidget<UVerticalBox>();
		UUserWidget* Scroll = MakeGame(Tree, ScrollboxPath, Name);
		if (!Scroll)
		{
			return nullptr;
		}
		Scroll->SetContentForSlot(TEXT("mScrollableContent"), OutBox);
		return Scroll;
	}

	UWidget* MakeHeading(UWidgetTree* Tree, const FText& Text)
	{
		UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
		UTextBlock* Title = MakeText(Tree, Text, 14.0f, TEXT("Bold"), FLinearColor(0.79f, 0.79f, 0.78f, 1.0f));
		Box->AddChildToVerticalBox(Title)->SetPadding(FMargin(4.0f, 0.0f, 4.0f, 4.0f));
		UImage* Line = Tree->ConstructWidget<UImage>();
		Line->SetColorAndOpacity(Orange);
		Line->SetDesiredSizeOverride(FVector2D(1.0, 2.0));
		Box->AddChildToVerticalBox(Line)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));
		return Box;
	}

	bool Click(UUserWidget* Button)
	{
		if (!Button)
		{
			return false;
		}
		bool bFired = false;
		for (const TCHAR* Name : { TEXT("OnClicked"), TEXT("OnClickedIncludeSelf") })
		{
			FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(Button->GetClass(), Name);
			const UFunction* Signature = Property ? Property->SignatureFunction : nullptr;
			if (!Signature)
			{
				continue;
			}
			TArray<uint8> Params;
			Params.SetNumZeroed(FMath::Max<int32>(1, Signature->ParmsSize));
			// The list button's (Index, ListButton) and the standard button's (buttonWidget): the button itself
			for (TFieldIterator<FProperty> It(Signature); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				if (FObjectPropertyBase* Self = CastField<FObjectPropertyBase>(*It))
				{
					Self->SetObjectPropertyValue_InContainer(Params.GetData(), Button);
				}
			}
			Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Button)->ProcessMulticastDelegate<UObject>(Params.GetData());
			bFired = true;
		}
		return bFired;
	}
}

UAreaActionsWindow::UAreaActionsWindow(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetIsFocusable(true);
}

TSharedRef<SWidget> UAreaActionsWindow::RebuildWidget()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (!Frame)
	{
		// Root: the whole screen; in its middle the frame at the window's size, as the game's own windows sit
		UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("WindowRoot"));
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("WindowSize"));
		const FVector2D Wanted = GetWindowSize();
		Size->SetWidthOverride(Wanted.X);
		Size->SetHeightOverride(Wanted.Y);
		UOverlaySlot* SizeSlot = Root->AddChildToOverlay(Size);
		SizeSlot->SetHorizontalAlignment(HAlign_Center);
		SizeSlot->SetVerticalAlignment(VAlign_Center);
		Frame = AreaActionsUI::MakeGame(WidgetTree, AreaActionsUI::FramePath, TEXT("WindowFrame"));
		UOverlay* Body = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("WindowBody"));
		if (UUserWidget* Background = AreaActionsUI::MakeGame(WidgetTree, AreaActionsUI::ContentBackgroundPath))
		{
			UOverlaySlot* BackSlot = Body->AddChildToOverlay(Background);
			BackSlot->SetHorizontalAlignment(HAlign_Fill);
			BackSlot->SetVerticalAlignment(VAlign_Fill);
		}
		if (UWidget* Content = BuildContent())
		{
			UOverlaySlot* ContentSlot = Body->AddChildToOverlay(Content);
			ContentSlot->SetHorizontalAlignment(HAlign_Fill);
			ContentSlot->SetVerticalAlignment(VAlign_Fill);
			ContentSlot->SetPadding(FMargin(20.0f));
		}
		if (Frame)
		{
			AreaActionsUI::SetText(Frame, TEXT("mTitleText"), GetWindowTitle());
			AreaActionsUI::SetBool(Frame, TEXT("mShowCloseButton"), true);
			AreaActionsUI::SetBool(Frame, TEXT("mCanShowInventory"), false);
			AreaActionsUI::SetBool(Frame, TEXT("mShowButtonHints"), false);
			AreaActionsUI::SetBool(Frame, TEXT("mCreateInventoryAddonOnConstruction"), false);
			AreaActionsUI::SetByte(Frame, TEXT("mHorizontalAlignment"), HAlign_Fill);
			// The frame's title tab: the mod's icon with the title, as the game's popup shows its warning icon
			if (FArrayProperty* Tabs = FindFProperty<FArrayProperty>(Frame->GetClass(), TEXT("mTabs")))
			{
				FScriptArrayHelper Array(Tabs, Tabs->ContainerPtrToValuePtr<void>(Frame));
				const FStructProperty* Inner = CastField<FStructProperty>(Tabs->Inner);
				if (Inner && Array.Num() == 0)
				{
					Array.AddValue();
				}
				for (TFieldIterator<FProperty> It(Inner ? Inner->Struct : nullptr); Inner && It; ++It)
				{
					void* Value = It->ContainerPtrToValuePtr<void>(Array.GetRawPtr(0));
					if (FTextProperty* TabText = CastField<FTextProperty>(*It))
					{
						TabText->SetPropertyValue(Value, GetWindowTitle());
					}
					else if (FObjectPropertyBase* TabIcon = CastField<FObjectPropertyBase>(*It))
					{
						TabIcon->SetObjectPropertyValue(Value, LoadObject<UTexture2D>(nullptr, TEXT("/AreaActions/Equipment/AreaActions/UI/AreaActionsIcon64.AreaActionsIcon64")));
					}
				}
			}
			Frame->SetContentForSlot(TEXT("WindowBody"), Body);
			AreaActionsUI::Bind(Frame, TEXT("OnClose"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsWindow, CloseClicked));
			Size->AddChild(Frame);
		}
		else
		{
			Size->AddChild(Body);
		}
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

FReply UAreaActionsWindow::NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	if (Event.GetKey() == EKeys::Escape)
	{
		CloseClicked();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(Geometry, Event);
}

void UAreaActionsWindow::CloseClicked()
{
	CloseWindow();
}

void UAreaActionsWindow::CloseWindow()
{
	// Equip_AreaActions.RemoveWidget(Widget): off the tool's stack, the input back to the game when it was the last window
	UObject* Tool = mInteractObject;
	UFunction* Remove = Tool ? Tool->FindFunction(TEXT("RemoveWidget")) : nullptr;
	FObjectPropertyBase* Param = Remove ? CastField<FObjectPropertyBase>(Remove->ChildProperties) : nullptr;
	if (Param && IsA(Param->PropertyClass))
	{
		TArray<uint8> Params;
		Params.SetNumZeroed(Remove->ParmsSize);
		Param->SetObjectPropertyValue_InContainer(Params.GetData(), this);
		Tool->ProcessEvent(Remove, Params.GetData());
	}
	else
	{
		RemoveFromParent();
	}
}

void UAreaActionsWindow::NativeDestruct()
{
	if (!bHasResult)
	{
		bHasResult = true;
		Canceled();
	}
	Super::NativeDestruct();
}

void UAreaActionsMessageShim::NativeConstruct()
{
	Super::NativeConstruct();
	auto ReadString = [this](const TCHAR* Name)
	{
		const FStrProperty* Property = FindFProperty<FStrProperty>(GetClass(), Name);
		return Property ? Property->GetPropertyValue_InContainer(this) : FString();
	};
	FPopupClosed Closed;
	UFGBlueprintFunctionLibrary::AddPopupWithCloseDelegate(GetOwningPlayer(), FText::FromString(ReadString(TEXT("Title"))), FText::FromString(ReadString(TEXT("Message"))), Closed, PID_OK);
	// Off the tool's stack in the next tick, after AddWidget has finished with it
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			UFunction* Remove = mInteractObject ? mInteractObject->FindFunction(TEXT("RemoveWidget")) : nullptr;
			FObjectPropertyBase* Param = Remove ? CastField<FObjectPropertyBase>(Remove->ChildProperties) : nullptr;
			if (Param && IsA(Param->PropertyClass))
			{
				TArray<uint8> Params;
				Params.SetNumZeroed(Remove->ParmsSize);
				Param->SetObjectPropertyValue_InContainer(Params.GetData(), this);
				mInteractObject->ProcessEvent(Remove, Params.GetData());
			}
			else
			{
				RemoveFromParent();
			}
		}));
	}
}

TSharedRef<SWidget> UAreaActionsStatusWidget::RebuildWidget()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (!Text)
	{
		// Bottom right, above the game's own key hints: a dark box with the hint text, as the game's hints
		UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("StatusRoot"));
		UBorder* Box = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("StatusBox"));
		Box->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		Box->SetPadding(FMargin(12.0f, 6.0f));
		Text = AreaActionsUI::MakeText(WidgetTree, FText::GetEmpty(), 14.0f, TEXT("Medium"), FLinearColor::White, TEXT("StatusText"));
		Text->SetAutoWrapText(false);
		Box->SetContent(Text);
		UOverlaySlot* BoxSlot = Root->AddChildToOverlay(Box);
		BoxSlot->SetHorizontalAlignment(HAlign_Right);
		BoxSlot->SetVerticalAlignment(VAlign_Bottom);
		BoxSlot->SetPadding(FMargin(0.0f, 0.0f, 40.0f, 230.0f));
		WidgetTree->RootWidget = Root;
		SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	return Super::RebuildWidget();
}

void UAreaActionsStatusWidget::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
	Super::NativeTick(Geometry, DeltaTime);
	// Equip_AreaActions.GetCurrentStateDescription(): the text of the tool's selection mode
	const FObjectPropertyBase* ToolProperty = FindFProperty<FObjectPropertyBase>(GetClass(), TEXT("AreaActionEquip"));
	UObject* Tool = ToolProperty ? ToolProperty->GetObjectPropertyValue_InContainer(this) : nullptr;
	UFunction* Describe = Tool ? Tool->FindFunction(TEXT("GetCurrentStateDescription")) : nullptr;
	// Its text comes back in an output parameter ("Current state")
	const FTextProperty* Result = nullptr;
	for (TFieldIterator<FProperty> It(Describe); Describe && It && !Result; ++It)
	{
		Result = It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm) ? CastField<FTextProperty>(*It) : nullptr;
	}
	if (!Result || !Text)
	{
		return;
	}
	TArray<uint8> Params;
	Params.SetNumZeroed(Describe->ParmsSize);
	Result->InitializeValue_InContainer(Params.GetData());
	Tool->ProcessEvent(Describe, Params.GetData());
	const FString Now = Result->GetPropertyValue_InContainer(Params.GetData()).ToString();
	Result->DestroyValue_InContainer(Params.GetData());
	if (Now != Shown)
	{
		Shown = Now;
		Text->SetText(FText::FromString(Now));
	}
}

void UAreaActionsChoiceContent::Setup(const FText& InMessage, TArray<FAnswer>&& InAnswers)
{
	Message = InMessage.ToString();
	Answers = MoveTemp(InAnswers);
}

TSharedRef<SWidget> UAreaActionsChoiceContent::RebuildWidget()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (!WidgetTree->RootWidget)
	{
		// As the game's default popup text (Widget_PopupDefaultText: 420 wide, font 12 centred), with the answers under it
		UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ChoiceRoot"));
		USizeBox* TextSize = WidgetTree->ConstructWidget<USizeBox>();
		TextSize->SetWidthOverride(480.0f);
		TextSize->SetMinDesiredHeight(120.0f);
		UTextBlock* Body = AreaActionsUI::MakeText(WidgetTree, FText::FromString(Message), 12.0f, TEXT("Default"), FLinearColor::White, TEXT("MessageText"));
		Body->SetJustification(ETextJustify::Center);
		TextSize->AddChild(Body);
		Root->AddChildToVerticalBox(TextSize)->SetPadding(FMargin(4.0f, 4.0f, 4.0f, 16.0f));
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Answers"));
		for (int32 Index = 0; Index < Answers.Num(); ++Index)
		{
			UUserWidget* Button = AreaActionsUI::MakeButton(WidgetTree, Answers[Index].Name, Answers[Index].Label, nullptr, NAME_None, Index == 0);
			if (!Button)
			{
				continue;
			}
			AreaActionsUI::Bind(Button, TEXT("OnClickedIncludeSelf"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsChoiceContent, AnswerClicked));
			UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button);
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(4.0f, 0.0f));
			Buttons.Add(Button);
		}
		Root->AddChildToVerticalBox(Row);
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

void UAreaActionsChoiceContent::AnswerClicked(UUserWidget* Button)
{
	if (bDone || Picked != INDEX_NONE)
	{
		return;
	}
	Picked = Buttons.IndexOfByKey(Button);
	if (Picked == INDEX_NONE)
	{
		return;
	}
	// The answer runs from the popup's close (PopupClosed), then the popup leaves the screen
	if (mPopupWidget)
	{
		mPopupWidget->CallPopupClosedClicked(true);
	}
	else
	{
		PopupClosed(true);
	}
	UFGBlueprintFunctionLibrary::ClosePopup(GetOwningPlayer());
}

UAreaActionsChoiceContent* UAreaActionsChoiceContent::Open(APlayerController* Player, const FText& Title, const FText& InMessage, TArray<FAnswer>&& InAnswers)
{
	UAreaActionsChoiceContent* Content = Player ? CreateWidget<UAreaActionsChoiceContent>(Player, UAreaActionsChoiceContent::StaticClass()) : nullptr;
	if (!Content)
	{
		return nullptr;
	}
	Content->Setup(InMessage, MoveTemp(InAnswers));
	FPopupClosed Closed;
	Closed.BindUFunction(Content, GET_FUNCTION_NAME_CHECKED(UAreaActionsChoiceContent, PopupClosed));
	UFGBlueprintFunctionLibrary::AddPopupWithContent(Player, Title, FText::GetEmpty(), Closed, Content, EKeys::Invalid, PID_NONE);
	return Content;
}

void UAreaActionsChoiceContent::PopupClosed(bool bConfirmed)
{
	if (bDone)
	{
		return;
	}
	bDone = true;
	// Escape (or the popup closed by the game) is the cancel answer, the last one
	const int32 Index = Answers.IsValidIndex(Picked) ? Picked : Answers.Num() - 1;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Choice '%s': %s"), *Message.Left(60), Answers.IsValidIndex(Index) ? *Answers[Index].Name.ToString() : TEXT("none"));
	if (Answers.IsValidIndex(Index) && Answers[Index].Run)
	{
		Answers[Index].Run();
	}
}

void UAreaActionsChoiceContent::Close()
{
	// No answer runs: the action that asked is gone
	const bool bShown = !bDone && mPopupWidget && mPopupWidget->IsInViewport();
	bDone = true;
	if (bShown)
	{
		UFGBlueprintFunctionLibrary::ClosePopup(GetOwningPlayer());
	}
}

#undef LOCTEXT_NAMESPACE

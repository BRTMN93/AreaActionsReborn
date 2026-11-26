#include "AreaActionsPopupBase.h"
#include "AreaActionBase.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsLog.h"
#include "AreaActionsTutorialSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "ModLoading/ModLoadingLibrary.h"

#define LOCTEXT_NAMESPACE "AreaActions"

namespace
{
	enum EMark { MarkFly, MarkHelp, MarkStates, MarkActions, MarkCount };
	// The Blueprint lists that say which help steps point at which part, in EMark order
	const TCHAR* const MarkLists[MarkCount] = { TEXT("FlyMessagesIdx"), TEXT("HelpToSeeAgainMessagesIdx"), TEXT("AreaBuildMessagesIdx"), TEXT("ActionsMessagesIdx") };

	template <typename TValue, typename TInner>
	TArray<TValue> ReadArray(const UObject* Object, const TCHAR* Name, TFunctionRef<TValue(const TInner*, const void*)> Read)
	{
		TArray<TValue> Out;
		const FArrayProperty* Property = Object ? FindFProperty<FArrayProperty>(Object->GetClass(), Name) : nullptr;
		const TInner* Inner = Property ? CastField<TInner>(Property->Inner) : nullptr;
		if (!Inner)
		{
			return Out;
		}
		FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(Object));
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			Out.Add(Read(Inner, Array.GetRawPtr(Index)));
		}
		return Out;
	}

	FText ReadText(const UObject* Object, const TCHAR* Name)
	{
		const FTextProperty* Property = Object ? FindFProperty<FTextProperty>(Object->GetClass(), Name) : nullptr;
		return Property ? Property->GetPropertyValue_InContainer(Object) : FText::GetEmpty();
	}

	// Defaults_C.StateToTitle / StateToDescription: the selection modes' texts
	FText StateText(const TCHAR* Map, uint8 State)
	{
		const UClass* Defaults = LoadClass<UObject>(nullptr, TEXT("/AreaActionsReborn/Equipment/AreaActions/Defaults.Defaults_C"));
		const UObject* Object = Defaults ? Defaults->GetDefaultObject() : nullptr;
		const FMapProperty* Property = Object ? FindFProperty<FMapProperty>(Object->GetClass(), Map) : nullptr;
		if (!Property || !CastField<FTextProperty>(Property->ValueProp))
		{
			return FText::GetEmpty();
		}
		FScriptMapHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Object));
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			if (*Helper.GetKeyPtr(It) == State)
			{
				return *reinterpret_cast<const FText*>(Helper.GetValuePtr(It));
			}
		}
		return FText::GetEmpty();
	}
}

FText UAreaActionsPopupBase::GetTitle() const
{
	FString Version;
	const UGameInstance* Instance = GetGameInstance();
	FModInfo Info;
	if (UModLoadingLibrary* Mods = Instance ? Instance->GetSubsystem<UModLoadingLibrary>() : nullptr; Mods && Mods->GetLoadedModInfo(TEXT("AreaActionsReborn"), Info))
	{
		Version = Info.Version.ToString();
	}
	return FText::Format(LOCTEXT("PopupTitle", "Area Actions v{Version}"), FFormatNamedArguments{ { TEXT("Version"), FText::FromString(Version) } });
}

UWidget* UAreaActionsPopupBase::BuildContent()
{
	UWidgetTree* Tree = WidgetTree;
	auto Mark = [Tree](UWidget* Content)
	{
		UBorder* Border = Tree->ConstructWidget<UBorder>();
		Border->SetPadding(FMargin(4.0f));
		Border->SetBrush(FSlateNoResource());
		Border->SetContent(Content);
		return Border;
	};
	UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("MenuRoot"));

	// Fly and Help
	UHorizontalBox* Top = Tree->ConstructWidget<UHorizontalBox>();
	FlyWidget = AreaActionsUI::MakeButton(Tree, TEXT("FlyButton"), FText::GetEmpty(), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPopupBase, FlyClicked));
	UWidget* Help = AreaActionsUI::MakeButton(Tree, TEXT("HelpButton"), LOCTEXT("Help", "Help"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPopupBase, HelpClicked));
	UBorder* FlyMark = Mark(FlyWidget);
	UBorder* HelpMark = Mark(Help);
	Top->AddChildToHorizontalBox(FlyMark);
	Top->AddChildToHorizontalBox(Tree->ConstructWidget<USpacer>())->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Top->AddChildToHorizontalBox(HelpMark);
	Root->AddChildToVerticalBox(Top)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	// The selection modes and the actions, side by side
	UHorizontalBox* Columns = Tree->ConstructWidget<UHorizontalBox>();
	UVerticalBox* States = nullptr;
	UVerticalBox* Actions = nullptr;
	UVerticalBox* StatesColumn = Tree->ConstructWidget<UVerticalBox>();
	StatesColumn->AddChildToVerticalBox(AreaActionsUI::MakeHeading(Tree, LOCTEXT("SelectionHeading", "Area selection")));
	if (UUserWidget* Scroll = AreaActionsUI::MakeScroll(Tree, States))
	{
		StatesColumn->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	UVerticalBox* ActionsColumn = Tree->ConstructWidget<UVerticalBox>();
	ActionsColumn->AddChildToVerticalBox(AreaActionsUI::MakeHeading(Tree, LOCTEXT("ActionsHeading", "Actions")));
	if (UUserWidget* Scroll = AreaActionsUI::MakeScroll(Tree, Actions))
	{
		ActionsColumn->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	UBorder* StatesMark = Mark(StatesColumn);
	UBorder* ActionsMark = Mark(ActionsColumn);
	UHorizontalBoxSlot* LeftSlot = Columns->AddChildToHorizontalBox(StatesMark);
	LeftSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	LeftSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));
	Columns->AddChildToHorizontalBox(ActionsMark)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Root->AddChildToVerticalBox(Columns)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Marks = { FlyMark, HelpMark, StatesMark, ActionsMark };

	// Selection modes: the Blueprint dispatcher's enum, all but its last value (the original hid it too)
	const FMulticastDelegateProperty* StateDispatcher = FindFProperty<FMulticastDelegateProperty>(GetClass(), TEXT("BuildStatusSelected"));
	const FProperty* StateParam = StateDispatcher && StateDispatcher->SignatureFunction ? CastField<FProperty>(StateDispatcher->SignatureFunction->ChildProperties) : nullptr;
	const UEnum* Enum = nullptr;
	if (const FByteProperty* ByteParam = CastField<FByteProperty>(StateParam))
	{
		Enum = ByteParam->Enum;
	}
	else if (const FEnumProperty* EnumParam = CastField<FEnumProperty>(StateParam))
	{
		Enum = EnumParam->GetEnum();
	}
	if (!Enum)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: no BuildStatusSelected(enum)"), *GetClass()->GetName());
	}
	for (int32 Index = 0; Enum && States && Index < Enum->NumEnums() - 1; ++Index)
	{
		const FString Name = Enum->GetNameStringByIndex(Index);
		if (Name.EndsWith(TEXT("NewEnumerator15")))
		{
			continue;
		}
		const uint8 Value = static_cast<uint8>(Enum->GetValueByIndex(Index));
		UUserWidget* Button = AreaActionsUI::MakeListButton(Tree, *FString::Printf(TEXT("State_%d"), Value), StateText(TEXT("StateToTitle"), Value),
			StateText(TEXT("StateToDescription"), Value), nullptr, States, this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPopupBase, StateClicked));
		if (Button)
		{
			States->AddChildToVerticalBox(Button);
			StateButtons.Add(Button, Value);
		}
	}

	// Actions: the world module's list, each with its Blueprint's Title and Description
	TArray<TSubclassOf<AAreaActionBase>> Available;
	if (AAreaActionsEquipment* Tool = Cast<AAreaActionsEquipment>(mInteractObject))
	{
		Tool->GetAvailableActions(Available);
	}
	for (const TSubclassOf<AAreaActionBase>& Class : Available)
	{
		const UObject* Default = Class ? Class->GetDefaultObject() : nullptr;
		UUserWidget* Button = Default && Actions ? AreaActionsUI::MakeListButton(Tree, *(TEXT("Action_") + Class->GetName()), ReadText(Default, TEXT("Title")),
			ReadText(Default, TEXT("Description")), nullptr, Actions, this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPopupBase, ActionClicked)) : nullptr;
		if (Button)
		{
			Actions->AddChildToVerticalBox(Button);
			ActionButtons.Add(Button, Class.Get());
		}
	}

	// The help: one step at a time under the lists, with the part it is about outlined
	TutorialBox = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("TutorialWidget"));
	TutorialBox->SetBrush(FSlateRoundedBoxBrush(AreaActionsUI::Darker, 0.0f, AreaActionsUI::Orange, 2.0f));
	TutorialBox->SetPadding(FMargin(14.0f, 10.0f));
	UVerticalBox* Steps = Tree->ConstructWidget<UVerticalBox>();
	TutorialTextBlock = AreaActionsUI::MakeText(Tree, FText::GetEmpty(), 14.0f, TEXT("Default"), FLinearColor::White, TEXT("TutorialText"));
	Steps->AddChildToVerticalBox(TutorialTextBlock)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	UHorizontalBox* StepButtons = Tree->ConstructWidget<UHorizontalBox>();
	StepButtons->AddChildToHorizontalBox(Tree->ConstructWidget<USpacer>())->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	PrevWidget = AreaActionsUI::MakeButton(Tree, TEXT("TutorialPrev"), LOCTEXT("HelpPrevious", "Previous"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPopupBase, PrevClicked));
	NextWidget = AreaActionsUI::MakeButton(Tree, TEXT("TutorialNext"), FText::GetEmpty(), this, GET_FUNCTION_NAME_CHECKED(UAreaActionsPopupBase, NextClicked), true);
	StepButtons->AddChildToHorizontalBox(PrevWidget)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
	StepButtons->AddChildToHorizontalBox(NextWidget);
	Steps->AddChildToVerticalBox(StepButtons);
	TutorialBox->SetContent(Steps);
	TutorialBox->SetVisibility(ESlateVisibility::Collapsed);
	Root->AddChildToVerticalBox(TutorialBox)->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	UpdateFlyText();
	UpdateTutorial();
	return Root;
}

void UAreaActionsPopupBase::Broadcast(FName Dispatcher, TFunctionRef<void(FProperty*, void*)> SetParam)
{
	FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(GetClass(), Dispatcher);
	UFunction* Signature = Property ? Property->SignatureFunction : nullptr;
	FProperty* Param = Signature ? CastField<FProperty>(Signature->ChildProperties) : nullptr;
	if (!Param)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: no dispatcher %s with a parameter"), *GetClass()->GetName(), *Dispatcher.ToString());
		return;
	}
	TArray<uint8> Params;
	Params.SetNumZeroed(Signature->ParmsSize);
	SetParam(Param, Params.GetData());
	Property->ContainerPtrToValuePtr<FMulticastScriptDelegate>(this)->ProcessMulticastDelegate<UObject>(Params.GetData());
}

void UAreaActionsPopupBase::StateClicked(int32 Index, UUserWidget* Button)
{
	const uint8* Value = StateButtons.Find(Button);
	if (!Value || bActed)
	{
		return;
	}
	bActed = true;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Menu: selection mode %d"), *Value);
	bHasResult = true;
	Broadcast(TEXT("BuildStatusSelected"), [Value](FProperty* Param, void* Params)
	{
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Param))
		{
			Enum->GetUnderlyingProperty()->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Params), static_cast<int64>(*Value));
		}
		else if (FNumericProperty* Number = CastField<FNumericProperty>(Param))
		{
			Number->SetIntPropertyValue(Number->ContainerPtrToValuePtr<void>(Params), static_cast<int64>(*Value));
		}
	});
}

void UAreaActionsPopupBase::ActionClicked(int32 Index, UUserWidget* Button)
{
	const TObjectPtr<UClass>* Class = ActionButtons.Find(Button);
	if (!Class || bActed)
	{
		return;
	}
	bActed = true;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Menu: action %s"), *GetNameSafe(*Class));
	bHasResult = true;
	UClass* Value = *Class;
	Broadcast(TEXT("ActionSelected"), [Value](FProperty* Param, void* Params)
	{
		if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Param))
		{
			Object->SetObjectPropertyValue_InContainer(Params, Value);
		}
	});
}

void UAreaActionsPopupBase::FlyClicked()
{
	if (AAreaActionsEquipment* Tool = Cast<AAreaActionsEquipment>(mInteractObject))
	{
		Tool->ToggleFlying();
	}
	UpdateFlyText();
}

void UAreaActionsPopupBase::UpdateFlyText()
{
	const APlayerController* Player = GetOwningPlayer();
	const ACharacter* Character = Player ? Cast<ACharacter>(Player->GetPawn()) : nullptr;
	AreaActionsUI::SetButtonText(FlyWidget, Character && AAreaActionsEquipment::IsFlying(Character) ? LOCTEXT("FlyOff", "Disable flying") : LOCTEXT("FlyOn", "Enable flying"));
}

void UAreaActionsPopupBase::HelpClicked()
{
	ShowHelp();
}

void UAreaActionsPopupBase::ShowHelp()
{
	TutorialStep = 0;
	UpdateTutorial();
}

void UAreaActionsPopupBase::PrevClicked()
{
	if (TutorialStep > 0)
	{
		--TutorialStep;
		UpdateTutorial();
	}
}

void UAreaActionsPopupBase::NextClicked()
{
	const TArray<FText> Messages = ReadArray<FText, FTextProperty>(this, TEXT("HelpMessages"), [](const FTextProperty* Inner, const void* Value) { return Inner->GetPropertyValue(Value); });
	if (TutorialStep == INDEX_NONE)
	{
		return;
	}
	if (Messages.IsValidIndex(TutorialStep + 1))
	{
		++TutorialStep;
		UpdateTutorial();
		return;
	}
	// Done: the help does not show by itself again (the player's settings file, D21)
	TutorialStep = INDEX_NONE;
	UpdateTutorial();
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(this, AAreaActionsTutorialSubsystem::StaticClass(), Found);
	if (AAreaActionsTutorialSubsystem* Tutorial = Found.IsEmpty() ? nullptr : Cast<AAreaActionsTutorialSubsystem>(Found[0]))
	{
		Tutorial->HelpDone();
	}
}

void UAreaActionsPopupBase::UpdateTutorial()
{
	const TArray<FText> Messages = ReadArray<FText, FTextProperty>(this, TEXT("HelpMessages"), [](const FTextProperty* Inner, const void* Value) { return Inner->GetPropertyValue(Value); });
	const bool bShown = Messages.IsValidIndex(TutorialStep);
	if (TutorialBox)
	{
		TutorialBox->SetVisibility(bShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (TutorialTextBlock)
	{
		TutorialTextBlock->SetText(bShown ? Messages[TutorialStep] : FText::GetEmpty());
	}
	AreaActionsUI::SetButtonText(NextWidget, TutorialStep == Messages.Num() - 1 ? ReadText(this, TEXT("DoneText")) : ReadText(this, TEXT("NextText")));
	AreaActionsUI::SetButtonEnabled(PrevWidget, TutorialStep > 0);
	for (int32 Index = 0; Index < Marks.Num() && Index < MarkCount; ++Index)
	{
		const TArray<int32> Steps = ReadArray<int32, FIntProperty>(this, MarkLists[Index], [](const FIntProperty* Inner, const void* Value) { return Inner->GetPropertyValue(Value); });
		const bool bMarked = bShown && Steps.Contains(TutorialStep);
		if (Marks[Index])
		{
			if (bMarked)
			{
				Marks[Index]->SetBrush(FSlateRoundedBoxBrush(FLinearColor::Transparent, 0.0f, AreaActionsUI::Orange, 2.0f));
			}
			else
			{
				Marks[Index]->SetBrush(FSlateNoResource());
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE

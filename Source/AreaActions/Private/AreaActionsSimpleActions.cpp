#include "AreaActionsSimpleActions.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "Buildables/FGBuildable.h"
#include "FGRecipe.h"

#define LOCTEXT_NAMESPACE "AreaActions"

namespace
{
	// "3 Constructors,  2 Assemblers", as the original wrote the counts
	FText CountsText(const FAreaActionsResult& Result)
	{
		TArray<FString> Parts;
		for (const FAreaActionsClassCount& Entry : Result.Counts)
		{
			FText Name = FText::FromString(GetNameSafe(Entry.Class));
			const FTextProperty* DisplayName = Entry.Class ? FindFProperty<FTextProperty>(Entry.Class, TEXT("mDisplayName")) : nullptr;
			if (DisplayName)
			{
				Name = DisplayName->GetPropertyValue_InContainer(Entry.Class->GetDefaultObject());
			}
			Parts.Add(FText::Format(LOCTEXT("ClassCount", "{count} {machine}s"), FFormatNamedArguments{ { TEXT("count"), Entry.Count }, { TEXT("machine"), Name } }).ToString());
		}
		return FText::FromString(FString::Join(Parts, TEXT(",  ")));
	}

	// The window's handlers run also when the tool is put away (the windows close after the action is gone); they do nothing then
	AAreaActionBase* ActiveShell(const UAreaAction* Action)
	{
		AAreaActionBase* Shell = Action->GetShell();
		return Shell && Shell->IsActive() ? Shell : nullptr;
	}
}

void UAreaActionSetRecipe::Start()
{
	AAreaActionBase* Shell = GetShell();
	UUserWidget* Window = Shell->OpenWindow(Shell->ParamsWidgetClass);
	if (!AAreaActionBase::BindWindowEvent(Window, TEXT("OnRecipeSelected"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionSetRecipe, Selected))
		|| !AAreaActionBase::BindWindowEvent(Window, TEXT("WidgetCanceled"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionSetRecipe, Canceled)))
	{
		Shell->Finish();
	}
}

void UAreaActionSetRecipe::Selected(TSubclassOf<UFGRecipe> InRecipe)
{
	AAreaActionBase* Shell = ActiveShell(this);
	UAreaActionsRCO* Rco = Shell ? Shell->GetRco() : nullptr;
	if (!Rco)
	{
		return;
	}
	Recipe = InRecipe;
	Rco->ServerSetRecipe(Shell->BeginRequest(), Shell->GetArea(), InRecipe);
}

void UAreaActionSetRecipe::Canceled()
{
	if (AAreaActionBase* Shell = ActiveShell(this))
	{
		Shell->Finish();
	}
}

void UAreaActionSetRecipe::OnResult(const FAreaActionsResult& Result)
{
	AAreaActionBase* Shell = GetShell();
	if (Shell->ShowFailure(Result.Code))
	{
		return;
	}
	const FText RecipeName = UFGRecipe::GetRecipeName(Recipe);
	switch (Result.Code)
	{
	case EAreaActionsResultCode::NothingInArea:
		Shell->ShowMessage(LOCTEXT("RecipeFailTitle", "Couldn't set recipe!"),
			FText::Format(LOCTEXT("RecipeNoMachine", "No machine in the area accepts {recipe}"), FFormatNamedArguments{ { TEXT("recipe"), RecipeName } }));
		break;
	case EAreaActionsResultCode::AlreadySet:
		Shell->ShowMessage(LOCTEXT("RecipeAlreadyTitle", "Recipes already set"),
			FText::Format(LOCTEXT("RecipeAlready", "All machines in the area already use {recipe}"), FFormatNamedArguments{ { TEXT("recipe"), RecipeName } }));
		break;
	default:
	{
		FText Message = FText::Format(LOCTEXT("RecipeSet", "Set recipe to {recipe} for {machines}"), FFormatNamedArguments{ { TEXT("recipe"), RecipeName }, { TEXT("machines"), CountsText(Result) } });
		if (Result.Items > 0)
		{
			Message = FText::Format(LOCTEXT("RecipeCrate", "{message}. {items} items from the machines are in a crate next to you."),
				FFormatNamedArguments{ { TEXT("message"), Message }, { TEXT("items"), Result.Items } });
		}
		Shell->ShowMessage(LOCTEXT("RecipeSetTitle", "Recipes set!"), Message);
	}
	}
}

void UAreaActionClearInventories::Start()
{
	AAreaActionBase* Shell = GetShell();
	UUserWidget* Window = Shell->OpenWindow(Shell->MessageYesNoClass, LOCTEXT("ClearConfirmTitle", "Confirm Clear Inventories"),
		LOCTEXT("ClearConfirm", "Are you sure you want to clear ALL the inventories the selected area?"));
	if (!AAreaActionBase::BindWindowEvent(Window, TEXT("YesClicked"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionClearInventories, Confirm))
		|| !AAreaActionBase::BindWindowEvent(Window, TEXT("NoClicked"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionClearInventories, Cancel)))
	{
		Shell->Finish();
	}
}

void UAreaActionClearInventories::Confirm()
{
	AAreaActionBase* Shell = ActiveShell(this);
	if (UAreaActionsRCO* Rco = Shell ? Shell->GetRco() : nullptr)
	{
		Rco->ServerClearInventories(Shell->BeginRequest(), Shell->GetArea());
	}
}

void UAreaActionClearInventories::Cancel()
{
	if (AAreaActionBase* Shell = ActiveShell(this))
	{
		Shell->Finish();
	}
}

void UAreaActionClearInventories::OnResult(const FAreaActionsResult& Result)
{
	AAreaActionBase* Shell = GetShell();
	if (!Shell->ShowFailure(Result.Code))
	{
		Shell->ShowMessage(LOCTEXT("ClearTitle", "Clear complete"), FText::Format(LOCTEXT("Cleared", "Cleared {Count} buildings deleting a total of {ItemReturnCount} items."),
			FFormatNamedArguments{ { TEXT("Count"), Result.Buildings }, { TEXT("ItemReturnCount"), Result.Items } }));
	}
}

void UAreaActionSetStationMode::Start()
{
	AAreaActionBase* Shell = GetShell();
	UUserWidget* Window = Shell->OpenWindow(Shell->MessageYesNoClass, LOCTEXT("StationTitle", "Set Station Load/Unload"), LOCTEXT("StationAsk", "Should the stations load?"));
	if (!AAreaActionBase::BindWindowEvent(Window, TEXT("YesClicked"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionSetStationMode, Load))
		|| !AAreaActionBase::BindWindowEvent(Window, TEXT("NoClicked"), this, GET_FUNCTION_NAME_CHECKED(UAreaActionSetStationMode, Unload)))
	{
		Shell->Finish();
	}
}

void UAreaActionSetStationMode::Load()
{
	Send(true);
}

void UAreaActionSetStationMode::Unload()
{
	Send(false);
}

void UAreaActionSetStationMode::Send(bool bInLoad)
{
	AAreaActionBase* Shell = ActiveShell(this);
	if (UAreaActionsRCO* Rco = Shell ? Shell->GetRco() : nullptr)
	{
		bLoad = bInLoad;
		Rco->ServerSetStationMode(Shell->BeginRequest(), Shell->GetArea(), bInLoad);
	}
}

void UAreaActionSetStationMode::OnResult(const FAreaActionsResult& Result)
{
	AAreaActionBase* Shell = GetShell();
	if (Shell->ShowFailure(Result.Code))
	{
		return;
	}
	if (Result.Code == EAreaActionsResultCode::NothingInArea)
	{
		Shell->ShowMessage(LOCTEXT("StationFailTitle", "Couldn't set stations!"), LOCTEXT("NoStations", "No stations in the area."));
		return;
	}
	Shell->ShowMessage(LOCTEXT("StationSetTitle", "Stations set!"), FText::Format(LOCTEXT("StationSet", "Set {machines} to {loadstring}"),
		FFormatNamedArguments{ { TEXT("machines"), CountsText(Result) }, { TEXT("loadstring"), bLoad ? LOCTEXT("Load", "Load") : LOCTEXT("Unload", "Unload") } }));
}

#undef LOCTEXT_NAMESPACE

#include "AreaActionsSimpleActions.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "Buildables/FGBuildable.h"
#include "FGRecipe.h"

#define LOCTEXT_NAMESPACE "AreaActions"

namespace
{
	// "3 Constructors, 2 Assemblers"
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
		return FText::FromString(FString::Join(Parts, TEXT(", ")));
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
	const int32 Id = Shell->BeginRequest();
	if (Id != INDEX_NONE)
	{
		Recipe = InRecipe;
		Rco->ServerSetRecipe(Id, Shell->GetArea(), InRecipe);
	}
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
	if (!Shell->OpenChoice(LOCTEXT("ClearConfirmTitle", "Confirm Clear Inventories"),
		LOCTEXT("ClearAsk", "Clear ALL the inventories in the selected area? The items can be deleted or moved to a crate next to you."),
		LOCTEXT("ClearDelete", "Delete items"), LOCTEXT("ClearToCrate", "Move to crate"),
		this, GET_FUNCTION_NAME_CHECKED(UAreaActionClearInventories, Delete), GET_FUNCTION_NAME_CHECKED(UAreaActionClearInventories, ToCrate)))
	{
		Shell->Finish();
	}
}

void UAreaActionClearInventories::Delete()
{
	Send(false);
}

void UAreaActionClearInventories::ToCrate()
{
	Send(true);
}

void UAreaActionClearInventories::Send(bool bInToCrate)
{
	AAreaActionBase* Shell = ActiveShell(this);
	if (UAreaActionsRCO* Rco = Shell ? Shell->GetRco() : nullptr)
	{
		if (const int32 Id = Shell->BeginRequest(); Id != INDEX_NONE)
		{
			bToCrate = bInToCrate;
			Rco->ServerClearInventories(Id, Shell->GetArea(), bInToCrate);
		}
	}
}
void UAreaActionClearInventories::OnResult(const FAreaActionsResult& Result)
{
	AAreaActionBase* Shell = GetShell();
	if (!Shell->ShowFailure(Result.Code))
	{
		FNumberFormattingOptions OneDecimal;
		OneDecimal.MaximumFractionalDigits = 1;
		const FFormatNamedArguments Arguments{ { TEXT("Count"), Result.Buildings }, { TEXT("ItemReturnCount"), Result.Items },
			{ TEXT("Fluid"), FText::AsNumber(Result.Fluid / 1000.0, &OneDecimal) } };
		FText Message = bToCrate
			? FText::Format(LOCTEXT("ClearedToCrate", "Cleared {Count} buildings taking a total of {ItemReturnCount} items. The solid items are in a crate next to you."), Arguments)
			: FText::Format(LOCTEXT("Cleared", "Cleared {Count} buildings deleting a total of {ItemReturnCount} items."), Arguments);
		if (Result.Fluid > 0)
		{
			// Fluids are shown apart, in m3 as the game shows them (AA-079); they never go into the crate
			Message = FText::Format(LOCTEXT("ClearedFluid", "{Message} {Fluid} m³ of fluids were deleted."),
				FFormatNamedArguments{ { TEXT("Message"), Message }, { TEXT("Fluid"), Arguments[TEXT("Fluid")] } });
		}
		Shell->ShowMessage(LOCTEXT("ClearTitle", "Clear complete"), Message);
	}
}

void UAreaActionSetStationMode::Start()
{
	AAreaActionBase* Shell = GetShell();
	if (!Shell->OpenChoice(LOCTEXT("StationTitle", "Set Station Load/Unload"), LOCTEXT("StationAsk", "What should the stations in the area do?"),
		LOCTEXT("Load", "Load"), LOCTEXT("Unload", "Unload"),
		this, GET_FUNCTION_NAME_CHECKED(UAreaActionSetStationMode, Load), GET_FUNCTION_NAME_CHECKED(UAreaActionSetStationMode, Unload)))
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
		if (const int32 Id = Shell->BeginRequest(); Id != INDEX_NONE)
		{
			bLoad = bInLoad;
			Rco->ServerSetStationMode(Id, Shell->GetArea(), bInLoad);
		}
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

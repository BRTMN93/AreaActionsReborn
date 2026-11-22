#include "AreaActionBase.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "Blueprint/UserWidget.h"
#include "FGCharacterPlayer.h"
#include "TimerManager.h"
#include "UI/FGInteractWidget.h"

#define LOCTEXT_NAMESPACE "AreaActions"

AAreaActionBase* UAreaAction::GetShell() const
{
	return GetTypedOuter<AAreaActionBase>();
}

void AAreaActionBase::BeginPlay()
{
	Super::BeginPlay();
	const APlayerController* OwnerPC = Cast<APlayerController>(GetOwner());
	if (!NativeAction || !OwnerPC || !OwnerPC->IsLocalController())
	{
		return;
	}
	// The tool binds Done and sets its ActiveAction after the spawn returns, so the action starts on the next tick
	GetWorldTimerManager().SetTimerForNextTick(this, &AAreaActionBase::StartNative);
}

void AAreaActionBase::EndPlay(const EEndPlayReason::Type Reason)
{
	bFinished = true;
	if (UAreaActionsRCO* Rco = GetRco(); Rco && ResultHandle.IsValid())
	{
		Rco->OnResult.Remove(ResultHandle);
	}
	Super::EndPlay(Reason);
}

void AAreaActionBase::StartNative()
{
	if (!IsActive())
	{
		return;
	}
	const AAreaActionsEquipment* Equipment = GetEquipment();
	if (!Equipment || !Equipment->GetArea(Area))
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: no tool or no area"), *GetClass()->GetName());
		ShowMessage(LOCTEXT("ErrorTitle", "Error"), LOCTEXT("NoArea", "The tool has no area."));
		return;
	}
	if (UAreaActionsRCO* Rco = GetRco())
	{
		ResultHandle = Rco->OnResult.AddUObject(this, &AAreaActionBase::HandleResult);
	}
	Action = NewObject<UAreaAction>(this, NativeAction);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Action %s started, %d corners"), *GetClass()->GetName(), Area.Polygon.Num());
	Action->Start();
}

int32 AAreaActionBase::BeginRequest()
{
	static int32 LastId = 0;
	RequestId = ++LastId;
	return RequestId;
}

void AAreaActionBase::HandleResult(int32 Id, const FAreaActionsResult& Result)
{
	if (Id == RequestId && IsActive() && Action)
	{
		RequestId = INDEX_NONE;
		Action->OnResult(Result);
	}
}

UAreaActionsRCO* AAreaActionBase::GetRco() const
{
	return UAreaActionsRCO::Of(Cast<APlayerController>(GetOwner()));
}

AAreaActionsEquipment* AAreaActionBase::GetEquipment() const
{
	const APlayerController* OwnerPC = Cast<APlayerController>(GetOwner());
	const AFGCharacterPlayer* Character = OwnerPC ? Cast<AFGCharacterPlayer>(OwnerPC->GetPawn()) : nullptr;
	if (Character)
	{
		for (AFGEquipment* Equipment : Character->GetActiveEquipments())
		{
			if (AAreaActionsEquipment* Tool = Cast<AAreaActionsEquipment>(Equipment))
			{
				return Tool;
			}
		}
	}
	return nullptr;
}

UUserWidget* AAreaActionBase::OpenWindow(TSubclassOf<UUserWidget> Class, const FText& Title, const FText& Message)
{
	APlayerController* OwnerPC = Cast<APlayerController>(GetOwner());
	AAreaActionsEquipment* Equipment = GetEquipment();
	UFunction* AddWidget = Equipment ? Equipment->FindFunction(TEXT("AddWidget")) : nullptr;
	UUserWidget* Window = Class && OwnerPC && AddWidget ? CreateWidget<UUserWidget>(OwnerPC, Class) : nullptr;
	if (!Window)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: cannot open window %s (tool %s)"), *GetClass()->GetName(), *GetNameSafe(Class), *GetNameSafe(Equipment));
		return nullptr;
	}
	if (UFGInteractWidget* Interact = Cast<UFGInteractWidget>(Window))
	{
		Interact->mInteractObject = Equipment;
	}
	// The message windows keep Title and Message as Blueprint strings
	auto SetString = [Window](const TCHAR* Name, const FText& Value)
	{
		if (FStrProperty* Property = FindFProperty<FStrProperty>(Window->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Window, Value.ToString());
		}
	};
	SetString(TEXT("Title"), Title);
	SetString(TEXT("Message"), Message);
	// Equip_AreaActions.AddWidget(Widget): the tool's window stack, input mode and mouse cursor
	TArray<uint8> Params;
	Params.SetNumZeroed(AddWidget->ParmsSize);
	if (FObjectPropertyBase* Param = CastField<FObjectPropertyBase>(AddWidget->ChildProperties))
	{
		Param->SetObjectPropertyValue_InContainer(Params.GetData(), Window);
	}
	Equipment->ProcessEvent(AddWidget, Params.GetData());
	return Window;
}

bool AAreaActionBase::BindWindowEvent(UUserWidget* Window, FName Dispatcher, UObject* Target, FName Function)
{
	FMulticastDelegateProperty* Property = Window ? FindFProperty<FMulticastDelegateProperty>(Window->GetClass(), Dispatcher) : nullptr;
	if (!Property || !Target || !Target->FindFunction(Function))
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("Cannot bind %s.%s to %s.%s"), *GetNameSafe(Window), *Dispatcher.ToString(), *GetNameSafe(Target), *Function.ToString());
		return false;
	}
	FScriptDelegate Delegate;
	Delegate.BindUFunction(Target, Function);
	Property->AddDelegate(MoveTemp(Delegate), Window);
	return true;
}

void AAreaActionBase::ShowMessage(const FText& Title, const FText& Message)
{
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Action %s message '%s': %s"), *GetClass()->GetName(), *Title.ToString(), *Message.ToString());
	UUserWidget* Window = OpenWindow(MessageOkClass, Title, Message);
	if (!Window || !BindWindowEvent(Window, TEXT("OkClicked"), this, GET_FUNCTION_NAME_CHECKED(AAreaActionBase, Finish)))
	{
		Finish();
	}
}

bool AAreaActionBase::ShowFailure(EAreaActionsResultCode Code)
{
	switch (Code)
	{
	case EAreaActionsResultCode::Rejected:
		ShowMessage(LOCTEXT("ErrorTitle", "Error"), LOCTEXT("Rejected", "The server refused the request."));
		return true;
	case EAreaActionsResultCode::Busy:
		ShowMessage(LOCTEXT("ErrorTitle", "Error"), LOCTEXT("Busy", "Another area action is still running."));
		return true;
	default:
		return false;
	}
}

void AAreaActionBase::Finish()
{
	if (!IsActive())
	{
		return;
	}
	bFinished = true;
	// BaseAction.Finished calls Done, the tool's ActionDone destroys this actor
	if (UFunction* Finished = FindFunction(TEXT("Finished")); Finished && Finished->ParmsSize == 0)
	{
		ProcessEvent(Finished, nullptr);
	}
	else
	{
		Destroy();
	}
}

#undef LOCTEXT_NAMESPACE

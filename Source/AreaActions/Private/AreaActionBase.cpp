#include "AreaActionBase.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsLog.h"
#include "AreaActionsRCO.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FGCharacterPlayer.h"
#include "TimerManager.h"
#include "UI/FGInteractWidget.h"

#define LOCTEXT_NAMESPACE "AreaActions"

AAreaActionBase* UAreaAction::GetShell() const
{
	return GetTypedOuter<AAreaActionBase>();
}

AAreaActionBase* UAreaAction::GetActiveShell() const
{
	AAreaActionBase* Shell = GetShell();
	return Shell && Shell->IsActive() ? Shell : nullptr;
}

void AAreaActionBase::BeginPlay()
{
	Super::BeginPlay();
	// An action is its player's own: the host's action must not appear in a client's game (Copy's did). The game's replication graph
	// takes an actor in when it spawns, so turning replication off is not enough: the actor also leaves the network drivers' lists.
	if (HasAuthority())
	{
		SetReplicates(false);
		if (UWorld* World = GetWorld())
		{
			World->RemoveNetworkActor(this);
		}
	}
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
	if (Action)
	{
		Action->End();
	}
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
	// One request per action: a second click on the same button (or a button of a window still closing) is not a second request (AA-081)
	if (RequestCount > 0)
	{
		UE_LOG(LogAreaActionsCpp, Display, TEXT("Action %s: request already sent, click ignored"), *GetClass()->GetName());
		return INDEX_NONE;
	}
	static int32 LastId = 0;
	RequestId = ++LastId;
	++RequestCount;
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
	FObjectPropertyBase* Param = CastField<FObjectPropertyBase>(AddWidget->ChildProperties);
	if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm) || !Window->IsA(Param->PropertyClass))
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s: the tool's AddWidget does not take %s (parameter %s)"), *GetClass()->GetName(), *Window->GetClass()->GetName(),
			Param ? *GetNameSafe(Param->PropertyClass) : TEXT("none"));
		return nullptr;
	}
	TArray<uint8> Params;
	Params.SetNumZeroed(AddWidget->ParmsSize);
	Param->SetObjectPropertyValue_InContainer(Params.GetData(), Window);
	Equipment->ProcessEvent(AddWidget, Params.GetData());
	return Window;
}

bool AAreaActionBase::BindWindowEvent(UUserWidget* Window, FName Dispatcher, UObject* Target, FName Function)
{
	FMulticastDelegateProperty* Property = Window ? FindFProperty<FMulticastDelegateProperty>(Window->GetClass(), Dispatcher) : nullptr;
	const UFunction* Handler = Target ? Target->FindFunction(Function) : nullptr;
	if (!Property || !Handler)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("Cannot bind %s.%s to %s.%s"), *GetNameSafe(Window), *Dispatcher.ToString(), *GetNameSafe(Target), *Function.ToString());
		return false;
	}
	// A handler with other parameters than the dispatcher sends would read garbage from the call (AA-081)
	if (!Property->SignatureFunction || !Handler->IsSignatureCompatibleWith(Property->SignatureFunction))
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("Cannot bind %s.%s to %s.%s: the signatures differ"), *GetNameSafe(Window), *Dispatcher.ToString(), *GetNameSafe(Target), *Function.ToString());
		return false;
	}
	FScriptDelegate Delegate;
	Delegate.BindUFunction(Target, Function);
	Property->AddDelegate(MoveTemp(Delegate), Window);
	return true;
}

bool AAreaActionBase::OpenChoice(const FText& Title, const FText& Message, const FText& First, const FText& Second, UObject* Target, FName OnFirst, FName OnSecond)
{
	UUserWidget* Window = OpenWindow(MessageYesNoClass, Title, Message);
	if (!Window || !BindWindowEvent(Window, TEXT("NoClicked"), this, GET_FUNCTION_NAME_CHECKED(AAreaActionBase, ChoiceClosed)))
	{
		return false;
	}
	RequestCountAtChoice = RequestCount;
	struct FAnswer { const TCHAR* Button; FText Label; FName Function; };
	const FAnswer Answers[] = { { TEXT("YesButton"), First, OnFirst }, { TEXT("NoButton"), Second, OnSecond } };
	for (const FAnswer& Answer : Answers)
	{
		UButton* Button = Cast<UButton>(Window->GetWidgetFromName(Answer.Button));
		if (!Button || !Target || !Target->FindFunction(Answer.Function))
		{
			UE_LOG(LogAreaActionsCpp, Error, TEXT("Cannot bind %s.%s to %s.%s"), *GetNameSafe(Window), Answer.Button, *GetNameSafe(Target), *Answer.Function.ToString());
			return false;
		}
		const FText& Label = Answer.Label;
		UWidgetTree::ForWidgetAndChildren(Button, [&Label](UWidget* Child)
		{
			if (UTextBlock* Text = Cast<UTextBlock>(Child))
			{
				Text->SetText(Label);
			}
		});
		FScriptDelegate Delegate;
		Delegate.BindUFunction(Target, Answer.Function);
		Button->OnClicked.Add(Delegate);
	}
	return true;
}

void AAreaActionBase::ChoiceClosed()
{
	GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		if (RequestCount == RequestCountAtChoice)
		{
			Finish();
		}
	}));
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

void AAreaActionBase::OnLeftClick()
{
	if (IsActive() && Action)
	{
		Action->OnClick(false);
	}
}

void AAreaActionBase::OnRightClick()
{
	if (IsActive() && Action)
	{
		Action->OnClick(true);
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

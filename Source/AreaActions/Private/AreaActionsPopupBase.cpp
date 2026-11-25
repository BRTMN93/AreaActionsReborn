#include "AreaActionsPopupBase.h"
#include "Engine/GameInstance.h"
#include "ModLoading/ModLoadingLibrary.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsLog.h"
#include "Components/Button.h"

#define LOCTEXT_NAMESPACE "AreaActions"

FText UAreaActionsPopupBase::GetTitle() const
{
	FString Version;
	const UGameInstance* Instance = GetGameInstance();
	FModInfo Info;
	if (UModLoadingLibrary* Mods = Instance ? Instance->GetSubsystem<UModLoadingLibrary>() : nullptr; Mods && Mods->GetLoadedModInfo(TEXT("AreaActions"), Info))
	{
		Version = Info.Version.ToString();
	}
	return FText::Format(LOCTEXT("PopupTitle", "Area Actions v{Version}"), FFormatNamedArguments{ { TEXT("Version"), FText::FromString(Version) } });
}

void UAreaActionsPopupBase::NativeConstruct()
{
	Super::NativeConstruct();
	if (UButton* Fly = Cast<UButton>(GetWidgetFromName(TEXT("FlyButton"))))
	{
		Fly->OnClicked.AddUniqueDynamic(this, &UAreaActionsPopupBase::FlyClicked);
	}
	else
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("%s has no FlyButton"), *GetClass()->GetName());
	}
}

void UAreaActionsPopupBase::FlyClicked()
{
	if (AAreaActionsEquipment* Tool = Cast<AAreaActionsEquipment>(mInteractObject))
	{
		Tool->ToggleFlying();
	}
}

#undef LOCTEXT_NAMESPACE

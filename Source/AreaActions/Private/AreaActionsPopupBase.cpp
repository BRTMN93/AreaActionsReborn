#include "AreaActionsPopupBase.h"
#include "Engine/GameInstance.h"
#include "ModLoading/ModLoadingLibrary.h"

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

#undef LOCTEXT_NAMESPACE

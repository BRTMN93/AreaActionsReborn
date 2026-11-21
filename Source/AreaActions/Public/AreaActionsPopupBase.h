#pragma once

#include "CoreMinimal.h"
#include "UI/FGInteractWidget.h"
#include "AreaActionsPopupBase.generated.h"

// Parent of the tool's menu Widget_AreaActionPopup
UCLASS(Abstract)
class AREAACTIONS_API UAreaActionsPopupBase : public UFGInteractWidget
{
	GENERATED_BODY()

public:
	// "Area Actions v<version from the .uplugin>"; the window's title is bound to it by name
	UFUNCTION(BlueprintPure, Category = "Area Actions")
	FText GetTitle() const;
};

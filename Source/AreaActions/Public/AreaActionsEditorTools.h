#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AreaActionsEditorTools.generated.h"

// Editor steps the Python scripts of the editor cannot do themselves (method M1 without the editor window); nothing in the game
UCLASS()
class AREAACTIONS_API UAreaActionsEditorTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Empties a Widget Blueprint's designer tree and its property bindings (its window is built in C++, D23); false outside the editor
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Editor")
	static bool ClearWidgetTree(UObject* WidgetBlueprint);
};

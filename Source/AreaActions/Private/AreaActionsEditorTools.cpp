#include "AreaActionsEditorTools.h"
#include "AreaActionsLog.h"
#if WITH_EDITOR
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#endif

bool UAreaActionsEditorTools::ClearWidgetTree(UObject* WidgetBlueprint)
{
#if WITH_EDITOR
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(WidgetBlueprint);
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		UE_LOG(LogAreaActionsCpp, Error, TEXT("ClearWidgetTree: %s is not a Widget Blueprint"), *GetNameSafe(WidgetBlueprint));
		return false;
	}
	TArray<UWidget*> Widgets;
	Blueprint->WidgetTree->GetAllWidgets(Widgets);
	for (UWidget* Widget : Widgets)
	{
		Blueprint->WidgetTree->RemoveWidget(Widget);
		Widget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}
	Blueprint->WidgetTree->RootWidget = nullptr;
	Blueprint->Bindings.Empty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("ClearWidgetTree: %s, %d widgets removed"), *Blueprint->GetName(), Widgets.Num());
	return true;
#else
	return false;
#endif
}

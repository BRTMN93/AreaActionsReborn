#include "AreaActionsLegacyLibrary.h"
#include "AreaActionsBuildings.h"

void UAreaActionsLegacyLibrary::ShouldIgnore(AActor* Actor, UObject* __WorldContext, bool& Ignore)
{
	Ignore = Actor && AreaActionsBuildings::ShouldIgnore(Actor->GetClass());
}

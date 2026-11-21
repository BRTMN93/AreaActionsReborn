#include "AreaActionsLegacyLibrary.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildablePipeBase.h"
#include "Buildables/FGBuildablePipeHyper.h"
#include "Buildables/FGBuildablePipeHyperAttachment.h"
#include "Buildables/FGBuildablePole.h"
#include "Buildables/FGBuildablePoleStackable.h"
#include "Buildables/FGBuildableRailroadSwitchControl.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableWire.h"
#include "Buildables/FGPipeHyperStart.h"

void UAreaActionsLegacyLibrary::ShouldIgnore(AActor* Actor, UObject* __WorldContext, bool& Ignore)
{
	// The original's list; FGConveyorPoleStackable and FGBuildablePipeHyperPart are gone from the game, their successors are
	// FGBuildablePoleStackable and the hypertube attachments (junction, booster)
	Ignore = Actor && (Actor->IsA<AFGBuildableConveyorBase>() || Actor->IsA<AFGBuildableWire>() || Actor->IsA<AFGBuildableRailroadTrack>()
		|| Actor->IsA<AFGBuildableRailroadSwitchControl>() || Actor->IsA<AFGBuildablePole>() || Actor->IsA<AFGBuildablePipeBase>()
		|| Actor->IsA<AFGBuildablePipeHyper>() || Actor->IsA<AFGBuildablePoleStackable>() || Actor->IsA<AFGPipeHyperStart>()
		|| Actor->IsA<AFGBuildableResourceExtractor>() || Actor->IsA<AFGBuildablePipeHyperAttachment>());
}

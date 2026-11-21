#include "AreaActionsEquipment.h"
#include "AreaActionsLegacyLibrary.h"
#include "Buildables/FGBuildable.h"
#include "InstanceData.h"

bool AAreaActionsEquipment::GetLocalBox(const AFGBuildable* Buildable, FBox& OutBox)
{
	OutBox.Init();
	if (!Buildable)
	{
		return false;
	}
	OutBox = Buildable->GetCombinedClearanceBox();
	if (OutBox.IsValid)
	{
		return true;
	}
	bool bIgnore = false;
	UAreaActionsLegacyLibrary::ShouldIgnore(const_cast<AFGBuildable*>(Buildable), nullptr, bIgnore);
	if (bIgnore)
	{
		return false;
	}
	// The class default stands at the origin, so its bounds are local
	const AActor* Default = Buildable->GetClass()->GetDefaultObject<AActor>();
	FVector Origin, Extent;
	Default->GetActorBounds(true, Origin, Extent, false);
	if (!Extent.IsNearlyZero())
	{
		OutBox += FBox(Origin - Extent, Origin + Extent);
	}
	if (const UAbstractInstanceDataObject* Data = Buildable->GetLightweightInstanceData())
	{
		for (const FInstanceData& Instance : Data->GetInstanceData())
		{
			if (Instance.StaticMesh)
			{
				OutBox += Instance.StaticMesh->GetBoundingBox().TransformBy(Instance.RelativeTransform);
			}
		}
	}
	return OutBox.IsValid != 0;
}

void AAreaActionsEquipment::GetActorCorners(AFGBuildable* Buildable, TArray<FVector>& Corners)
{
	Corners.Reset();
	FBox Box;
	if (!GetLocalBox(Buildable, Box))
	{
		return;
	}
	const FTransform Transform = Buildable->GetActorTransform();
	const FVector Center = Box.GetCenter();
	const FVector Extent = Box.GetExtent();
	// Gray code order, as the Blueprint's GetBoxCorners: neighbours differ in one axis, so the four bottom corners go round
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const int32 Gray = Index ^ (Index >> 1);
		const FVector Sign((Gray & 1) ? -1.0 : 1.0, (Gray & 2) ? -1.0 : 1.0, (Gray & 4) ? -1.0 : 1.0);
		Corners.Add(Transform.TransformPosition(Center + Extent * Sign));
	}
}

#include "AreaActionsTypes.h"

bool FAreaActionsArea::Contains(const FVector& Point) const
{
	if (Point.Z < MinZ || Point.Z > MaxZ || Polygon.Num() < 3)
	{
		return false;
	}
	bool bInside = false;
	for (int32 I = 0, J = Polygon.Num() - 1; I < Polygon.Num(); J = I++)
	{
		const FVector2D& A = Polygon[I];
		const FVector2D& B = Polygon[J];
		if ((A.Y > Point.Y) != (B.Y > Point.Y) && Point.X < (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X)
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

void FAreaActionsResult::AddCount(UClass* Class)
{
	if (FAreaActionsClassCount* Found = Counts.FindByPredicate([Class](const FAreaActionsClassCount& Entry) { return Entry.Class == Class; }))
	{
		++Found->Count;
	}
	else if (Counts.Num() < MaxCounts)
	{
		Counts.Add({ Class, 1 });
	}
}

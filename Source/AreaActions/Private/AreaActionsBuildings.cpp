#include "AreaActionsBuildings.h"
#include "AbstractInstanceManager.h"
#include "AreaActionsEquipment.h"
#include "AreaActionsLog.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableManufacturer.h"
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
#include "Components/InstancedStaticMeshComponent.h"
#include "FGBuildableBeam.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "InstanceData.h"

namespace AreaActionsBuildings
{
	namespace
	{
		// Places closer than this are the same place (cm, degrees)
		constexpr double SameDistance = 1.0;
		constexpr double SameDegrees = 0.1;
		constexpr double CellSize = 50.0;

		FIntVector Cell(const FVector& At)
		{
			return FIntVector(FMath::FloorToInt32(At.X / CellSize), FMath::FloorToInt32(At.Y / CellSize), FMath::FloorToInt32(At.Z / CellSize));
		}

		// Half metres, as the original's RoundTo0HalfOr1
		double RoundHalf(double Value)
		{
			return FMath::RoundHalfFromZero(Value * 2.0) / 2.0;
		}

		TSubclassOf<UFGRecipe> RecipeOf(UWorld* World, const FSnapshot& Building)
		{
			if (Building.BuiltWith)
			{
				return Building.BuiltWith;
			}
			AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
			return Lightweights ? Lightweights->GetBuiltWithRecipeForBuildableClass(Building.Class) : nullptr;
		}

		void FromActor(AFGBuildable* Actor, FSnapshot& Out)
		{
			Out.Class = Actor->GetClass();
			Out.Transform = Actor->GetActorTransform();
			Out.BuiltWith = Actor->GetBuiltWithRecipe();
			Out.Customization = Actor->GetCustomizationData_Native();
			Out.Actor = Actor;
			Out.bIgnored = ShouldIgnore(Out.Class);
			if (const AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Actor))
			{
				Out.MachineRecipe = Machine->GetCurrentRecipe();
			}
			if (const AFGBuildableSplitterSmart* Splitter = Cast<AFGBuildableSplitterSmart>(Actor))
			{
				Out.SortRules = Splitter->GetSortRules();
			}
		}

		void FromLightweight(TSubclassOf<AFGBuildable> Class, int32 Index, const FRuntimeBuildableInstanceData& Data, FSnapshot& Out)
		{
			Out.Class = Class;
			Out.Transform = Data.Transform;
			Out.BuiltWith = Data.BuiltWithRecipe;
			Out.Customization = Data.CustomizationData;
			Out.TypeData = Data.TypeSpecificData;
			Out.LightweightIndex = Index;
			Out.bIgnored = ShouldIgnore(Class);
		}

		const FBuildableBeamLightweightData* BeamData(const FSnapshot& Building)
		{
			return Building.TypeData.IsValid() && Building.TypeData.GetStruct() == FBuildableBeamLightweightData::StaticStruct()
				? static_cast<const FBuildableBeamLightweightData*>(Building.TypeData.GetStructValueRaw()) : nullptr;
		}
	}

	bool ShouldIgnore(const UClass* Class)
	{
		static const UClass* const Skipped[] = { AFGBuildableConveyorBase::StaticClass(), AFGBuildableWire::StaticClass(), AFGBuildableRailroadTrack::StaticClass(),
			AFGBuildableRailroadSwitchControl::StaticClass(), AFGBuildablePole::StaticClass(), AFGBuildablePipeBase::StaticClass(), AFGBuildablePipeHyper::StaticClass(),
			AFGBuildablePoleStackable::StaticClass(), AFGPipeHyperStart::StaticClass(), AFGBuildableResourceExtractor::StaticClass(),
			AFGBuildablePipeHyperAttachment::StaticClass() };
		for (const UClass* Each : Skipped)
		{
			if (Class && Class->IsChildOf(Each))
			{
				return true;
			}
		}
		return false;
	}

	bool IsLightweightClass(const UClass* Class)
	{
		const AFGBuildable* Default = Class ? Class->GetDefaultObject<AFGBuildable>() : nullptr;
		return Default && Default->ManagedByLightweightBuildableSubsystem() && Default->GetLightweightInstanceData();
	}

	bool GetLocalBox(const FSnapshot& Building, FBox& Out)
	{
		if (const AFGBuildable* Actor = Building.Actor.Get())
		{
			return AAreaActionsEquipment::GetLocalBox(Actor, Out);
		}
		Out.Init();
		const AFGBuildable* Default = Building.Class ? Building.Class->GetDefaultObject<AFGBuildable>() : nullptr;
		if (!Default)
		{
			return false;
		}
		if (const FBuildableBeamLightweightData* Beam = BeamData(Building); Beam && Default->IsA<AFGBuildableBeam>())
		{
			// A beam's box grows with its length; the class default has none
			TArray<FFGClearanceData> Clearance;
			static_cast<const AFGBuildableBeam*>(Default)->GetBeamClearanceData(Clearance, Beam->BeamLength);
			for (const FFGClearanceData& Each : Clearance)
			{
				Out += Each.ClearanceBox.TransformBy(Each.RelativeTransform);
			}
			if (Out.IsValid)
			{
				return true;
			}
		}
		Out = Default->GetCombinedClearanceBox();
		if (Out.IsValid)
		{
			return true;
		}
		if (const UAbstractInstanceDataObject* Data = Default->GetLightweightInstanceData())
		{
			for (const FInstanceData& Instance : Data->GetInstanceData())
			{
				if (Instance.StaticMesh)
				{
					Out += Instance.StaticMesh->GetBoundingBox().TransformBy(Instance.RelativeTransform);
				}
			}
		}
		return Out.IsValid != 0;
	}

	bool GetLightweight(UWorld* World, UClass* Class, int32 Index, FSnapshot& Out)
	{
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
		const FRuntimeBuildableInstanceData* Data = Lightweights ? Lightweights->GetRuntimeDataForBuildableClassAndIndex(Class, Index) : nullptr;
		if (!Data || !Data->IsValid())
		{
			return false;
		}
		FromLightweight(Class, Index, *Data, Out);
		return true;
	}

	bool FromHit(UWorld* World, const FHitResult& Hit, FSnapshot& Out)
	{
		if (AFGBuildable* Actor = Cast<AFGBuildable>(Hit.GetActor()))
		{
			FromActor(Actor, Out);
			return true;
		}
		AAbstractInstanceManager* Manager = AAbstractInstanceManager::GetInstanceManager(World);
		FInstanceHandle Handle;
		if (!Manager || !Manager->ResolveHit(Hit, Handle))
		{
			return false;
		}
		if (AFGBuildable* Owner = Cast<AFGBuildable>(AAbstractInstanceManager::GetOwnerByHandle(Handle)))
		{
			FromActor(Owner, Out);
			return true;
		}
		FLightweightBuildableInstanceRef Ref;
		if (!AFGLightweightBuildableSubsystem::ResolveLightweightInstance(Handle, Ref) || !Ref.GetBuildableClass())
		{
			return false;
		}
		Out = FSnapshot();
		Out.Class = Ref.GetBuildableClass();
		Out.Transform = Ref.GetBuildableTransform();
		Out.BuiltWith = Ref.GetBuiltWithRecipe();
		if (const FFGDynamicStruct* TypeData = Ref.GetTypeSpecificData())
		{
			Out.TypeData = *TypeData;
		}
		if (const FFactoryCustomizationData* Customization = Ref.GetCustomizationData())
		{
			Out.Customization = *Customization;
		}
		Out.bIgnored = ShouldIgnore(Out.Class);
		return true;
	}

	void GetCorners(const FSnapshot& Building, TArray<FVector>& Out)
	{
		Out.Reset();
		FBox Box;
		if (!GetLocalBox(Building, Box))
		{
			return;
		}
		const FVector Center = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const int32 Gray = Index ^ (Index >> 1);
			const FVector Sign((Gray & 1) ? -1.0 : 1.0, (Gray & 2) ? -1.0 : 1.0, (Gray & 4) ? -1.0 : 1.0);
			Out.Add(Building.Transform.TransformPosition(Center + Extent * Sign));
		}
	}

	bool FCollector::Step(UWorld* World, const FAreaActionsArea& Area, TArray<FSnapshot>& Out, double EndTime)
	{
		int32 Seen = 0;
		auto TimeUp = [&Seen, EndTime]() { return (++Seen & 255) == 0 && FPlatformTime::Seconds() > EndTime; };
		if (!bActorsDone)
		{
			AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
			// shortcut: a cursor over the live list (as the simple actions); a building removed meanwhile can hide one entry
			static const TArray<AFGBuildable*> None;
			const TArray<AFGBuildable*>& All = Buildables ? Buildables->GetAllBuildablesRef() : None;
			while (ActorCursor < All.Num())
			{
				AFGBuildable* Actor = All[ActorCursor++];
				if (IsValid(Actor) && !Actor->GetIsLightweightTemporary() && Area.Contains(Actor->GetActorLocation()))
				{
					FromActor(Actor, Out.AddDefaulted_GetRef());
				}
				if (TimeUp())
				{
					return false;
				}
			}
			bActorsDone = true;
		}
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
		if (!Lightweights)
		{
			return true;
		}
		const TMap<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Instances = Lightweights->GetAllLightweightBuildableInstances();
		if (!bClassesRead)
		{
			Instances.GetKeys(Classes);
			bClassesRead = true;
		}
		while (ClassCursor < Classes.Num())
		{
			const TArray<FRuntimeBuildableInstanceData>* Array = Instances.Find(Classes[ClassCursor]);
			while (Array && IndexCursor < Array->Num())
			{
				const int32 Index = IndexCursor++;
				const FRuntimeBuildableInstanceData& Data = (*Array)[Index];
				if (Data.IsValid() && Area.Contains(Data.Transform.GetLocation()))
				{
					FromLightweight(Classes[ClassCursor], Index, Data, Out.AddDefaulted_GetRef());
				}
				if (TimeUp())
				{
					return false;
				}
			}
			++ClassCursor;
			IndexCursor = 0;
		}
		return true;
	}

	void CollectAll(UWorld* World, const FAreaActionsArea& Area, TArray<FSnapshot>& Out)
	{
		FCollector Collector;
		while (!Collector.Step(World, Area, Out, TNumericLimits<double>::Max()))
		{
		}
	}

	FFrame ComputeFrame(const TArray<FSnapshot>& Buildings)
	{
		FFrame Frame;
		if (Buildings.IsEmpty())
		{
			return Frame;
		}
		// The middle of the places (the original's AreaCenter), every building counted
		FBox2D Places(ForceInit);
		for (const FSnapshot& Building : Buildings)
		{
			Places += FVector2D(Building.Transform.GetLocation());
		}
		const FVector2D Center = Places.GetCenter();
		// Lowest and highest corner; the most common rotation of the buildings the copy takes, first counted wins a tie
		double MinZ = TNumericLimits<double>::Max(), MaxZ = TNumericLimits<double>::Lowest();
		TMap<int64, int32> Yaws;
		TArray<TArray<FVector>> Corners;
		for (const FSnapshot& Building : Buildings)
		{
			FBox Box;
			TArray<FVector>& World = Corners.AddDefaulted_GetRef();
			if (GetLocalBox(Building, Box))
			{
				for (int32 Index = 0; Index < 8; ++Index)
				{
					const FVector Corner = Building.Transform.TransformPosition(FVector(Index & 1 ? Box.Max.X : Box.Min.X, Index & 2 ? Box.Max.Y : Box.Min.Y, Index & 4 ? Box.Max.Z : Box.Min.Z));
					World.Add(Corner);
					MinZ = FMath::Min(MinZ, Corner.Z);
					MaxZ = FMath::Max(MaxZ, Corner.Z);
				}
			}
			if (!Building.bIgnored)
			{
				++Yaws.FindOrAdd(FMath::RoundToInt64(Building.Transform.Rotator().Yaw * 100.0));
			}
		}
		if (MinZ > MaxZ)
		{
			for (const FSnapshot& Building : Buildings)
			{
				MinZ = FMath::Min(MinZ, Building.Transform.GetLocation().Z);
				MaxZ = FMath::Max(MaxZ, Building.Transform.GetLocation().Z);
			}
		}
		int32 Best = 0;
		for (const TPair<int64, int32>& Entry : Yaws)
		{
			if (Entry.Value > Best)
			{
				Best = Entry.Value;
				Frame.Yaw = FMath::Fmod(Entry.Key / 100.0, 90.0);
			}
		}
		Frame.Position = FVector(Center, MinZ);
		Frame.MaxZ = MaxZ;
		// The pattern's size along the area's axes (Fill)
		const FRotator Axes(0.0, Frame.Yaw, 0.0);
		FBox2D Extent(ForceInit);
		for (const TArray<FVector>& World : Corners)
		{
			for (const FVector& Corner : World)
			{
				Extent += FVector2D(Axes.UnrotateVector(Corner - Frame.Position));
			}
		}
		if (Extent.bIsValid)
		{
			Frame.PatternSize = FVector(RoundHalf((Extent.Max.X - Extent.Min.X) / 100.0), RoundHalf((Extent.Max.Y - Extent.Min.Y) / 100.0), RoundHalf((MaxZ - MinZ) / 100.0)) * 100.0;
		}
		return Frame;
	}

	void FillSteps(int32 Amount, TArray<int32>& Out)
	{
		Out.Reset();
		// The original's AutoForExclusiveEnd(0, Amount): 0 .. Amount-1 up, Amount+1 .. 0 down
		const int32 First = Amount > 0 ? 0 : Amount + 1;
		const int32 Last = Amount > 0 ? Amount - 1 : 0;
		for (int32 Step = First; Step <= Last; ++Step)
		{
			Out.Add(Step);
		}
	}

	void Plan(const TArray<FSnapshot>& Buildings, const FFrame& Frame, const FAreaActionsPlacement& Placement, int64 Limit, TArray<FPlanned>& Out)
	{
		Out.Reset();
		const FQuat Axes(FRotator(0.0, Frame.Yaw, 0.0));
		auto Add = [&Out, Limit](int32 Source, const FTransform& Transform)
		{
			if (Out.Num() <= Limit)
			{
				Out.Add({ Source, Transform });
			}
		};
		if (!Placement.bFill)
		{
			const FVector Pivot = Frame.Position + Placement.RotationCenter;
			const FVector Move = Axes.RotateVector(Placement.Delta);
			const FQuat Turn(FRotator(0.0, Placement.Yaw, 0.0));
			for (int32 Index = 0; Index < Buildings.Num(); ++Index)
			{
				const FSnapshot& Building = Buildings[Index];
				if (!Building.bIgnored)
				{
					FTransform Copy = Building.Transform;
					Copy.SetLocation(Pivot + Move + Turn.RotateVector(Building.Transform.GetLocation() - Pivot));
					Copy.SetRotation(Turn * Building.Transform.GetRotation());
					Add(Index, Copy);
				}
			}
			return;
		}
		TArray<int32> StepsX, StepsY, StepsZ;
		FillSteps(Placement.Amount.X, StepsX);
		FillSteps(Placement.Amount.Y, StepsY);
		FillSteps(Placement.Amount.Z, StepsZ);
		const FVector Pitch = Frame.PatternSize + Placement.Border;
		for (const int32 X : StepsX)
		{
			for (const int32 Y : StepsY)
			{
				for (const int32 Z : StepsZ)
				{
					if (X == 0 && Y == 0 && Z == 0)
					{
						continue;
					}
					FVector Offset = Pitch * FVector(X, Y, Z);
					Offset.Z += FMath::Abs(X) * Placement.Ramp.X + FMath::Abs(Y) * Placement.Ramp.Y;
					const FVector Move = Axes.RotateVector(Offset);
					for (int32 Index = 0; Index < Buildings.Num(); ++Index)
					{
						if (!Buildings[Index].bIgnored)
						{
							FTransform Copy = Buildings[Index].Transform;
							Copy.AddToTranslation(Move);
							Add(Index, Copy);
						}
					}
					if (Out.Num() > Limit)
					{
						return;
					}
				}
			}
		}
	}

	void FExisting::Build(UWorld* World, const TSet<UClass*>& Classes)
	{
		Places.Reset();
		if (AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World))
		{
			for (const AFGBuildable* Actor : Buildables->GetAllBuildablesRef())
			{
				if (IsValid(Actor) && Classes.Contains(Actor->GetClass()))
				{
					Add(Actor->GetClass(), Actor->GetActorTransform());
				}
			}
		}
		if (AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World))
		{
			for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Entry : Lightweights->GetAllLightweightBuildableInstances())
			{
				if (!Classes.Contains(Entry.Key.Get()))
				{
					continue;
				}
				for (const FRuntimeBuildableInstanceData& Data : Entry.Value)
				{
					if (Data.IsValid())
					{
						Add(Entry.Key.Get(), Data.Transform);
					}
				}
			}
		}
	}

	void FExisting::Add(const UClass* Class, const FTransform& Transform)
	{
		Places.FindOrAdd(Class).Add(Cell(Transform.GetLocation()), Transform);
	}

	bool FExisting::Contains(const UClass* Class, const FTransform& Transform) const
	{
		const TMultiMap<FIntVector, FTransform>* OfClass = Places.Find(Class);
		if (!OfClass)
		{
			return false;
		}
		const FIntVector Center = Cell(Transform.GetLocation());
		TArray<const FTransform*> Found;
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 Z = -1; Z <= 1; ++Z)
				{
					Found.Reset();
					OfClass->MultiFindPointer(Center + FIntVector(X, Y, Z), Found);
					for (const FTransform* Other : Found)
					{
						if (FVector::Dist(Other->GetLocation(), Transform.GetLocation()) <= SameDistance
							&& FMath::RadiansToDegrees(Other->GetRotation().AngularDistance(Transform.GetRotation())) <= SameDegrees)
						{
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	void AddCost(UWorld* World, const FSnapshot& Building, FItemCounts& InOut)
	{
		const TSubclassOf<UFGRecipe> Recipe = RecipeOf(World, Building);
		if (!Recipe)
		{
			return;
		}
		int32 Times = 1;
		if (const FBuildableBeamLightweightData* Beam = BeamData(Building))
		{
			if (const AFGBuildableBeam* Default = Cast<AFGBuildableBeam>(Building.Class->GetDefaultObject()))
			{
				Times = FMath::Max(1, Default->GetDismantleReturnsMultiplierForBeam(Beam->BeamLength));
			}
		}
		for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(World, Recipe))
		{
			InOut.FindOrAdd(Ingredient.ItemClass) += Ingredient.Amount * Times;
		}
	}

	bool Spawn(UWorld* World, const FSnapshot& Building, const FTransform& Transform)
	{
		const TSubclassOf<UFGRecipe> Recipe = RecipeOf(World, Building);
		if (IsLightweightClass(Building.Class))
		{
			AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
			if (!Lightweights || !Recipe)
			{
				return false;
			}
			FRuntimeBuildableInstanceData Data;
			Data.Transform = Transform;
			Data.CustomizationData = Building.Customization;
			Data.BuiltWithRecipe = Recipe;
			Data.TypeSpecificData = Building.TypeData;
			return Lightweights->AddFromBuildableInstanceData(Building.Class, Data) != INDEX_NONE;
		}
		AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
		AFGBuildable* Actor = Buildables ? Buildables->BeginSpawnBuildable(Building.Class, Transform) : nullptr;
		if (!Actor)
		{
			return false;
		}
		Actor->SetBuiltWithRecipe(Recipe);
		Actor->SetCustomizationData_Native(Building.Customization);
		Actor->FinishSpawning(Transform);
		if (AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Actor); Machine && Building.MachineRecipe)
		{
			Machine->SetRecipe(Building.MachineRecipe);
		}
		if (AFGBuildableSplitterSmart* Splitter = Cast<AFGBuildableSplitterSmart>(Actor); Splitter && Building.SortRules.Num() > 0)
		{
			Splitter->SetSortRules(Building.SortRules);
		}
		return true;
	}

	void GetMeshes(UWorld* World, const FSnapshot& Building, TArray<TPair<UStaticMesh*, FTransform>>& Out)
	{
		Out.Reset();
		auto AddHandle = [&Out, &Building](const FInstanceHandle* Handle)
		{
			const UInstancedStaticMeshComponent* Component = Handle ? Handle->GetInstanceComponent() : nullptr;
			if (Component && Component->GetStaticMesh())
			{
				Out.Emplace(Component->GetStaticMesh(), Handle->GetWorldTransform().GetRelativeTransform(Building.Transform));
			}
		};
		if (const AFGBuildable* Actor = Building.Actor.Get())
		{
			for (const FInstanceHandlePtr& Handle : Actor->GetLightweightInstanceHandles().HandleArray)
			{
				AddHandle(Handle.Get());
			}
			TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
			for (const UStaticMeshComponent* Mesh : Meshes)
			{
				if (Mesh && Mesh->GetStaticMesh() && Mesh->IsVisible() && !Mesh->IsA<UInstancedStaticMeshComponent>())
				{
					Out.Emplace(Mesh->GetStaticMesh(), Mesh->GetComponentTransform().GetRelativeTransform(Building.Transform));
				}
			}
			return;
		}
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
		const FRuntimeBuildableInstanceData* Data = Lightweights ? Lightweights->GetRuntimeDataForBuildableClassAndIndex(Building.Class, Building.LightweightIndex) : nullptr;
		if (Data)
		{
			for (const FInstanceOwnerHandlePtr& Handle : Data->Handles)
			{
				AddHandle(Handle.Get());
			}
		}
	}
}

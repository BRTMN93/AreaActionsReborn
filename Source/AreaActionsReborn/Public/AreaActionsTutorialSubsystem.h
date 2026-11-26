#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FGSaveInterface.h"
#include "AreaActionsTutorialSubsystem.generated.h"

// Parent of AreaActionsTutorialSubsystem: whether the tool's help shows by itself. The original's ADA-style messages are gone from the
// game's API and are not ported (AA-060). Whether the player saw the help to its end is kept in the player's settings file
// (AreaActionsSettings, decision D21), so a client keeps it too; the flags here are still written, for the Blueprint and for saves made before.
UCLASS(Abstract)
class AREAACTIONSREBORN_API AAreaActionsTutorialSubsystem : public AActor, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	AAreaActionsTutorialSubsystem();

	virtual bool ShouldSave_Implementation() const override { return true; }
	virtual bool NeedTransform_Implementation() override { return false; }

	// Called when the tool is equipped: the help shows by itself until the player has seen it to the end
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void OnEquip(bool& ShowHelp);

	// The player finished the help
	UFUNCTION(BlueprintCallable, Category = "Area Actions")
	void HelpDone();

	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Area Actions")
	bool HasDoneFirstEquip = false;
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Area Actions")
	bool HasSeenHelp = false;
};

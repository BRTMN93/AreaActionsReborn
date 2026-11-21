#include "AreaActionsTutorialSubsystem.h"

void AAreaActionsTutorialSubsystem::OnEquip(bool& ShowHelp)
{
	ShowHelp = !HasDoneFirstEquip || !HasSeenHelp;
	HasDoneFirstEquip = true;
}

void AAreaActionsTutorialSubsystem::HelpDone()
{
	HasSeenHelp = true;
}

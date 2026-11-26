#pragma once

#include "CoreMinimal.h"

// The player's own settings (the help seen to its end, "Use build cost", the container distance), kept in a file on the player's computer:
// the same in a game alone, at a host and at a client, never in a save (decision D21). The test mode uses a file of its own, emptied when
// a scenario starts.
namespace AreaActionsSettings
{
	bool GetBool(const TCHAR* Key, bool bDefault);
	void SetBool(const TCHAR* Key, bool bValue);
	double GetDouble(const TCHAR* Key, double Default);
	void SetDouble(const TCHAR* Key, double Value);
	// The file in use
	FString GetPath();
#if AA_WITH_TESTS
	void ResetForTest();
#endif
}

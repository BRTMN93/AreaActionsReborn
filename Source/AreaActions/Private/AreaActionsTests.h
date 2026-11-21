#pragma once

#include "CoreMinimal.h"

#if AA_WITH_TESTS

class UWorld;

// Developer test mode, started by -AATest=<Scenario>. Without the argument nothing here runs or logs.
// Lines: "AATEST <scenario> step <text>", "AATEST <scenario> check PASS|FAIL <text>", at the end "AATEST RESULT <scenario> PASS|FAIL <passed>/<total>".
// The game then closes by itself, unless -AATestStay is given. narzedzia\run_scenario.ps1 reads the result.
class FAreaActionsTests
{
public:
	// Once per game process, in the first game world
	static void StartFromCommandLine(UWorld* World);
};

#endif

#pragma once

#include "CoreMinimal.h"

// Not LogAreaActions: the mod's Blueprint log nodes already use that name, and a C++ category of the same name crashes the game at start
AREAACTIONS_API DECLARE_LOG_CATEGORY_EXTERN(LogAreaActionsCpp, Log, All);

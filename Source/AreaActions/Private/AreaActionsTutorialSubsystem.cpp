#include "AreaActionsTutorialSubsystem.h"
#include "AreaActionsSettings.h"
#include "AreaActionsLog.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace AreaActionsSettings
{
	static const TCHAR* Section = TEXT("AreaActions");
	static const TCHAR* HelpDoneKey = TEXT("HelpDone");

	FString GetPath()
	{
		FString Name = TEXT("Settings");
#if AA_WITH_TESTS
		// Two test games on one computer each keep their own file
		FString Scenario, Role;
		if (FParse::Value(FCommandLine::Get(), TEXT("AATest="), Scenario))
		{
			Name = FParse::Value(FCommandLine::Get(), TEXT("AATestRole="), Role) ? FString::Printf(TEXT("Settings_Test_%s"), *Role) : TEXT("Settings_Test");
		}
#endif
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AreaActions"), Name + TEXT(".ini"));
	}

	static FConfigFile& File()
	{
		static FConfigFile Cached;
		static bool bRead = false;
		if (!bRead)
		{
			bRead = true;
			Cached.Read(GetPath());
		}
		return Cached;
	}

	static void Write()
	{
		const FString Path = GetPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		// FConfigFile::Write skips the disk under -Multiprocess (two games on one computer), so the text is saved here
		FString Text;
		File().WriteToString(Text, Path);
		if (!FFileHelper::SaveStringToFile(Text, *Path))
		{
			UE_LOG(LogAreaActionsCpp, Warning, TEXT("settings file %s not saved"), *Path);
		}
	}

	bool GetBool(const TCHAR* Key, bool bDefault)
	{
		bool bValue = bDefault;
		File().GetBool(Section, Key, bValue);
		return bValue;
	}

	void SetBool(const TCHAR* Key, bool bValue)
	{
		if (GetBool(Key, !bValue) != bValue)
		{
			File().SetBool(Section, Key, bValue);
			Write();
		}
	}

	double GetDouble(const TCHAR* Key, double Default)
	{
		double Value = Default;
		File().GetDouble(Section, Key, Value);
		return FMath::IsFinite(Value) ? Value : Default;
	}

	void SetDouble(const TCHAR* Key, double Value)
	{
		if (GetDouble(Key, Value + 1.0) != Value)
		{
			File().SetDouble(Section, Key, Value);
			Write();
		}
	}

#if AA_WITH_TESTS
	void ResetForTest()
	{
		IFileManager::Get().Delete(*GetPath(), false, true, true);
		File().Empty();
	}
#endif
}

AAreaActionsTutorialSubsystem::AAreaActionsTutorialSubsystem()
{
	// The world module spawns it on the server only; the tool and the menu find it in every game, a client's too, and the calls below
	// touch only that game's settings file
	bReplicates = true;
	bAlwaysRelevant = true;
}

void AAreaActionsTutorialSubsystem::OnEquip(bool& ShowHelp)
{
	// The help shows by itself until the player saw it to its end, in any save (D21; the original kept it in the save)
	ShowHelp = !AreaActionsSettings::GetBool(AreaActionsSettings::HelpDoneKey, false);
	HasDoneFirstEquip = true;
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Tutorial OnEquip: help shows by itself %d"), ShowHelp ? 1 : 0);
}

void AAreaActionsTutorialSubsystem::HelpDone()
{
	HasSeenHelp = true;
	AreaActionsSettings::SetBool(AreaActionsSettings::HelpDoneKey, true);
	UE_LOG(LogAreaActionsCpp, Display, TEXT("Tutorial HelpDone"));
}

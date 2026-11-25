using UnrealBuildTool;

public class AreaActions : ModuleRules
{
	public AreaActions(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// FactoryGame transitive dependencies, as in the Alpakit C++ template
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			"GeometryCollectionEngine",
			"AnimGraphRuntime",
			"AssetRegistry",
			"NavigationSystem",
			"AIModule",
			"GameplayTasks",
			"SlateCore", "Slate", "UMG",
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			"NetCore",
			"GameplayTags",
			"EnhancedInput",
			"Projects",
			"DummyHeaders",
		});
		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.Add("AnimGraph");
			// UAreaActionsEditorTools (editor scripts only)
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "UMGEditor" });
		}

		// AbstractInstance: the lightweight instance data. UBT warns that the .uplugin does not list the plugin; the game ships it inside
		// FactoryGame, so it stays out of the .uplugin.
		PublicDependencyModuleNames.AddRange(new string[] { "FactoryGame", "SML", "AbstractInstance" });

		// Developer test mode (-AATest=<Scenario>). The release gate decides whether a release build keeps it.
		bool bWithTests = true;
		if (bWithTests) {
			PrivateDefinitions.Add("AA_WITH_TESTS=1");
			// The test mode's client joins a dedicated server through the game's Server Manager; the server target has no client module
			if (Target.Type != TargetRules.TargetType.Server) {
				PrivateDependencyModuleNames.Add("FactoryDedicatedClient");
			}
			// ... and the test mode's dedicated server keeps its own auto-load session after loading a test save
			if (Target.Type == TargetRules.TargetType.Server) {
				PrivateDependencyModuleNames.Add("FactoryDedicatedServer");
			}
		} else {
			PrivateDefinitions.Add("AA_WITH_TESTS=0");
		}
		// The preview stops its holograms' sound components (Wwise)
		PrivateDependencyModuleNames.Add("AkAudio");
	}
}

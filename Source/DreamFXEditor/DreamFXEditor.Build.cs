using UnrealBuildTool;

public class DreamFXEditor : ModuleRules
{
	public DreamFXEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"AssetTools",
				"Core",
				"CoreUObject",
				"DreamFX",
				"Engine",
				"Niagara",
				"NiagaraEditor",
				"Projects",
				"UnrealEd"
			});
	}
}

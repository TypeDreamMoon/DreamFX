using UnrealBuildTool;

public class DreamFXEditor : ModuleRules
{
	public DreamFXEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Private/ is on the include path so the layered folders (Adapter, Schema, Generation, ...)
		// can include each other by their folder-qualified path.
		PrivateIncludePaths.Add(ModuleDirectory + "/Private");

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"AssetTools",
				"Core",
				"CoreUObject",
				"DreamFX",
				"Engine",
				"Json",
				"JsonUtilities",
				"Niagara",
				"NiagaraEditor",
				"Projects",
				"Slate",
				"SlateCore",
				"UnrealEd"
			});
	}
}

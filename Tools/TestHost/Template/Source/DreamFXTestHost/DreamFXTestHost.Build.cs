// Copyright (c) 2026 TypeDreamMoon. All rights reserved.

using UnrealBuildTool;

/*
 * The test host's only module, and it is empty.
 *
 * A project needs a primary game module to be a code project, and a code project is what gives the host
 * its own DreamFXTestHostEditor target -- which is what builds the DreamFX plugin sitting in Plugins/.
 *
 * It depends on nothing beyond the engine core on purpose: a dependency here would be a second, silent way
 * for the plugin's own dependencies to be satisfied, and the host exists partly to prove the plugin
 * declares everything it uses.
 */
public class DreamFXTestHost : ModuleRules
{
	public DreamFXTestHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}

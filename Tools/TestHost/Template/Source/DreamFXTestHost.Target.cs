// Copyright (c) 2026 TypeDreamMoon. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/*
 * The game target of the DreamFX test host.
 *
 * Nothing builds this one: DreamFX's gate runs entirely in the editor (commandlets and automation tests).
 * It exists so the host is an ordinary, complete project whose project files generate.
 *
 * Do not build it casually. A game target against a source engine is monolithic, and a monolithic target
 * defaults to a UNIQUE build environment, which compiles the whole engine again into this project.
 */
public class DreamFXTestHostTarget : TargetRules
{
	public DreamFXTestHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("DreamFXTestHost");
	}
}

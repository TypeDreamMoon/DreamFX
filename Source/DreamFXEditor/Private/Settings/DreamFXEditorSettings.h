#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "DreamFXEditorSettings.generated.h"

/**
 * The editor-side knobs: everything the headless pipeline does not need.
 *
 * Config=Editor rather than Config=Game on purpose -- none of this changes what a build produces, so
 * none of it belongs in a file the runtime reads. It lands in Project Settings > Plugins > DreamFX.
 */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "DreamFX"))
class UDreamFXEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Whether *Open DreamFX Workspace* asks VSCode for a new window.
	 *
	 * Only the workspace launcher reads it. Opening a single file always reuses the window -- a new
	 * window per diagnostic jump would be unusable, and that is the same split DreamShader shipped.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Workspace",
		meta = (DisplayName = "Open Workspace In New Window"))
	bool bOpenWorkspaceInNewWindow = true;

	/**
	 * Where *Export .dfs* writes, relative to the project directory.
	 *
	 * Ordinary source, watched and built like the rest of `DFX/` (plan-v4 V1). What keeps an export
	 * from behaving like *Adopt* is not where it sits but what it names: its `Name=` points into the
	 * `Decompiled/` content namespace, so a build writes a mirror and never the asset it was read
	 * from. Files here that predate that rule are refused with DFX8013 rather than obeyed.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Decompiler",
		meta = (DisplayName = "Decompiled Output Directory"))
	FString DecompiledOutputDirectory = TEXT("DFX/Decompiled");

	/**
	 * How many files a *startup* batch may hold before the watcher stops building it automatically.
	 *
	 * The directory watcher reports everything that changed while the editor was shut down, all at
	 * once, as if it had just been saved. After a re-export that is the whole source tree: 24 systems
	 * rebuilt concurrently put 237 jobs in the Niagara compile queue and the editor did not survive
	 * it. Past this many, the watcher offers the rebuild instead of starting it.
	 *
	 * Only the startup backlog is gated. Saving a file while the editor is open still rebuilds
	 * immediately however many files the save touches, which is what makes the iteration loop work.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Workspace", meta = (ClampMin = "1",
		DisplayName = "Startup Rebuild Threshold"))
	int32 StartupRebuildThreshold = 8;
};

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"

#include "DreamFXSourceSettings.generated.h"

/**
 * Where the project's DreamFX sources live.
 *
 * Its own class, in its own ini, rather than a field on UDreamFXEditorSettings: every Dream plugin keeps its source
 * directory in `Config/DefaultDreamSource.ini`, one section each, so that Project Settings shows them together under
 * "Dream Source" and the VSCode extensions -- which cannot ask the engine -- have one file to read. The section
 * (`[/Script/DreamFXEditor.DreamFXSourceSettings]`) and the key are a contract with dreamfx-language-support.
 *
 * Never read directly: FDreamFXPaths::ResolveProjectSourceDirectory applies the rest of the rule -- the legacy `DFX/`
 * directory still answers while the configured one does not exist.
 */
UCLASS(Config = DreamSource, DefaultConfig, meta = (DisplayName = "FX"))
class UDreamFXSourceSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UDreamFXSourceSettings();

	/**
	 * The directory holding the project's .dfs/.dfe/.dfm files, relative to the project directory. A plugin's own
	 * sources are not configurable: they live in `<Plugin>/DreamSource/FX`. Takes effect after an editor restart --
	 * the source watcher is set up at startup.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Source", meta = (RelativeToGameDir))
	FDirectoryPath SourceDirectory;

	/** The default value, `DreamSource/FX`. */
	static const TCHAR* GetDefaultSourceDirectory() { return TEXT("DreamSource/FX"); }

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Dream Source"); }
	virtual FName GetSectionName() const override { return TEXT("FX"); }
};

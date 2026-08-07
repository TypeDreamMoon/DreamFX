#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "DreamFXCommandlet.generated.h"

/**
 * Headless DreamFX build.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=DreamFX [options]
 *
 *   -File=<path>     build one source file (absolute, or relative to the project directory)
 *   -All             build every .dfs under every DFX root (the default when -File is absent)
 *   -Verify          check only: report drift between source and asset, write nothing
 *   -Lint            run the static checks only; no asset access, no generation
 *   -Force           rebuild even when the provenance hash says the asset is current
 *   -NoSave          build in memory without writing packages (used by tests)
 *   -Schema=<name>   print a module's live input signature; -Stack= picks which stack to probe in
 *   -ListModules     print every module the search paths expose (-ListDynamicInputs for the others)
 *
 * Exit code is the error count, so a CI gate can just test for zero.
 */
UCLASS()
class UDreamFXCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDreamFXCommandlet();

	virtual int32 Main(const FString& Params) override;
};

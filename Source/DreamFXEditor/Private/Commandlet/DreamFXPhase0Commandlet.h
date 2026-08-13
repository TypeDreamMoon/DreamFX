#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DreamFXPhase0Commandlet.generated.h"

/**
 * Phase 0 feasibility nail.
 *
 * Answers R1 empirically: can UNiagaraExternalEditUtilities drive a Niagara System end to end
 * from a headless commandlet, where FNiagaraExternalEditContext's internally-held
 * FNiagaraSystemViewModel has no editor UI to attach to?
 *
 * Deliberately contains NO DreamFX language logic. It only exercises the engine API surface the
 * generator backend would sit on, and reports one PASS/FAIL line per probe.
 *
 * Run:
 *   UnrealEditor-Cmd.exe <project>.uproject -run=DreamFXPhase0 -unattended -nopause -nosplash
 *
 * Optional switches:
 *   -Path=/Game/DreamFX_Phase0   package path for the generated system
 *   -Name=NS_DreamFXPhase0       asset name
 *   -Save                        save the generated package to disk
 */
UCLASS()
class UDreamFXPhase0Commandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDreamFXPhase0Commandlet();

	virtual int32 Main(const FString& Params) override;
};

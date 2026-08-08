#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Where DreamFX attaches itself to the editor UI (plan-v3 E1-E3).
	 *
	 * Everything goes through `UToolMenus` and per-class asset context menus -- public mechanisms only,
	 * no NiagaraEditor source is touched. Registration is idempotent and skipped entirely under a
	 * commandlet or `-NoDreamFXEditor`.
	 */
	class FDreamFXMenus
	{
	public:
		static void Register();
		static void Unregister();
	};
}

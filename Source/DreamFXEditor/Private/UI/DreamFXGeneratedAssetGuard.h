#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/**
	 * Plan 4.6-3, the third leg of the anti-drift tripod.
	 *
	 * "Text is the only truth" is a mechanism, not an agreement. The provenance stamp and `-verify`
	 * catch drift in CI; this catches it at the moment someone is about to create it, by telling them
	 * the asset they just opened is generated and that hand edits will not survive the next build.
	 *
	 * Deliberately does not lock anything. The escape hatch has to stay open -- someone will need to
	 * poke at a generated asset to debug it. What must not happen is drifting *without noticing*.
	 */
	class FGeneratedAssetGuard
	{
	public:
		static void Register();
		static void Unregister();
	};
}

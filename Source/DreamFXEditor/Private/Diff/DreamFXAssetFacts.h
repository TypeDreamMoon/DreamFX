#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;

namespace UE::DreamFX::Editor
{
	/**
	 * Every comparable fact about one Niagara system, unsorted; the caller compares as a multiset.
	 *
	 * The house rule's enforcer (plan.md design principle 5): proving anything about an asset takes
	 * asset-level evidence, and before this existed the only routine comparison channel WAS the
	 * export -- both sides of an export-vs-export diff are the same lossy exporter's output, so a
	 * loss on the export side is structurally invisible there. This walks the assets themselves by
	 * reflection: what one side has that the other does not, never "first difference".
	 *
	 * It lives here rather than inside the commandlet because two callers need it, and for the same
	 * reason in both cases. `-AssetDiff` compares an authored original against its mirror; the
	 * round-trip corpus compares the asset built from a fixture against the asset rebuilt from that
	 * fixture's export -- and a corpus test that compared only the two export TEXTS could never see
	 * a loss the exporter makes on both sides, which is the exact shape of every curve tangent and
	 * every stage binding that went missing.
	 *
	 * Facts naming the compiler's own view of the scripts (the `compiled ...` family) are only as
	 * good as the last compile: PostLoad discards a cached VM whose stored id does not match its
	 * graph, so a system that has not been compiled in this session reports no stages, no data
	 * interfaces and no written attributes. Compile both sides before comparing them.
	 */
	void DescribeSystemFacts(UNiagaraSystem* System, TArray<FString>& OutFacts);
}

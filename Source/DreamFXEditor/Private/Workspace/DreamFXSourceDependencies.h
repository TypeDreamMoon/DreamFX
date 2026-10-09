#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/** Direct .dfe dependencies, resolved by the same rules used when generating a system. */
	class FSourceDependencyIndex
	{
	public:
		void Refresh(const TArray<FString>& SourceFiles);
		void FindDependents(const TArray<FString>& ChangedFiles, TSet<FString>& OutSources) const;
		void Reset() { DependenciesBySource.Reset(); }

	private:
		TMap<FString, TArray<FString>> DependenciesBySource;
	};
}

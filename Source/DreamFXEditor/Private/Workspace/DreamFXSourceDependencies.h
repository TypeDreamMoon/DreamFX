#pragma once

#include "CoreMinimal.h"

namespace UE::DreamFX::Editor
{
	/** .dfe and System parent dependencies, including transitive source-parent chains. */
	class FSourceDependencyIndex
	{
	public:
		void Refresh(const TArray<FString>& SourceFiles);
		void FindDependents(const TArray<FString>& ChangedFiles, TSet<FString>& OutSources) const;
		void Reset() { DependenciesBySource.Reset(); TrackedSources.Reset(); }

	private:
		TMap<FString, TArray<FString>> DependenciesBySource;
		TSet<FString> TrackedSources;
	};
}

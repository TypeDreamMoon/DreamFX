#include "Workspace/DreamFXSourceDependencies.h"

#include "DreamFXParser.h"
#include "Misc/Paths.h"
#include "SourceFiles/DreamFXPaths.h"

namespace UE::DreamFX::Editor
{
	void FSourceDependencyIndex::Refresh(const TArray<FString>& SourceFiles)
	{
		DependenciesBySource.Reset();
		for (const FString& SourceFile : SourceFiles)
		{
			if (!FPaths::GetExtension(SourceFile).Equals(TEXT("dfs"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FDocument Document;
			FDiagnosticSink Diagnostics;
			if (!FParser::ParseFile(SourceFile, Document, Diagnostics))
			{
				continue; // The build of this source reports its parse error.
			}
			TArray<FString>& Dependencies = DependenciesBySource.FindOrAdd(SourceFile);
			for (const FEmitter& Emitter : Document.Emitters)
			{
				FString ReferencedFile, Error;
				if (!Emitter.FromPath.IsEmpty() && FDreamFXPaths::ResolveSourceReference(
					Emitter.FromPath, SourceFile, TEXT(".dfe"), ReferencedFile, Error))
				{
					Dependencies.AddUnique(ReferencedFile);
				}
			}
		}
	}

	void FSourceDependencyIndex::FindDependents(const TArray<FString>& ChangedFiles, TSet<FString>& OutSources) const
	{
		for (const TPair<FString, TArray<FString>>& Source : DependenciesBySource)
		{
			for (const FString& Dependency : Source.Value)
			{
				if (ChangedFiles.ContainsByPredicate([&Dependency](const FString& Changed)
					{ return FPaths::IsSamePath(Dependency, Changed); }))
				{
					OutSources.Add(Source.Key);
					break;
				}
			}
		}
	}
}

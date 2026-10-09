#include "Workspace/DreamFXSourceDependencies.h"

#include "DreamFXParser.h"
#include "Misc/Paths.h"
#include "SourceFiles/DreamFXPaths.h"

namespace UE::DreamFX::Editor
{
	void FSourceDependencyIndex::Refresh(const TArray<FString>& SourceFiles)
	{
		Reset();
		TArray<FString> Pending;
		for (const FString& File : SourceFiles)
		{
			if (FPaths::GetExtension(File).Equals(TEXT("dfs"), ESearchCase::IgnoreCase))
			{
				const FString FullPath = FPaths::ConvertRelativePathToFull(File);
				TrackedSources.Add(FullPath);
				Pending.AddUnique(FullPath);
			}
		}
		TArray<FString> Visited;
		while (!Pending.IsEmpty())
		{
			const FString SourceFile = Pending.Pop();
			if (Visited.ContainsByPredicate([&SourceFile](const FString& File) { return FPaths::IsSamePath(File, SourceFile); }))
			{
				continue;
			}
			Visited.Add(SourceFile);
			FDocument Document;
			FDiagnosticSink Diagnostics;
			if (!FParser::ParseFile(SourceFile, Document, Diagnostics))
			{
				continue; // The build of this source reports its parse error.
			}
			TArray<FString>& Dependencies = DependenciesBySource.FindOrAdd(SourceFile);
			if (!Document.ParentPath.IsEmpty())
			{
				FString ParentFile, Error;
				if (FDreamFXPaths::ResolveSourceReference(Document.ParentPath, SourceFile, TEXT(".dfs"), ParentFile, Error))
				{
					Dependencies.AddUnique(ParentFile);
					Pending.Add(ParentFile);
				}
			}
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
		// Reach a fixed point so a .dfe edit also reaches grandchildren through a .dfs parent.
		// Keep this separate from source parsing: the old graph still works after a file is deleted.
		TArray<FString> Invalidated = ChangedFiles;
		bool bAdded;
		do
		{
			bAdded = false;
			for (const TPair<FString, TArray<FString>>& Source : DependenciesBySource)
			{
				if (Invalidated.ContainsByPredicate([&Source](const FString& File) { return FPaths::IsSamePath(File, Source.Key); }))
				{
					continue;
				}
				for (const FString& Dependency : Source.Value)
				{
					if (Invalidated.ContainsByPredicate([&Dependency](const FString& Changed)
						{ return FPaths::IsSamePath(Dependency, Changed); }))
					{
						Invalidated.Add(Source.Key);
						for (const FString& Tracked : TrackedSources)
						{
							if (FPaths::IsSamePath(Tracked, Source.Key)) { OutSources.Add(Tracked); break; }
						}
						bAdded = true;
						break;
					}
				}
			}
		}
		while (bAdded);
	}
}

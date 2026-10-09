#include "Generation/DreamFXSystemInheritance.h"

#include "DreamFXParser.h"
#include "Generation/DreamFXEmitterMerge.h"
#include "Generation/DreamFXProvenance.h"
#include "Generation/DreamFXValueLowering.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SourceFiles/DreamFXPaths.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		bool ValidateLayer(const FDocument& Document, FDiagnosticSink& Diagnostics)
		{
			TSet<FName> Parameters;
			for (const FParameterDecl& Parameter : Document.Parameters)
			{
				if (Parameters.Contains(FName(*Parameter.Name)))
				{
					Diagnostics.Error(TEXT("DFX3054"), Parameter.Location, FString::Printf(
						TEXT("Parameter '%s' is declared twice in one inheritance layer. An override must be declared in the child source, not twice in the same source."),
						*Parameter.Name));
					return false;
				}
				Parameters.Add(FName(*Parameter.Name));
			}
			TSet<FName> Emitters;
			for (const FEmitter& Emitter : Document.Emitters)
			{
				if (Emitters.Contains(FName(*Emitter.Name)))
				{
					Diagnostics.Error(TEXT("DFX3054"), Emitter.Location, FString::Printf(
						TEXT("Emitter '%s' is declared twice in one inheritance layer. Give each emitter in a source a unique name."), *Emitter.Name));
					return false;
				}
				Emitters.Add(FName(*Emitter.Name));
			}
			return true;
		}

		bool MergeSystems(const FDocument& Base, const FDocument& Child, FDocument& Out, FDiagnosticSink& Diagnostics)
		{
			// Identity always comes from the child, including when it intentionally changes Root.
			Out = Child;
			Out.ParentPath.Reset();
			Out.Settings = Base.Settings;
			for (const FPropertyEntry& Setting : Child.Settings)
			{
				FPropertyEntry* Existing = Out.Settings.FindByPredicate([&](const FPropertyEntry& Candidate)
					{ return Candidate.Name.Equals(Setting.Name, ESearchCase::IgnoreCase); });
				if (Existing) { *Existing = Setting; }
				else { Out.Settings.Add(Setting); }
			}

			Out.Parameters = Base.Parameters;
			for (const FParameterDecl& Parameter : Child.Parameters)
			{
				FParameterDecl* Existing = Out.Parameters.FindByPredicate([&](const FParameterDecl& Candidate)
					{ return FName(*Candidate.Name) == FName(*Parameter.Name); });
				if (Existing)
				{
					FNiagaraTypeDefinition BaseType, ChildType;
					bool bBaseDI = false, bChildDI = false;
					const FString PreviousFile = Diagnostics.GetFile();
					ON_SCOPE_EXIT { Diagnostics.SetFile(PreviousFile); };
					Diagnostics.SetFile(Existing->SourceFile.IsEmpty() ? Base.SourceFilePath : Existing->SourceFile);
					if (!FValueLowering::ResolveDeclaredType(*Existing, Diagnostics, BaseType, bBaseDI)) { return false; }
					Diagnostics.SetFile(Parameter.SourceFile.IsEmpty() ? Child.SourceFilePath : Parameter.SourceFile);
					if (!FValueLowering::ResolveDeclaredType(Parameter, Diagnostics, ChildType, bChildDI)) { return false; }
					if (BaseType != ChildType || bBaseDI != bChildDI)
					{
						Diagnostics.Error(TEXT("DFX3053"), Parameter.Location, FString::Printf(
							TEXT("Inherited parameter '%s' changes type from %s to %s. An override must preserve the parent's parameter type."),
							*Parameter.Name, *FValueLowering::DescribeType(BaseType), *FValueLowering::DescribeType(ChildType)));
						return false;
					}
					*Existing = Parameter;
				}
				else { Out.Parameters.Add(Parameter); }
			}

			Out.Stacks = Base.Stacks;
			for (const FStack& Stack : Child.Stacks)
			{
				const int32 Index = Out.Stacks.IndexOfByPredicate([&](const FStack& Candidate)
				{
					return Candidate.Kind == Stack.Kind && (Stack.Kind != EStackKind::SimulationStage
						|| FName(*Candidate.Stage.Name) == FName(*Stack.Stage.Name));
				});
				if (Index != INDEX_NONE) { Out.Stacks[Index] = Stack; }
				else { Out.Stacks.Add(Stack); }
			}

			Out.Emitters = Base.Emitters;
			for (const FEmitter& Emitter : Child.Emitters)
			{
				FEmitter* Existing = Out.Emitters.FindByPredicate([&](const FEmitter& Candidate)
					{ return FName(*Candidate.Name) == FName(*Emitter.Name); });
				if (Existing)
				{
					FEmitter Merged;
					if (!MergeEmitterDefinitions(*Existing, Emitter, Merged, Diagnostics)) { return false; }
					*Existing = MoveTemp(Merged);
				}
				else { Out.Emitters.Add(Emitter); }
			}

			FString ParentKey = Child.ParentPath;
			FPaths::NormalizeFilename(ParentKey);
			TMap<FString, FString> Dependencies;
			Dependencies.Add(ParentKey, Base.SourceHash);
			Out.SourceHash = FProvenance::HashWithSourceDependencies(Child.SourceHash, Dependencies);
			return true;
		}

		bool ResolveRecursive(const FDocument& Child, FDocument& Out, FDiagnosticSink& Diagnostics, TArray<FString>& Chain)
		{
			const FString PreviousFile = Diagnostics.GetFile();
			Diagnostics.SetFile(Child.SourceFilePath);
			ON_SCOPE_EXIT { Diagnostics.SetFile(PreviousFile); };
			if (!ValidateLayer(Child, Diagnostics)) { return false; }
			if (Child.ParentPath.IsEmpty())
			{
				Out = Child;
				return true;
			}
			if (Child.Kind != EDocumentKind::System)
			{
				Diagnostics.Error(TEXT("DFX3051"), Child.ParentLocation, TEXT("Only a System document can inherit a .dfs parent."));
				return false;
			}

			FString ParentFile, Error;
			if (!FDreamFXPaths::ResolveSourceReference(Child.ParentPath, Child.SourceFilePath, TEXT(".dfs"), ParentFile, Error))
			{
				Diagnostics.Error(TEXT("DFX3050"), Child.ParentLocation, Error);
				return false;
			}
			if (Chain.Num() >= 128 || Chain.ContainsByPredicate([&ParentFile](const FString& File)
				{ return FPaths::IsSamePath(File, ParentFile); }))
			{
				Diagnostics.Error(TEXT("DFX3052"), Child.ParentLocation, FString::Printf(
					TEXT("System inheritance contains a cycle or exceeds 128 sources: %s -> %s."), *FString::Join(Chain, TEXT(" -> ")), *ParentFile));
				return false;
			}

			FDocument Parent;
			FDiagnosticSink ParentDiagnostics;
			if (!FParser::ParseFile(ParentFile, Parent, ParentDiagnostics))
			{
				Diagnostics.Append(ParentDiagnostics);
				Diagnostics.Error(TEXT("DFX3051"), Child.ParentLocation,
					FString::Printf(TEXT("Parent source '%s' could not be parsed; see its diagnostics."), *ParentFile));
				return false;
			}
			Diagnostics.Append(ParentDiagnostics);
			if (Parent.Kind != EDocumentKind::System)
			{
				Diagnostics.Error(TEXT("DFX3051"), Child.ParentLocation,
					FString::Printf(TEXT("Parent source '%s' must declare a System."), *ParentFile));
				return false;
			}
			Chain.Add(ParentFile);
			FDocument FlattenedParent;
			const bool bResolved = ResolveRecursive(Parent, FlattenedParent, Diagnostics, Chain);
			Chain.Pop();
			return bResolved && MergeSystems(FlattenedParent, Child, Out, Diagnostics);
		}
	}

	bool ResolveSystemInheritance(const FDocument& Child, FDocument& OutFlattened, FDiagnosticSink& Diagnostics)
	{
		if (Child.ParentPath.IsEmpty()) { OutFlattened = Child; return true; }
		TArray<FString> Chain;
		if (!Child.SourceFilePath.IsEmpty()) { Chain.Add(FPaths::ConvertRelativePathToFull(Child.SourceFilePath)); }
		FDocument Flattened;
		if (!ResolveRecursive(Child, Flattened, Diagnostics, Chain)) { return false; }
		OutFlattened = MoveTemp(Flattened);
		return true;
	}
}

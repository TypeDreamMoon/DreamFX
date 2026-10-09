#include "DreamFXEmitterMerge.h"

#include "DreamFXValueLowering.h"

namespace UE::DreamFX::Editor
{
	bool MergeEmitterDefinitions(const FEmitter& Base, const FEmitter& Override, FEmitter& OutMerged,
		FDiagnosticSink& Diagnostics)
	{
		OutMerged = Base;
		OutMerged.Name = Override.Name;
		OutMerged.Location = Override.Location;

		for (const FPropertyEntry& Setting : Override.Settings)
		{
			FPropertyEntry* Existing = OutMerged.Settings.FindByPredicate([&](const FPropertyEntry& Candidate)
			{
				return Candidate.Name.Equals(Setting.Name, ESearchCase::IgnoreCase);
			});
			if (Existing) { *Existing = Setting; }
			else { OutMerged.Settings.Add(Setting); }
		}

		for (const FStack& Stack : Override.Stacks)
		{
			const int32 Index = OutMerged.Stacks.IndexOfByPredicate([&](const FStack& Candidate)
			{
				return Candidate.Kind == Stack.Kind
					&& (Stack.Kind != EStackKind::SimulationStage
						|| FName(*Candidate.Stage.Name) == FName(*Stack.Stage.Name));
			});
			if (Index != INDEX_NONE) { OutMerged.Stacks[Index] = Stack; }
			else { OutMerged.Stacks.Add(Stack); }
		}

		// Defaults have stable parameter names, unlike module calls. Keep untouched defaults and
		// replace matching values in place. A type change would reinterpret the base's stack reads.
		for (const FStatement& Default : Override.Defaults)
		{
			FStatement* Existing = OutMerged.Defaults.FindByPredicate([&](const FStatement& Candidate)
			{
				return FName(*Candidate.Name) == FName(*Default.Name);
			});
			if (Existing)
			{
				auto ResolveType = [&](const FStatement& Statement, FNiagaraTypeDefinition& Type)
				{
					FParameterDecl Declaration;
					Declaration.Name = Statement.Name;
					Declaration.TypeName = Statement.TypeName;
					Declaration.InnerTypeName = Statement.InnerTypeName;
					Declaration.Location = Statement.Location;
					bool bDataInterface = false;
					return FValueLowering::ResolveDeclaredType(Declaration, Diagnostics, Type, bDataInterface);
				};
				FNiagaraTypeDefinition BaseType, OverrideType;
				if (!ResolveType(*Existing, BaseType) || !ResolveType(Default, OverrideType)) { return false; }
				if (BaseType != OverrideType)
				{
					Diagnostics.Error(TEXT("DFX3048"), Default.Location,
						FString::Printf(TEXT("Default override '%s' changes its type from %s to %s. Overrides must keep the base parameter's type."),
							*Default.Name, *FValueLowering::DescribeType(BaseType), *FValueLowering::DescribeType(OverrideType)));
					return false;
				}
				*Existing = Default;
			}
			else { OutMerged.Defaults.Add(Default); }
		}

		// Renderer identity remains declaration order, so renderer overrides replace the whole list.
		if (Override.Renderers.Num() > 0) { OutMerged.Renderers = Override.Renderers; }
		return true;
	}
}

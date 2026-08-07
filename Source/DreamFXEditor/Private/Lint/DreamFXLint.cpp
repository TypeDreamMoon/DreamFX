#include "DreamFXLint.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		const FProperty* FindSetting(const TArray<FProperty>& Settings, const TCHAR* Name)
		{
			return Settings.FindByPredicate([Name](const FProperty& Setting)
			{
				return Setting.Name.Equals(Name, ESearchCase::IgnoreCase);
			});
		}

		bool SettingEquals(const TArray<FProperty>& Settings, const TCHAR* Name, const TCHAR* ExpectedValue)
		{
			const FProperty* Setting = FindSetting(Settings, Name);
			if (Setting == nullptr || !Setting->Value.IsValid())
			{
				return false;
			}
			const FValue& Value = *Setting->Value;
			if (Value.Kind == EValueKind::Name)
			{
				return Value.Text.Equals(ExpectedValue, ESearchCase::IgnoreCase);
			}
			if (Value.Kind == EValueKind::Bool)
			{
				return (Value.bBool ? TEXT("true") : TEXT("false")) == FString(ExpectedValue);
			}
			return false;
		}

		/** True when any value in the tree is a call whose name starts with "Random". */
		bool MentionsRandom(const FValue& Value)
		{
			if (Value.Kind == EValueKind::Call && Value.Text.Contains(TEXT("Random"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			for (const FValuePtr& Element : Value.Elements)
			{
				if (Element.IsValid() && MentionsRandom(*Element))
				{
					return true;
				}
			}
			for (const FNamedArgument& Argument : Value.Arguments)
			{
				if (Argument.Value.IsValid() && MentionsRandom(*Argument.Value))
				{
					return true;
				}
			}
			if (Value.Left.IsValid() && MentionsRandom(*Value.Left))
			{
				return true;
			}
			if (Value.Right.IsValid() && MentionsRandom(*Value.Right))
			{
				return true;
			}
			return false;
		}

		/** Finds the first statement in an emitter that introduces randomness. */
		const FStatement* FindRandomStatement(const FEmitter& Emitter)
		{
			for (const FStack& Stack : Emitter.Stacks)
			{
				for (const FStatement& Statement : Stack.Statements)
				{
					if (Statement.Name.Contains(TEXT("Random"), ESearchCase::IgnoreCase))
					{
						return &Statement;
					}
					for (const FNamedArgument& Argument : Statement.Arguments)
					{
						if (Argument.Value.IsValid() && MentionsRandom(*Argument.Value))
						{
							return &Statement;
						}
					}
					if (Statement.Value.IsValid() && MentionsRandom(*Statement.Value))
					{
						return &Statement;
					}
				}
			}
			return nullptr;
		}

		/** Finds the first spawn module whose output is a rate rather than a fixed count. */
		const FStatement* FindUnboundedSpawnStatement(const FEmitter& Emitter)
		{
			static const TCHAR* const RateModules[] =
			{
				TEXT("SpawnRate"), TEXT("SpawnPerUnit"), TEXT("SpawnPerFrame"), TEXT("SpawnParticlesInGrid"),
			};

			for (const FStack& Stack : Emitter.Stacks)
			{
				for (const FStatement& Statement : Stack.Statements)
				{
					if (Statement.Kind != EStatementKind::ModuleCall)
					{
						continue;
					}
					for (const TCHAR* Module : RateModules)
					{
						if (Statement.Name.Contains(Module, ESearchCase::IgnoreCase))
						{
							return &Statement;
						}
					}
				}
			}
			return nullptr;
		}

		void LintEmitter(const FEmitter& Emitter, FDiagnosticSink& Diagnostics)
		{
			// A GPU emitter cannot compute its own bounds: the particle data never leaves the GPU, so
			// there is nothing to read back. Without fixed bounds it gets a default box and vanishes
			// at the wrong camera angle -- a bug that only shows up in a specific shot.
			if (SettingEquals(Emitter.Settings, TEXT("SimTarget"), TEXT("GPU"))
				&& FindSetting(Emitter.Settings, TEXT("FixedBounds")) == nullptr)
			{
				const FProperty* SimTarget = FindSetting(Emitter.Settings, TEXT("SimTarget"));
				Diagnostics.Warning(TEXT("DFX7101"), SimTarget ? SimTarget->Location : Emitter.Location,
					FString::Printf(TEXT("Emitter '%s' simulates on the GPU but declares no FixedBounds. GPU emitters cannot compute their own bounds, so it will use a default box and may be culled unexpectedly."),
						*Emitter.Name));
			}

			// Rate-based spawning has no ceiling of its own: a long-lived emitter with an automatic
			// allocation estimate keeps reallocating, and a runaway rate has nothing to stop it.
			if (const FStatement* Spawn = FindUnboundedSpawnStatement(Emitter))
			{
				if (!SettingEquals(Emitter.Settings, TEXT("AllocationMode"), TEXT("Fixed"))
					&& !SettingEquals(Emitter.Settings, TEXT("AllocationMode"), TEXT("FixedCount")))
				{
					Diagnostics.Warning(TEXT("DFX7102"), Spawn->Location,
						FString::Printf(TEXT("Emitter '%s' spawns by rate ('%s') with no upper bound. Set AllocationMode = Fixed and PreAllocationCount to cap the particle count."),
							*Emitter.Name, *Spawn->Name));
				}
			}

			// Randomness without a fixed seed means the effect differs every play. Fine for ambience,
			// wrong for anything a cinematic or a test compares against.
			if (const FStatement* Random = FindRandomStatement(Emitter))
			{
				if (!SettingEquals(Emitter.Settings, TEXT("Determinism"), TEXT("true")))
				{
					Diagnostics.Warning(TEXT("DFX7103"), Random->Location,
						FString::Printf(TEXT("Emitter '%s' uses randomness ('%s') but does not set Determinism = true, so it will look different on every play. Add Determinism and RandomSeed to its Settings if reproducibility matters."),
							*Emitter.Name, *Random->Name));
				}
			}
		}
	}

	void FLint::Run(const FDocument& Document, FDiagnosticSink& Diagnostics)
	{
		Diagnostics.SetFile(Document.SourceFilePath);

		if (Document.Kind == EDocumentKind::System)
		{
			for (const FEmitter& Emitter : Document.Emitters)
			{
				LintEmitter(Emitter, Diagnostics);
			}
		}
		else if (Document.Kind == EDocumentKind::Emitter)
		{
			LintEmitter(Document.EmitterDefinition, Diagnostics);
		}
	}
}

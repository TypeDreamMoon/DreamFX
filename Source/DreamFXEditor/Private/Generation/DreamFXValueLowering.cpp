#include "DreamFXValueLowering.h"

#include "NiagaraTypes.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** Number of float components a vector-family Niagara type expects, or 0 if it is not one. */
		int32 FloatComponentCount(const FNiagaraTypeDefinition& Type)
		{
			if (Type == FNiagaraTypeDefinition::GetVec2Def())     { return 2; }
			if (Type == FNiagaraTypeDefinition::GetVec3Def())     { return 3; }
			if (Type == FNiagaraTypeDefinition::GetPositionDef()) { return 3; }
			if (Type == FNiagaraTypeDefinition::GetVec4Def())     { return 4; }
			if (Type == FNiagaraTypeDefinition::GetColorDef())    { return 4; }
			if (Type == FNiagaraTypeDefinition::GetQuatDef())     { return 4; }
			return 0;
		}

		bool EvaluateScalar(const FValue& Value, double& OutNumber, bool& bOutIsInteger)
		{
			if (Value.Kind != EValueKind::Number)
			{
				return false;
			}
			OutNumber = Value.Number;
			bOutIsInteger = Value.bIsIntegerLiteral;
			return true;
		}
	}

	FString FValueLowering::DescribeType(const FNiagaraTypeDefinition& Type)
	{
		return Type.IsValid() ? Type.GetName() : TEXT("<invalid>");
	}

	bool FValueLowering::Lower(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
		const FString& InputDisplayName, FDiagnosticSink& Diagnostics, FInputValue& OutValue)
	{
		if (!TargetType.IsValid())
		{
			Diagnostics.Error(TEXT("DFX4004"), Value.Location,
				FString::Printf(TEXT("Input '%s' has no resolvable Niagara type."), *InputDisplayName));
			return false;
		}

		const int32 Components = FloatComponentCount(TargetType);

		switch (Value.Kind)
		{
		case EValueKind::Number:
		{
			double Number = 0.0;
			bool bIsInteger = false;
			EvaluateScalar(Value, Number, bIsInteger);

			if (TargetType == FNiagaraTypeDefinition::GetFloatDef())
			{
				// L7: widening int -> float is always fine.
				FNiagaraFloat Float;
				Float.Value = static_cast<float>(Number);
				OutValue = FInputValue::MakeLiteral(FNiagaraFloat::StaticStruct(), &Float);
				return true;
			}

			if (TargetType == FNiagaraTypeDefinition::GetIntDef() || TargetType.IsEnum())
			{
				if (!bIsInteger)
				{
					// L7: narrowing is refused outright. A silently truncated spawn count is one of the
					// hardest VFX bugs to trace back to its source line.
					Diagnostics.Error(TEXT("DFX4003"), Value.Location,
						FString::Printf(TEXT("Input '%s' is an integer, but %s was written. Narrowing is not implicit -- write int(%s) if truncation is intended."),
							*InputDisplayName, *Value.ToSourceString(), *Value.ToSourceString()));
					return false;
				}
				FNiagaraInt32 Integer;
				Integer.Value = static_cast<int32>(Number);
				OutValue = FInputValue::MakeLiteral(FNiagaraInt32::StaticStruct(), &Integer);
				return true;
			}

			if (Components > 0)
			{
				Diagnostics.Error(TEXT("DFX4002"), Value.Location,
					FString::Printf(TEXT("Input '%s' is a %s with %d components, but a single number was written. Write all %d components, e.g. (%s, ...)."),
						*InputDisplayName, *DescribeType(TargetType), Components, Components, *Value.ToSourceString()));
				return false;
			}

			Diagnostics.Error(TEXT("DFX4001"), Value.Location,
				FString::Printf(TEXT("Input '%s' expects %s, but a number was written."),
					*InputDisplayName, *DescribeType(TargetType)));
			return false;
		}

		case EValueKind::Bool:
		{
			if (TargetType != FNiagaraTypeDefinition::GetBoolDef())
			{
				Diagnostics.Error(TEXT("DFX4001"), Value.Location,
					FString::Printf(TEXT("Input '%s' expects %s, but a boolean was written."),
						*InputDisplayName, *DescribeType(TargetType)));
				return false;
			}
			FNiagaraBool Boolean;
			Boolean.SetValue(Value.bBool);
			OutValue = FInputValue::MakeLiteral(FNiagaraBool::StaticStruct(), &Boolean);
			return true;
		}

		case EValueKind::Vector:
		{
			if (Components == 0)
			{
				Diagnostics.Error(TEXT("DFX4001"), Value.Location,
					FString::Printf(TEXT("Input '%s' expects %s, but a vector literal was written."),
						*InputDisplayName, *DescribeType(TargetType)));
				return false;
			}

			if (Value.Elements.Num() != Components)
			{
				Diagnostics.Error(TEXT("DFX4002"), Value.Location,
					FString::Printf(TEXT("Input '%s' is a %s and needs %d components, but %d were written."),
						*InputDisplayName, *DescribeType(TargetType), Components, Value.Elements.Num()));
				return false;
			}

			float Floats[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int32 Index = 0; Index < Components; ++Index)
			{
				const FValuePtr& Element = Value.Elements[Index];
				double Number = 0.0;
				bool bIsInteger = false;
				if (!Element.IsValid() || !EvaluateScalar(*Element, Number, bIsInteger))
				{
					Diagnostics.Error(TEXT("DFX4005"), Element.IsValid() ? Element->Location : Value.Location,
						FString::Printf(TEXT("Component %d of input '%s' is not a numeric literal."),
							Index, *InputDisplayName));
					return false;
				}
				Floats[Index] = static_cast<float>(Number);
			}

			// The struct layouts here are all tightly packed floats, so a single memcpy of the right
			// width is exact for every member of the family.
			const UScriptStruct* Struct = TargetType.GetScriptStruct();
			if (Struct == nullptr || Struct->GetStructureSize() != Components * static_cast<int32>(sizeof(float)))
			{
				Diagnostics.Error(TEXT("DFX4004"), Value.Location,
					FString::Printf(TEXT("Input '%s' has type %s whose in-memory layout is not %d packed floats; DreamFX cannot write it as a vector literal."),
						*InputDisplayName, *DescribeType(TargetType), Components));
				return false;
			}

			OutValue = FInputValue::MakeLiteral(Struct, Floats);
			return true;
		}

		case EValueKind::String:
			Diagnostics.Error(TEXT("DFX4001"), Value.Location,
				FString::Printf(TEXT("Input '%s' expects %s; a quoted string is only valid for asset-typed values."),
					*InputDisplayName, *DescribeType(TargetType)));
			return false;

		case EValueKind::Name:
			Diagnostics.Error(TEXT("DFX4090"), Value.Location,
				FString::Printf(TEXT("Input '%s': linked parameters and enum literals are not available yet (planned for Phase 2)."),
					*InputDisplayName));
			return false;

		case EValueKind::Call:
		case EValueKind::Hlsl:
		case EValueKind::Curve:
		case EValueKind::Negate:
		case EValueKind::Binary:
			Diagnostics.Error(TEXT("DFX4091"), Value.Location,
				FString::Printf(TEXT("Input '%s': dynamic inputs, hlsl blocks, curves and inline expressions are not available yet (planned for Phase 3)."),
					*InputDisplayName));
			return false;

		default:
			Diagnostics.Error(TEXT("DFX4005"), Value.Location,
				FString::Printf(TEXT("Input '%s': unsupported value form."), *InputDisplayName));
			return false;
		}
	}
}

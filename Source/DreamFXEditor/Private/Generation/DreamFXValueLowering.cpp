#include "DreamFXValueLowering.h"

#include "NiagaraDataInterface.h"
#include "NiagaraTypes.h"
#include "UObject/UObjectIterator.h"

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

		/**
		 * Niagara's parameter namespaces, borrowed wholesale as the DSL's scope system (plan 2.3-2).
		 * A dotted name starting with one of these is a parameter reference; anything else is an enum
		 * entry or a plain identifier.
		 */
		const TCHAR* const ParameterNamespaces[] =
		{
			TEXT("Particles"), TEXT("Emitter"), TEXT("System"), TEXT("User"), TEXT("Engine"),
			TEXT("Module"), TEXT("Transient"), TEXT("StackContext"), TEXT("Local"), TEXT("Output"),
			TEXT("Parameters"), TEXT("DataInstance"),
		};

		/**
		 * Niagara's user-defined enum assets label entries "Complete (Let Particles Finish then Kill
		 * Emitter)". The leading label is the name; the parenthesis is prose. Nobody would write the
		 * whole sentence in a DSL, so it is trimmed off before matching.
		 */
		FString EnumLabelOf(const FString& DisplayName)
		{
			int32 ParenIndex;
			const FString Label = DisplayName.FindChar(TEXT('('), ParenIndex)
				? DisplayName.Left(ParenIndex)
				: DisplayName;
			return Label.TrimStartAndEnd();
		}

		/**
		 * Finds an enum entry by the short name an author writes.
		 *
		 * UEnum stores entries fully qualified ("ENiagaraCoordinateSpace::Simulation") but nobody
		 * writes that in a DSL. Display names are matched too -- and for user-defined enum assets they
		 * are the *only* useful name, since the internal ones are NewEnumerator0, NewEnumerator1, ...
		 */
		bool FindEnumEntry(const UEnum* Enum, const FString& Written, FName& OutQualifiedName)
		{
			if (Enum == nullptr)
			{
				return false;
			}

			const FString NormalizedWritten = NormalizeInputIdentifier(Written);

			// The trailing entry of a UENUM is the generated _MAX sentinel; it is not a real value.
			const int32 Count = Enum->NumEnums() - 1;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FString ShortName = Enum->GetNameStringByIndex(Index);
				const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();

				if (ShortName.Equals(Written, ESearchCase::IgnoreCase)
					|| NormalizeInputIdentifier(DisplayName) == NormalizedWritten
					|| NormalizeInputIdentifier(EnumLabelOf(DisplayName)) == NormalizedWritten)
				{
					OutQualifiedName = Enum->GetNameByIndex(Index);
					return true;
				}
			}
			return false;
		}

		FString ListEnumEntries(const UEnum* Enum)
		{
			if (Enum == nullptr)
			{
				return TEXT("(none)");
			}
			TArray<FString> Names;
			const int32 Count = Enum->NumEnums() - 1;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				// User-defined enum assets store meaningless internal names (NewEnumerator0) and carry
				// the real ones as display names, so listing only one of the two is useless roughly
				// half the time.
				const FString ShortName = Enum->GetNameStringByIndex(Index);
				const FString Label = EnumLabelOf(Enum->GetDisplayNameTextByIndex(Index).ToString());
				Names.Add(Label.IsEmpty() || Label == ShortName ? ShortName : Label);
			}
			return FString::Join(Names, TEXT(", "));
		}
	}

	bool FValueLowering::IsNamespacedName(const FString& Name)
	{
		FString Head = Name;
		int32 DotIndex;
		if (Name.FindChar(TEXT('.'), DotIndex))
		{
			Head = Name.Left(DotIndex);
		}
		else
		{
			// A single segment is an enum entry or a plain identifier, never a parameter reference:
			// every Niagara parameter is namespace-qualified.
			return false;
		}

		for (const TCHAR* Namespace : ParameterNamespaces)
		{
			if (Head.Equals(Namespace, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	FString FValueLowering::DescribeType(const FNiagaraTypeDefinition& Type)
	{
		return Type.IsValid() ? Type.GetName() : TEXT("<invalid>");
	}

	bool FValueLowering::ResolveDeclaredType(const FParameterDecl& Declaration, FDiagnosticSink& Diagnostics,
		FNiagaraTypeDefinition& OutType, bool& bOutIsDataInterface)
	{
		bOutIsDataInterface = false;

		struct FScalarTypeEntry
		{
			const TCHAR* Name;
			const FNiagaraTypeDefinition& (*Getter)();
		};

		static const FScalarTypeEntry ScalarTypes[] =
		{
			{ TEXT("float"),       &FNiagaraTypeDefinition::GetFloatDef },
			{ TEXT("int"),         &FNiagaraTypeDefinition::GetIntDef },
			{ TEXT("int32"),       &FNiagaraTypeDefinition::GetIntDef },
			{ TEXT("bool"),        &FNiagaraTypeDefinition::GetBoolDef },
			{ TEXT("Vector2"),     &FNiagaraTypeDefinition::GetVec2Def },
			{ TEXT("Vec2"),        &FNiagaraTypeDefinition::GetVec2Def },
			{ TEXT("Vector"),      &FNiagaraTypeDefinition::GetVec3Def },
			{ TEXT("Vector3"),     &FNiagaraTypeDefinition::GetVec3Def },
			{ TEXT("Vec3"),        &FNiagaraTypeDefinition::GetVec3Def },
			{ TEXT("Vector4"),     &FNiagaraTypeDefinition::GetVec4Def },
			{ TEXT("Vec4"),        &FNiagaraTypeDefinition::GetVec4Def },
			{ TEXT("Color"),       &FNiagaraTypeDefinition::GetColorDef },
			{ TEXT("LinearColor"), &FNiagaraTypeDefinition::GetColorDef },
			{ TEXT("Position"),    &FNiagaraTypeDefinition::GetPositionDef },
			{ TEXT("Quat"),        &FNiagaraTypeDefinition::GetQuatDef },
		};

		for (const FScalarTypeEntry& Entry : ScalarTypes)
		{
			if (Declaration.TypeName.Equals(Entry.Name, ESearchCase::IgnoreCase))
			{
				OutType = Entry.Getter();
				return true;
			}
		}

		// `DI<SkeletalMesh>` and the asset shorthands both land on a data interface class. Niagara has
		// no raw-UObject parameter type: a texture reaches a system through a Texture data interface,
		// not a bare UTexture2D.
		FString DataInterfaceName;
		if (Declaration.TypeName.Equals(TEXT("DI"), ESearchCase::IgnoreCase))
		{
			if (Declaration.InnerTypeName.IsEmpty())
			{
				Diagnostics.Error(TEXT("DFX4020"), Declaration.Location,
					FString::Printf(TEXT("Parameter '%s': 'DI' needs an inner type, e.g. DI<SkeletalMesh>."),
						*Declaration.Name));
				return false;
			}
			DataInterfaceName = Declaration.InnerTypeName;
		}
		else
		{
			DataInterfaceName = Declaration.TypeName;
		}

		// Matched by reflection rather than a fixed table so data interfaces from other plugins work
		// without a DreamFX change -- the same reasoning as renderer class lookup.
		const FString Candidate = FString::Printf(TEXT("NiagaraDataInterface%s"), *DataInterfaceName);
		for (TObjectIterator<UClass> ClassIterator; ClassIterator; ++ClassIterator)
		{
			UClass* Class = *ClassIterator;
			if (!Class->IsChildOf(UNiagaraDataInterface::StaticClass())
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			const FString ClassName = Class->GetName();
			if (ClassName.Equals(Candidate, ESearchCase::IgnoreCase)
				|| ClassName.Equals(DataInterfaceName, ESearchCase::IgnoreCase))
			{
				OutType = FNiagaraTypeDefinition(Class);
				bOutIsDataInterface = true;
				return true;
			}
		}

		Diagnostics.Error(TEXT("DFX4021"), Declaration.Location,
			FString::Printf(TEXT("Parameter '%s' has unknown type '%s'. Expected float, int, bool, Vector2, Vector, Vector4, Color, Position, Quat, or a data interface written as DI<Name>."),
				*Declaration.Name, *Declaration.TypeName));
		return false;
	}

	bool FValueLowering::InferType(const FValue& Value, const FString& TargetName, FDiagnosticSink& Diagnostics,
		FNiagaraTypeDefinition& OutType)
	{
		switch (Value.Kind)
		{
		case EValueKind::Number:
			OutType = Value.bIsIntegerLiteral ? FNiagaraTypeDefinition::GetIntDef() : FNiagaraTypeDefinition::GetFloatDef();
			return true;

		case EValueKind::Bool:
			OutType = FNiagaraTypeDefinition::GetBoolDef();
			return true;

		case EValueKind::Vector:
			switch (Value.Elements.Num())
			{
			case 2:
				OutType = FNiagaraTypeDefinition::GetVec2Def();
				return true;
			case 3:
				OutType = FNiagaraTypeDefinition::GetVec3Def();
				return true;
			case 4:
				// Four components are ambiguous between Vector4 and Color. The parameter name is the
				// only signal available, and getting it wrong is visible immediately, so the guess is
				// safe -- but it is a guess, hence the note in the docs.
				OutType = TargetName.Contains(TEXT("Color"), ESearchCase::IgnoreCase)
					? FNiagaraTypeDefinition::GetColorDef()
					: FNiagaraTypeDefinition::GetVec4Def();
				return true;
			default:
				Diagnostics.Error(TEXT("DFX4002"), Value.Location,
					FString::Printf(TEXT("'%s' is assigned a %d-component vector; Niagara has 2, 3 and 4 component types only."),
						*TargetName, Value.Elements.Num()));
				return false;
			}

		default:
			Diagnostics.Error(TEXT("DFX4022"), Value.Location,
				FString::Printf(TEXT("Cannot infer the type of '%s' from its value. A first assignment to a new attribute must use a literal, so its type is unambiguous."),
					*TargetName));
			return false;
		}
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
		{
			// A namespaced name is a parameter reference regardless of the target type; a bare name is
			// an enum entry. Checking the namespace first matters because an enum-typed input can
			// legitimately be driven by a linked parameter.
			if (IsNamespacedName(Value.Text))
			{
				OutValue = FInputValue::MakeLinked(FNiagaraVariableBase(TargetType, FName(*Value.Text)));
				return true;
			}

			if (UEnum* Enum = TargetType.GetEnum())
			{
				FName QualifiedName;
				if (!FindEnumEntry(Enum, Value.Text, QualifiedName))
				{
					Diagnostics.Error(TEXT("DFX4006"), Value.Location,
						FString::Printf(TEXT("Input '%s' is a %s, which has no entry named '%s'. Valid entries: %s"),
							*InputDisplayName, *DescribeType(TargetType), *Value.Text, *ListEnumEntries(Enum)));
					return false;
				}
				OutValue = FInputValue::MakeEnum(Enum, QualifiedName);
				return true;
			}

			Diagnostics.Error(TEXT("DFX4007"), Value.Location,
				FString::Printf(TEXT("Input '%s' expects %s. '%s' is neither a literal of that type nor a parameter reference -- parameter references start with a namespace such as User., Particles., Emitter., System. or Engine."),
					*InputDisplayName, *DescribeType(TargetType), *Value.Text));
			return false;
		}

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

#include "DreamFXExpressions.h"

#include "DreamFXValueLowering.h"

#include "Dom/JsonObject.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceVector2DCurve.h"
#include "NiagaraDataInterfaceVector4Curve.h"
#include "NiagaraDataInterfaceVectorCurve.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamFX::Editor
{
	namespace
	{
		/** L6's whitelist. Every entry has the same name and meaning in HLSL, which is why it is short. */
		struct FBuiltin
		{
			const TCHAR* Name;
			int32 MinArguments;
			int32 MaxArguments;
		};

		const FBuiltin Builtins[] =
		{
			{ TEXT("normalize"), 1, 1 },
			{ TEXT("saturate"),  1, 1 },
			{ TEXT("clamp"),     3, 3 },
			{ TEXT("lerp"),      3, 3 },
			{ TEXT("frac"),      1, 1 },
			{ TEXT("min"),       2, 2 },
			{ TEXT("max"),       2, 2 },
			{ TEXT("abs"),       1, 1 },
			{ TEXT("floor"),     1, 1 },
			{ TEXT("ceil"),      1, 1 },
			{ TEXT("pow"),       2, 2 },
			{ TEXT("sqrt"),      1, 1 },
			{ TEXT("dot"),       2, 2 },
			{ TEXT("cross"),     2, 2 },
			{ TEXT("length"),    1, 1 },
		};

		const FBuiltin* FindBuiltin(const FString& Name)
		{
			for (const FBuiltin& Builtin : Builtins)
			{
				if (Name.Equals(Builtin.Name, ESearchCase::CaseSensitive))
				{
					return &Builtin;
				}
			}
			return nullptr;
		}

		FString FormatNumber(double Number)
		{
			// Always emit a decimal point: HLSL's integer literals do not implicitly widen in every
			// context, and `1/2` meaning zero is a bug nobody enjoys finding.
			FString Text = FString::SanitizeFloat(Number);
			if (!Text.Contains(TEXT(".")) && !Text.Contains(TEXT("e")) && !Text.Contains(TEXT("E")))
			{
				Text += TEXT(".0");
			}
			return Text;
		}

		bool RenderNode(const FValue& Value, const FString& DisplayName, FDiagnosticSink& Diagnostics, FString& Out);

		bool RenderArguments(const TArray<FValuePtr>& Arguments, const FString& DisplayName,
			FDiagnosticSink& Diagnostics, TArray<FString>& Out)
		{
			for (const FValuePtr& Argument : Arguments)
			{
				FString Rendered;
				if (!Argument.IsValid() || !RenderNode(*Argument, DisplayName, Diagnostics, Rendered))
				{
					return false;
				}
				Out.Add(MoveTemp(Rendered));
			}
			return true;
		}

		bool RenderNode(const FValue& Value, const FString& DisplayName, FDiagnosticSink& Diagnostics, FString& Out)
		{
			switch (Value.Kind)
			{
			case EValueKind::Number:
				Out = FormatNumber(Value.Number);
				return true;

			case EValueKind::Bool:
				Out = Value.bBool ? TEXT("true") : TEXT("false");
				return true;

			case EValueKind::Name:
				if (!FValueLowering::IsNamespacedName(Value.Text))
				{
					Diagnostics.Error(TEXT("DFX4032"), Value.Location,
						FString::Printf(TEXT("'%s' in the expression for '%s' is not a parameter. Only namespace-qualified parameters (User.X, Particles.X, Engine.X, ...) can be read from an expression."),
							*Value.Text, *DisplayName));
					return false;
				}
				// Niagara's custom-HLSL translator resolves namespaced parameter names written in
				// dotted form directly against the parameter map.
				Out = Value.Text;
				return true;

			case EValueKind::Vector:
			{
				TArray<FString> Components;
				if (!RenderArguments(Value.Elements, DisplayName, Diagnostics, Components))
				{
					return false;
				}
				if (Components.Num() < 2 || Components.Num() > 4)
				{
					Diagnostics.Error(TEXT("DFX4002"), Value.Location,
						FString::Printf(TEXT("Expression for '%s': a vector literal must have 2 to 4 components."), *DisplayName));
					return false;
				}
				Out = FString::Printf(TEXT("float%d(%s)"), Components.Num(), *FString::Join(Components, TEXT(", ")));
				return true;
			}

			case EValueKind::Negate:
			{
				FString Operand;
				if (!Value.Left.IsValid() || !RenderNode(*Value.Left, DisplayName, Diagnostics, Operand))
				{
					return false;
				}
				Out = FString::Printf(TEXT("(-%s)"), *Operand);
				return true;
			}

			case EValueKind::Binary:
			{
				FString Left;
				FString Right;
				if (!Value.Left.IsValid() || !RenderNode(*Value.Left, DisplayName, Diagnostics, Left)
					|| !Value.Right.IsValid() || !RenderNode(*Value.Right, DisplayName, Diagnostics, Right))
				{
					return false;
				}
				// Fully parenthesised rather than precedence-aware: the DSL's precedence and HLSL's
				// agree today, but relying on that agreement is a silent-wrong-answer risk for no gain.
				Out = FString::Printf(TEXT("(%s %s %s)"), *Left, *Value.Text, *Right);
				return true;
			}

			case EValueKind::Call:
			{
				const FBuiltin* Builtin = FindBuiltin(Value.Text);
				if (Builtin == nullptr)
				{
					Diagnostics.Error(TEXT("DFX4031"), Value.Location,
						FString::Printf(TEXT("'%s' is not an allowed inline function. Allowed: %s. Anything else belongs in a .dfm dynamic input or an hlsl { } block."),
							*Value.Text, *FExpressions::ListBuiltins()));
					return false;
				}

				if (Value.Arguments.Num() > 0)
				{
					Diagnostics.Error(TEXT("DFX4033"), Value.Location,
						FString::Printf(TEXT("Builtin '%s' takes positional arguments, not named ones."), *Value.Text));
					return false;
				}

				const int32 Count = Value.Elements.Num();
				if (Count < Builtin->MinArguments || Count > Builtin->MaxArguments)
				{
					Diagnostics.Error(TEXT("DFX4034"), Value.Location,
						Builtin->MinArguments == Builtin->MaxArguments
							? FString::Printf(TEXT("Builtin '%s' takes %d argument(s), but %d were written."),
								*Value.Text, Builtin->MinArguments, Count)
							: FString::Printf(TEXT("Builtin '%s' takes %d to %d arguments, but %d were written."),
								*Value.Text, Builtin->MinArguments, Builtin->MaxArguments, Count));
					return false;
				}

				TArray<FString> Arguments;
				if (!RenderArguments(Value.Elements, DisplayName, Diagnostics, Arguments))
				{
					return false;
				}
				Out = FString::Printf(TEXT("%s(%s)"), *Value.Text, *FString::Join(Arguments, TEXT(", ")));
				return true;
			}

			case EValueKind::Hlsl:
			{
				// A raw block nested inside arithmetic is legal and useful: `hlsl { ... } * 0.5`.
				FString Inner;
				if (!FExpressions::PrepareRawBlock(Value, DisplayName, Diagnostics, Inner))
				{
					return false;
				}
				Out = FString::Printf(TEXT("(%s)"), *Inner);
				return true;
			}

			default:
				Diagnostics.Error(TEXT("DFX4035"), Value.Location,
					FString::Printf(TEXT("Expression for '%s' contains a value that has no HLSL form."), *DisplayName));
				return false;
			}
		}

		/** The curve data interface classes DreamFX can fill, and how many float channels each takes. */
		int32 CurveChannelCount(const FNiagaraTypeDefinition& Type)
		{
			UClass* Class = Type.IsDataInterface() ? Type.GetClass() : nullptr;
			if (Class == nullptr)
			{
				return 0;
			}
			if (Class->IsChildOf(UNiagaraDataInterfaceCurve::StaticClass()))          { return 1; }
			if (Class->IsChildOf(UNiagaraDataInterfaceVector2DCurve::StaticClass()))  { return 2; }
			if (Class->IsChildOf(UNiagaraDataInterfaceVectorCurve::StaticClass()))    { return 3; }
			if (Class->IsChildOf(UNiagaraDataInterfaceVector4Curve::StaticClass()))   { return 4; }
			return 0;
		}

		const TCHAR* InterpModeToEnum(const FString& Written, bool& bOutRecognised)
		{
			bOutRecognised = true;
			if (Written.IsEmpty() || Written.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))     { return TEXT("RCIM_Cubic"); }
			if (Written.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase))                         { return TEXT("RCIM_Cubic"); }
			if (Written.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))                        { return TEXT("RCIM_Linear"); }
			if (Written.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))                      { return TEXT("RCIM_Constant"); }
			bOutRecognised = false;
			return TEXT("RCIM_Cubic");
		}
	}

	bool FExpressions::IsBuiltinFunction(const FString& Name)
	{
		return FindBuiltin(Name) != nullptr;
	}

	FString FExpressions::ListBuiltins()
	{
		TArray<FString> Names;
		for (const FBuiltin& Builtin : Builtins)
		{
			Names.Add(Builtin.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	bool FExpressions::RequiresHlslLowering(const FValue& Value)
	{
		switch (Value.Kind)
		{
		case EValueKind::Binary:
		case EValueKind::Negate:
			return true;
		case EValueKind::Call:
			// A dynamic input call is a value mode of its own, not an expression.
			return IsBuiltinFunction(Value.Text);
		default:
			return false;
		}
	}

	bool FExpressions::Render(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
		const FString& DisplayName, FDiagnosticSink& Diagnostics, FString& OutHlsl)
	{
		FString Rendered;
		if (!RenderNode(Value, DisplayName, Diagnostics, Rendered))
		{
			return false;
		}

		// A scalar expression driving a vector input is the one implicit conversion worth allowing:
		// `Gravity = User.Strength * -1.0` reads naturally and HLSL splats it anyway. Being explicit
		// about the cast means the generated code says what it does.
		OutHlsl = Rendered;
		(void)TargetType;
		return true;
	}

	bool FExpressions::PrepareRawBlock(const FValue& Value, const FString& DisplayName,
		FDiagnosticSink& Diagnostics, FString& OutHlsl)
	{
		FString Body = Value.Text;

		// Collapse to one line. The stack input stores a single expression string, and embedded
		// newlines survive into generated HLSL where they only make the compile log harder to read.
		Body.ReplaceInline(TEXT("\r\n"), TEXT(" "), ESearchCase::CaseSensitive);
		Body.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);
		Body.ReplaceInline(TEXT("\t"), TEXT(" "), ESearchCase::CaseSensitive);
		while (Body.Contains(TEXT("  ")))
		{
			Body.ReplaceInline(TEXT("  "), TEXT(" "), ESearchCase::CaseSensitive);
		}
		Body.TrimStartAndEndInline();
		Body.RemoveFromEnd(TEXT(";"));
		Body.TrimEndInline();

		if (Body.IsEmpty())
		{
			Diagnostics.Error(TEXT("DFX4036"), Value.Location,
				FString::Printf(TEXT("The hlsl block for '%s' is empty."), *DisplayName));
			return false;
		}

		// The input's contract is a single rvalue -- no declarations, no statements, no return. The
		// node it lowers to has one typed output pin and no body of its own to hold them. A block that
		// genuinely needs statements is what a .dfm DynamicInput is for.
		if (Body.Contains(TEXT(";")) || Body.Contains(TEXT("return ")))
		{
			Diagnostics.Error(TEXT("DFX4030"), Value.Location,
				FString::Printf(TEXT("The hlsl block for '%s' must be a single expression: no statements, no local variables, no return. Move multi-statement logic into a .dfm DynamicInput and call it here."),
					*DisplayName));
			return false;
		}

		OutHlsl = Body;
		return true;
	}

	bool FExpressions::RenderCurve(const FValue& Value, const FNiagaraTypeDefinition& TargetType,
		const FString& DisplayName, FDiagnosticSink& Diagnostics, FString& OutJson)
	{
		const int32 Channels = CurveChannelCount(TargetType);
		if (Channels == 0)
		{
			Diagnostics.Error(TEXT("DFX4037"), Value.Location,
				FString::Printf(TEXT("'%s' is %s; a curve { } literal only fits a curve data interface input."),
					*DisplayName, *FValueLowering::DescribeType(TargetType)));
			return false;
		}

		if (Value.CurveKeys.Num() == 0)
		{
			Diagnostics.Error(TEXT("DFX4038"), Value.Location,
				FString::Printf(TEXT("The curve for '%s' has no keys."), *DisplayName));
			return false;
		}

		// Every key field is written explicitly, tangents included. Plan 3.5 is blunt about why:
		// dropping tangents loses the shape, and losing the shape is losing data.
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FCurveKey& Key : Value.CurveKeys)
		{
			bool bRecognised = false;
			const TCHAR* InterpMode = InterpModeToEnum(Key.Interpolation, bRecognised);
			if (!bRecognised)
			{
				Diagnostics.Error(TEXT("DFX4039"), Key.Location,
					FString::Printf(TEXT("Unknown curve interpolation '%s'. Expected Auto, Cubic, Linear or Constant."),
						*Key.Interpolation));
				return false;
			}

			TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
			KeyObject->SetStringField(TEXT("InterpMode"), InterpMode);
			// An explicit tangent is only honoured in User tangent mode; without it the curve
			// re-derives its own and the authored shape is quietly discarded.
			KeyObject->SetStringField(TEXT("TangentMode"),
				(Key.bHasArrive || Key.bHasLeave) ? TEXT("RCTM_User") : TEXT("RCTM_Auto"));
			KeyObject->SetStringField(TEXT("TangentWeightMode"), TEXT("RCTWM_WeightedNone"));
			KeyObject->SetNumberField(TEXT("Time"), Key.Time);
			KeyObject->SetNumberField(TEXT("Value"), Key.Value);
			KeyObject->SetNumberField(TEXT("ArriveTangent"), Key.bHasArrive ? Key.ArriveTangent : 0.0f);
			KeyObject->SetNumberField(TEXT("LeaveTangent"), Key.bHasLeave ? Key.LeaveTangent : 0.0f);
			Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
		}

		TSharedRef<FJsonObject> Curve = MakeShared<FJsonObject>();
		Curve->SetArrayField(TEXT("Keys"), Keys);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		if (Channels == 1)
		{
			Root->SetObjectField(TEXT("Curve"), Curve);
		}
		else
		{
			// The multi-channel interfaces hold one FRichCurve per component. A single-valued literal
			// feeds every channel the same shape, which is what "one curve" means for them.
			static const TCHAR* const ChannelNames[] = { TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve"), TEXT("WCurve") };
			for (int32 Index = 0; Index < Channels; ++Index)
			{
				Root->SetObjectField(ChannelNames[Index], Curve);
			}
		}

		FString Result;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(Root, Writer);
		OutJson = Result;
		return true;
	}
}

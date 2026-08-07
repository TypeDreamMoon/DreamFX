#include "DreamFXTypes.h"

#include "Misc/SecureHash.h"

namespace UE::DreamFX
{
	const TCHAR* LexStackKind(EStackKind Kind)
	{
		switch (Kind)
		{
		case EStackKind::SystemSpawn:     return TEXT("SystemSpawn");
		case EStackKind::SystemUpdate:    return TEXT("SystemUpdate");
		case EStackKind::EmitterSpawn:    return TEXT("EmitterSpawn");
		case EStackKind::EmitterUpdate:   return TEXT("EmitterUpdate");
		case EStackKind::ParticleSpawn:   return TEXT("ParticleSpawn");
		case EStackKind::ParticleUpdate:  return TEXT("ParticleUpdate");
		case EStackKind::SimulationStage: return TEXT("Stage");
		case EStackKind::EventHandler:    return TEXT("OnEvent");
		default:                          return TEXT("<unknown>");
		}
	}

	bool ParseStackKind(const FString& Text, EStackKind& OutKind)
	{
		static const TPair<const TCHAR*, EStackKind> Table[] =
		{
			{ TEXT("SystemSpawn"),    EStackKind::SystemSpawn },
			{ TEXT("SystemUpdate"),   EStackKind::SystemUpdate },
			{ TEXT("EmitterSpawn"),   EStackKind::EmitterSpawn },
			{ TEXT("EmitterUpdate"),  EStackKind::EmitterUpdate },
			{ TEXT("ParticleSpawn"),  EStackKind::ParticleSpawn },
			{ TEXT("ParticleUpdate"), EStackKind::ParticleUpdate },
		};

		for (const TPair<const TCHAR*, EStackKind>& Entry : Table)
		{
			if (Text == Entry.Key)
			{
				OutKind = Entry.Value;
				return true;
			}
		}
		return false;
	}

	bool IsSystemScopeStack(EStackKind Kind)
	{
		return Kind == EStackKind::SystemSpawn || Kind == EStackKind::SystemUpdate;
	}

	const TCHAR* LexDocumentKind(EDocumentKind Kind)
	{
		switch (Kind)
		{
		case EDocumentKind::System:       return TEXT("System");
		case EDocumentKind::Emitter:      return TEXT("Emitter");
		case EDocumentKind::Module:       return TEXT("Module");
		case EDocumentKind::DynamicInput: return TEXT("DynamicInput");
		default:                          return TEXT("<unknown>");
		}
	}

	FValuePtr FValue::Make(EValueKind InKind, const FSourceLocation& Loc)
	{
		FValuePtr Node = MakeShared<FValue>();
		Node->Kind = InKind;
		Node->Location = Loc;
		return Node;
	}

	FValuePtr FValue::MakeNumber(double InNumber, bool bInteger, const FSourceLocation& Loc)
	{
		FValuePtr Node = Make(EValueKind::Number, Loc);
		Node->Number = InNumber;
		Node->bIsIntegerLiteral = bInteger;
		return Node;
	}

	FValuePtr FValue::MakeBool(bool bInValue, const FSourceLocation& Loc)
	{
		FValuePtr Node = Make(EValueKind::Bool, Loc);
		Node->bBool = bInValue;
		return Node;
	}

	FValuePtr FValue::MakeString(const FString& InText, const FSourceLocation& Loc)
	{
		FValuePtr Node = Make(EValueKind::String, Loc);
		Node->Text = InText;
		return Node;
	}

	FValuePtr FValue::MakeName(const FString& InText, const FSourceLocation& Loc)
	{
		FValuePtr Node = Make(EValueKind::Name, Loc);
		Node->Text = InText;
		return Node;
	}

	bool FValue::IsLiteral() const
	{
		switch (Kind)
		{
		case EValueKind::Number:
		case EValueKind::Bool:
		case EValueKind::String:
			return true;
		case EValueKind::Vector:
			for (const FValuePtr& Element : Elements)
			{
				if (!Element.IsValid() || !Element->IsLiteral())
				{
					return false;
				}
			}
			return true;
		default:
			return false;
		}
	}

	FString FValue::ToSourceString() const
	{
		switch (Kind)
		{
		case EValueKind::Number:
			// Integer literals must not gain a ".0" here: L7 keys the int-vs-float distinction off the
			// written form, and round-tripping through this function has to preserve it.
			return bIsIntegerLiteral
				? FString::Printf(TEXT("%lld"), static_cast<int64>(Number))
				: FString::SanitizeFloat(Number);

		case EValueKind::Bool:
			return bBool ? TEXT("true") : TEXT("false");

		case EValueKind::String:
			return FString::Printf(TEXT("\"%s\""), *Text.ReplaceCharWithEscapedChar());

		case EValueKind::Name:
			return Text;

		case EValueKind::Vector:
		{
			TArray<FString> Parts;
			for (const FValuePtr& Element : Elements)
			{
				Parts.Add(Element.IsValid() ? Element->ToSourceString() : TEXT("<null>"));
			}
			return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
		}

		case EValueKind::Array:
		{
			TArray<FString> Parts;
			for (const FValuePtr& Element : Elements)
			{
				Parts.Add(Element.IsValid() ? Element->ToSourceString() : TEXT("<null>"));
			}
			return FString::Printf(TEXT("[%s]"), *FString::Join(Parts, TEXT(", ")));
		}

		case EValueKind::Call:
		{
			TArray<FString> Parts;
			for (const FValuePtr& Element : Elements)
			{
				Parts.Add(Element.IsValid() ? Element->ToSourceString() : TEXT("<null>"));
			}
			for (const FNamedArgument& Argument : Arguments)
			{
				Parts.Add(FString::Printf(TEXT("%s = %s"), *Argument.Name,
					Argument.Value.IsValid() ? *Argument.Value->ToSourceString() : TEXT("<null>")));
			}
			return FString::Printf(TEXT("%s(%s)"), *Text, *FString::Join(Parts, TEXT(", ")));
		}

		case EValueKind::Hlsl:
			return FString::Printf(TEXT("hlsl {%s}"), *Text);

		case EValueKind::Curve:
		{
			FString Result = TEXT("curve { ");
			for (const FCurveKey& Key : CurveKeys)
			{
				Result += FString::Printf(TEXT("%s -> %s; "),
					*FString::SanitizeFloat(Key.Time), *FString::SanitizeFloat(Key.Value));
			}
			return Result + TEXT("}");
		}

		case EValueKind::Negate:
			return FString::Printf(TEXT("-%s"), Left.IsValid() ? *Left->ToSourceString() : TEXT("<null>"));

		case EValueKind::Binary:
			return FString::Printf(TEXT("(%s %s %s)"),
				Left.IsValid() ? *Left->ToSourceString() : TEXT("<null>"), *Text,
				Right.IsValid() ? *Right->ToSourceString() : TEXT("<null>"));

		default:
			return TEXT("<value>");
		}
	}

	const FAttribute* FParameterDecl::FindAttribute(const TCHAR* Key) const
	{
		return Attributes.FindByPredicate([Key](const FAttribute& Attribute)
		{
			return Attribute.Key.Equals(Key, ESearchCase::IgnoreCase);
		});
	}

	bool FParameterDecl::HasAttribute(const TCHAR* Key) const
	{
		return FindAttribute(Key) != nullptr;
	}

	const FStack* FEmitter::FindStack(EStackKind Kind) const
	{
		return Stacks.FindByPredicate([Kind](const FStack& Stack) { return Stack.Kind == Kind; });
	}

	const FProperty* FDocument::FindSetting(const TCHAR* SettingName) const
	{
		return Settings.FindByPredicate([SettingName](const FProperty& Property)
		{
			return Property.Name.Equals(SettingName, ESearchCase::IgnoreCase);
		});
	}

	const FStack* FDocument::FindStack(EStackKind InKind) const
	{
		return Stacks.FindByPredicate([InKind](const FStack& Stack) { return Stack.Kind == InKind; });
	}

	FString HashSourceText(const FString& SourceText)
	{
		// Line endings are normalised out of the hash so that a git checkout flipping CRLF/LF does not
		// invalidate every provenance stamp in the project and force a full rebuild.
		FString Normalized = SourceText;
		Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		Normalized.ReplaceInline(TEXT("\r"), TEXT("\n"), ESearchCase::CaseSensitive);

		const FTCHARToUTF8 Utf8(*Normalized);

		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

		uint8 Digest[16];
		Md5.Final(Digest);

		return BytesToHex(Digest, sizeof(Digest));
	}
}

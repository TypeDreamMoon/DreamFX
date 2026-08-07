#pragma once

#include "CoreMinimal.h"
#include "DreamFXDiagnostics.h"

/**
 * DreamFXLang AST.
 *
 * Deliberately reflection-free plain C++: nothing here is a USTRUCT. The AST never crosses a
 * serialisation boundary -- text goes in, Niagara assets come out -- and keeping UHT out of the
 * runtime module keeps its build cheap.
 *
 * Every node carries the source location of its first token so that DFX6xxx diagnostics (Niagara
 * compile events, which only know about stack references) can be mapped back to a line and column.
 */
namespace UE::DreamFX
{
	/** Which of the six script stacks a statement block targets. */
	enum class EStackKind : uint8
	{
		SystemSpawn,
		SystemUpdate,
		EmitterSpawn,
		EmitterUpdate,
		ParticleSpawn,
		ParticleUpdate,

		/** Reserved: named GPU simulation stage (`Stage <name> = {}`). Parsed, rejected at lowering in v1. */
		SimulationStage,
		/** Reserved: event handler stack (`OnEvent <name> = {}`). Parsed, rejected at lowering in v1. */
		EventHandler,
	};

	DREAMFX_API const TCHAR* LexStackKind(EStackKind Kind);
	DREAMFX_API bool ParseStackKind(const FString& Text, EStackKind& OutKind);
	/** True for the two stacks that live at system scope and are written at the top level of a .dfs. */
	DREAMFX_API bool IsSystemScopeStack(EStackKind Kind);

	enum class EValueKind : uint8
	{
		/** Numeric literal. bIsIntegerLiteral distinguishes `24` from `24.0` for the L7 conversion rules. */
		Number,
		Bool,
		/** Quoted string -- asset paths, descriptions. */
		String,
		/**
		 * A dotted name: `Once`, `Random_Range`, `User.SparkSpeed`, `Particles.Velocity`.
		 * Resolved into an enum literal or a linked parameter at lowering time, once the target
		 * input's type is known.
		 */
		Name,
		/** `(1, 0.72, 0.25, 1)` -- component count decides the concrete Niagara type. */
		Vector,
		/** `[ "a", "b" ]` */
		Array,
		/** `Foo(A = 1, B = 2)` -- dynamic input, or an L6 builtin like `normalize(...)`. */
		Call,
		/** `hlsl { ... }` -- raw, unescaped body. */
		Hlsl,
		/** `curve { 0.0 -> 1.0; ... }` */
		Curve,
		/** `-X` */
		Negate,
		/** `A + B`, `A * B`, ... Text holds the operator. */
		Binary,
	};

	struct FValue;
	using FValuePtr = TSharedPtr<FValue>;

	/** `Name = Value` inside a call's argument list. */
	struct FNamedArgument
	{
		FString Name;
		FValuePtr Value;
		FSourceLocation Location;
	};

	/** One `0.7 -> 0.85 [ Interp=Cubic; Arrive=-0.4; Leave=-1.2 ]` entry of a curve literal. */
	struct FCurveKey
	{
		float Time = 0.0f;
		float Value = 0.0f;

		/** Auto | Cubic | Linear | Constant. Empty means Auto. */
		FString Interpolation;

		bool bHasArrive = false;
		float ArriveTangent = 0.0f;
		bool bHasLeave = false;
		float LeaveTangent = 0.0f;

		FSourceLocation Location;
	};

	struct FValue
	{
		EValueKind Kind = EValueKind::Number;
		FSourceLocation Location;

		/** Number / Bool payload. */
		double Number = 0.0;
		bool bBool = false;
		/** True when the literal was written without a decimal point or exponent (L7). */
		bool bIsIntegerLiteral = false;

		/** String payload, dotted name, HLSL body, call callee, or binary operator. */
		FString Text;

		/** Vector components, array elements, or positional call arguments. */
		TArray<FValuePtr> Elements;

		/** Named call arguments. */
		TArray<FNamedArgument> Arguments;

		/** Curve literal keys. */
		TArray<FCurveKey> CurveKeys;

		/** Binary operands (Kind == Binary), or the single operand of Negate in Left. */
		FValuePtr Left;
		FValuePtr Right;

		static DREAMFX_API FValuePtr MakeNumber(double InNumber, bool bInteger, const FSourceLocation& Loc);
		static DREAMFX_API FValuePtr MakeBool(bool bInValue, const FSourceLocation& Loc);
		static DREAMFX_API FValuePtr MakeString(const FString& InText, const FSourceLocation& Loc);
		static DREAMFX_API FValuePtr MakeName(const FString& InText, const FSourceLocation& Loc);
		static DREAMFX_API FValuePtr Make(EValueKind InKind, const FSourceLocation& Loc);

		/** True for Number/Bool/String/Vector -- values needing no schema knowledge to evaluate. */
		DREAMFX_API bool IsLiteral() const;

		/** Debug/round-trip rendering. Not a promise of byte-identical reproduction (see L6). */
		DREAMFX_API FString ToSourceString() const;
	};

	/** `[ Group="Burst"; SortPriority=10; StaticSwitch ]` -- a flag-only attribute has an empty Value. */
	struct FAttribute
	{
		FString Key;
		FValuePtr Value;
		FSourceLocation Location;
	};

	/** `Key = Value;` inside Settings or a renderer block. */
	struct FProperty
	{
		FString Name;
		FValuePtr Value;
		FSourceLocation Location;
	};

	/**
	 * One entry of a `Properties = {}` (user parameters) or `Inputs = {}` (module inputs) block:
	 * `float SparkCount = 24.0 [ Group="Burst" ]`.
	 */
	struct FParameterDecl
	{
		/** `float`, `Color`, `Vector`, `Texture2D`, `DI` ... */
		FString TypeName;
		/** Inner type of `DI<SkeletalMesh>`; empty otherwise. */
		FString InnerTypeName;
		FString Name;
		/** May be null: `DI<SkeletalMesh> TargetMesh [ Group="Bind" ]` declares without a default. */
		FValuePtr DefaultValue;
		TArray<FAttribute> Attributes;
		FSourceLocation Location;

		DREAMFX_API const FAttribute* FindAttribute(const TCHAR* Key) const;
		DREAMFX_API bool HasAttribute(const TCHAR* Key) const;
	};

	enum class EStatementKind : uint8
	{
		/** `ModuleName(Input = Value, ...)` */
		ModuleCall,
		/** `Namespace.Var = Value` */
		Assignment,
	};

	struct FStatement
	{
		EStatementKind Kind = EStatementKind::ModuleCall;
		FSourceLocation Location;

		/** Module name or path for ModuleCall; fully qualified assignment target otherwise. */
		FString Name;

		/** R7 `ModuleName@Version` pin. Empty when unpinned. */
		FString VersionPin;

		/** ModuleCall inputs. */
		TArray<FNamedArgument> Arguments;

		/** Assignment right-hand side. */
		FValuePtr Value;

		/** Innermost enclosing `#Region` label. v1 keeps this as text only (L5). */
		FString Region;
	};

	/** `Bind SpriteSize -> Particles.SpriteSize;` */
	struct FRendererBinding
	{
		FString PropertyName;
		FString Target;
		FSourceLocation Location;
	};

	struct FRenderer
	{
		/** `SpriteRenderer`, `MeshRenderer`, `RibbonRenderer`, ... */
		FString TypeName;
		/** Optional author-facing label. Not persisted to the asset -- renderers are addressed by index. */
		FString Name;
		TArray<FProperty> Properties;
		TArray<FRendererBinding> Bindings;
		/** Reserved `MaterialParam X = V;` entries (L8). Parsed, rejected at lowering in v1. */
		TArray<FProperty> MaterialParameters;
		FSourceLocation Location;
	};

	struct FStack
	{
		EStackKind Kind = EStackKind::ParticleUpdate;
		/** Stage / event handler name; empty for the six built-in stacks. */
		FString Name;
		TArray<FStatement> Statements;
		FSourceLocation Location;
	};

	struct FEmitter
	{
		FString Name;
		/** `Emitter Flash from "DFX/Emitters/E_MoonFlashCard"` -- copy semantics in v1 (R3). */
		FString FromPath;
		FSourceLocation FromLocation;

		TArray<FProperty> Settings;
		TArray<FStack> Stacks;
		TArray<FRenderer> Renderers;
		FSourceLocation Location;

		DREAMFX_API const FStack* FindStack(EStackKind Kind) const;
	};

	enum class EDocumentKind : uint8
	{
		System,
		Emitter,
		Module,
		DynamicInput,
	};

	DREAMFX_API const TCHAR* LexDocumentKind(EDocumentKind Kind);

	struct FDocument
	{
		EDocumentKind Kind = EDocumentKind::System;

		/** `Name="Systems/NS_ToonHitSpark"` -- package path relative to the resolved root. */
		FString Name;
		/** `Root="Plugin.MoonToon"` -- empty means the project content root. */
		FString Root;
		FSourceLocation HeaderLocation;

		/** Absolute path of the file this was parsed from. */
		FString SourceFilePath;
		/** Hash of the source text, for the 4.6 provenance stamp. */
		FString SourceHash;

		TArray<FProperty> Settings;

		/** `Properties = {}` for a system, `Inputs = {}` for a module / dynamic input. */
		TArray<FParameterDecl> Parameters;

		/** System-scope stacks (SystemSpawn / SystemUpdate). */
		TArray<FStack> Stacks;

		/** System documents only. */
		TArray<FEmitter> Emitters;

		/** Emitter documents (.dfe) only. */
		FEmitter EmitterDefinition;

		/** Module / dynamic input documents (.dfm) only: the raw `Body = { }` text. */
		FString Body;
		FSourceLocation BodyLocation;

		DREAMFX_API const FProperty* FindSetting(const TCHAR* SettingName) const;
		DREAMFX_API const FStack* FindStack(EStackKind InKind) const;
	};

	/** Stable hash of source text, used by the provenance stamp to skip unchanged rebuilds. */
	DREAMFX_API FString HashSourceText(const FString& SourceText);
}

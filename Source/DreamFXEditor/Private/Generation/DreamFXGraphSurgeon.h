#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"

class UEdGraphPin;
class UNiagaraGraph;
class UNiagaraNode;
class UNiagaraNodeCustomHlsl;
class UNiagaraScriptVariable;
struct FNiagaraTypeDefinition;
struct FNiagaraVariable;
struct FNiagaraVariableMetaData;

namespace UE::DreamFX::Editor
{
	/**
	 * The handful of Niagara graph operations a .dfm needs that a stock engine declares but does not
	 * export, behind one interface so the module generator does not have to know which engine it is
	 * running on.
	 *
	 * There are two implementations and three outcomes (plan-v7 S1):
	 *
	 *   1. MoonEngine carries NIAGARAEDITOR_API on all five declarations, so the direct implementation
	 *      simply calls them. This is the path the whole corpus is verified against and it is not
	 *      affected by anything in the reflection implementation -- the two never coexist in a build.
	 *   2. A stock engine exports none of them, but every *behaviour* they provide is reachable another
	 *      way: public data members need no export macro at all, public virtuals dispatch through the
	 *      vtable, `UEdGraph::CreateNode` takes a runtime UClass, and the three private fields that are
	 *      left are UPROPERTYs. The reflected implementation re-derives the five operations from those.
	 *   3. If the reflected implementation cannot confirm the shapes it depends on -- a renamed property,
	 *      a class that has moved -- it refuses to run rather than write a subtly wrong graph, and .dfm
	 *      generation degrades to the check-only behaviour it had before any of this existed.
	 *
	 * The reflected implementation is compiled on every engine, including MoonEngine, so that the state
	 * it produces can be diffed against the direct implementation's on an engine where both work.
	 */
	class FGraphSurgeon
	{
	public:
		virtual ~FGraphSurgeon() = default;

		/** A finalized parameter map get node in Graph, or null if the class could not be reached. */
		virtual UNiagaraNode* CreateParameterMapGet(UNiagaraGraph& Graph) = 0;

		/** A finalized parameter map set node in Graph, or null if the class could not be reached. */
		virtual UNiagaraNode* CreateParameterMapSet(UNiagaraGraph& Graph) = 0;

		/**
		 * A new typed pin on a node that takes dynamic pins, named and wired like the editor's own
		 * "drop a type on the Add pin" gesture. Null if the node would not accept one.
		 */
		virtual UEdGraphPin* AddTypedPin(UNiagaraNode& Node, EEdGraphPinDirection Direction,
			const FNiagaraTypeDefinition& Type, FName Name) = 0;

		/** Puts a body on a custom HLSL node and tells the graph the node changed. */
		virtual void SetCustomHlsl(UNiagaraNodeCustomHlsl& Node, const FString& Hlsl) = 0;

		/** Turns a bare custom HLSL node into a dynamic input producing OutputType. */
		virtual void InitAsDynamicInput(UNiagaraNodeCustomHlsl& Node, const FNiagaraTypeDefinition& OutputType) = 0;

		/**
		 * Registers a parameter on the graph and returns the script variable that carries its default
		 * and its description. Null if the graph refused it.
		 */
		virtual UNiagaraScriptVariable* AddParameter(UNiagaraGraph& Graph, const FNiagaraVariable& Variable,
			const FNiagaraVariableMetaData& MetaData) = 0;

		/** "direct" or "reflection" -- for the provenance stamp and for diagnostics. */
		virtual const TCHAR* Describe() const = 0;

		/**
		 * The state 1 / state 2 / state 3 choice, made once per process.
		 *
		 * Returns null only in state 3, with OutUnavailableReason naming the specific check that failed
		 * so the diagnostic can say which engine assumption broke rather than "unsupported".
		 */
		static TUniquePtr<FGraphSurgeon> Create(FString& OutUnavailableReason);
	};
}

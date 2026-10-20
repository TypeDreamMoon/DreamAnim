// Copyright DreamMoon. All Rights Reserved.
//
// The semantic pass for .dcr, inside: what pins and signatures are, the value an expression has, and the
// builder of one graph. DcrSemantic.h is the entry; the .cpp files here split the work by what they read:
// the document (DcrSemantic.cpp), the hierarchy (DcrSemanticHierarchy.cpp), what nodes look like
// (DcrSemanticSignature.cpp), values and links (DcrSemanticValue.cpp), expressions (DcrSemanticExpr.cpp)
// and statements (DcrSemanticCode.cpp).

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Lang/DabDiagnostic.h"
#include "Model/DcrModel.h"
#include "Rig/DcrNames.h"
#include "Rig/DcrSemantic.h"
#include "Rig/DcrSyntax.h"
#include "Rig/DcrValues.h"

class FProperty;
class UClass;
class UObject;
class UScriptStruct;
struct FRigVMDispatchFactory;
struct FRigVMTemplate;

namespace UE::DreamAnim::Rig::Sem
{
	// ------------------------------------------------------------------------------------------
	// Pins and signatures
	// ------------------------------------------------------------------------------------------

	/** A pin of a node as the text sees it. Execute and hidden pins are not pins here. */
	struct FPin
	{
		FString Name;
		bool bInput = false;
		bool bOutput = false;
		/** Unset for a wildcard that is not resolved yet. */
		Model::FDcrType Type;
		/** A unit's member; null for the pins of everything else. */
		const FProperty* Property = nullptr;
		/** A dispatch argument that takes several types: its resolved type is listed in the node, and its value always kept. */
		bool bWildcard = false;
		/** The pin an aggregate node grows by (its first two inputs). */
		bool bAggregate = false;
		/** The value a fresh node holds, in export text; empty is the type's default. */
		FString Default;
	};

	struct FSignature
	{
		TArray<FPin> Pins;
		/** The execute pins by root pin name: those execution comes in by, those it leaves by (an IO pin is in both). */
		TArray<FString> ExecIn;
		TArray<FString> ExecOut;

		const FPin* Find(const FString& Name) const;
		FPin* Find(const FString& Name);
		bool IsMutable() const { return !ExecIn.IsEmpty() || !ExecOut.IsEmpty(); }
		int32 NumInputs() const;
		int32 NumOutputs() const;
		/** The nth pin that takes a value, in declaration order. */
		const FPin* GetInput(int32 Index) const;
	};

	/** A dispatch factory and its template. */
	struct FDispatchInfo
	{
		const UScriptStruct* Struct = nullptr;
		const FRigVMDispatchFactory* Factory = nullptr;
		const FRigVMTemplate* Template = nullptr;
	};

	/** The pins of a unit by reflection: execute pins apart, every other member that is not hidden. */
	FSignature MakeUnitSignature(const UScriptStruct& Struct);
	/** A dispatch's pins from its template's arguments; wildcard pins have no type yet. */
	FSignature MakeDispatchSignature(const UScriptStruct& Struct);
	bool GetDispatchInfo(const UScriptStruct& Struct, FDispatchInfo& Out);
	/** The notation of the template a unit is a permutation of; empty for a unit that is not. */
	FString GetUnitNotation(const UScriptStruct& Struct);
	/** The unit or dispatch struct named by its full name (`RigVMFunction_ControlFlowBranch`); null when there is none. */
	const UScriptStruct* FindUnitByFullName(const TCHAR* FullName);
	const UScriptStruct* FindDispatchByFullName(const TCHAR* FullName);

	/** The second aggregate input of a unit that aggregate nodes are made of (`B` for Add); empty when the unit is not one. */
	FString GetUnitAggregateSecond(const UScriptStruct& Struct);
	/** `B` -> `C`, `Z`-less letters and numbers the engine's way. */
	FString GetNextAggregateName(const FString& Last);

	enum class EResolveResult : uint8
	{
		/** Every wildcard has a type. */
		Resolved,
		/** Some have not. */
		Open,
		/** The types given fit no permutation. */
		Mismatch,
	};

	/**
	 * Resolves a dispatch's template from the types known for some of its arguments (by name). OutTypes holds a
	 * type for every argument it could settle.
	 */
	EResolveResult ResolveTemplateTypes(const FRigVMTemplate& Template, const TMap<FString, Model::FDcrType>& Known, TMap<FString, Model::FDcrType>& OutTypes);

	int32 GetTypeIndexOf(const Model::FDcrType& Type);
	Model::FDcrType GetTypeFromIndex(int32 TypeIndex);
	bool IsArrayType(const Model::FDcrType& Type);
	/** Whether a value of From can be linked to a pin of To: the same type, or a float and a double. Unset types fit anything. */
	bool AreTypesCompatible(const Model::FDcrType& From, const Model::FDcrType& To);
	/** The type of a member path (`Translation.X`, `0.Item`) below a value of Type, and the same path with member names as the engine spells them. */
	bool GetTypeAtSubPath(const Model::FDcrType& Type, const FString& Path, Model::FDcrType& OutType, FString& OutEnginePath);
	/** `Get Transform 2` -> `Get_Transform_2`: a node name that has no dots or spaces. */
	FString MakeNodeBaseName(const FString& Name);
	/** The expression inside the parentheses the author wrote around it. */
	const Lang::FDabExpr& StripParens(const Lang::FDabExpr& Expr);
	/** The name of a function's, a variable setter's and a collapse node's execute pin. */
	extern const TCHAR* const DefaultExecuteName;

	// ------------------------------------------------------------------------------------------
	// Values
	// ------------------------------------------------------------------------------------------

	struct FNodeRec;

	/** What an expression has been evaluated to. */
	struct FVal
	{
		enum class EKind : uint8
		{
			/** Reported already, or nothing. */
			Invalid,
			/** A pin of a node. */
			Pin,
			/** A constant, not typed yet: its expression, written into whatever it goes to. */
			Literal,
			/** A call that gives nothing. */
			Void,
		};

		EKind Kind = EKind::Invalid;
		FNodeRec* Rec = nullptr;
		/** The root output pin; empty when the node has several outputs and none was picked. */
		FString Pin;
		/** Below the root pin, with the engine's member names. */
		FString Sub;
		/** Of the value. Unset while a wildcard is open. */
		Model::FDcrType Type;
		const Lang::FDabExpr* Expr = nullptr;
		/** An index into the body's pending dispatches whose types are not settled. */
		int32 Pending = INDEX_NONE;
		Lang::FDabSpan Span;

		bool IsPin() const { return Kind == EKind::Pin; }
		FString GetPath() const;
	};

	/** A node being built; its model node joins the graph when the body is finished. */
	struct FNodeRec
	{
		Model::FDcrModelNode Node;
		FSignature Sig;
		int32 Pending = INDEX_NONE;
	};

	// ------------------------------------------------------------------------------------------
	// The document's names
	// ------------------------------------------------------------------------------------------

	struct FFunctionDecl
	{
		const Lang::FDabNamedBlockStmt* Stmt = nullptr;
		FString Name;
		bool bInline = false;
		bool bMutable = true;
		bool bPublic = false;
		/** The signature's types all resolved. */
		bool bValid = true;
		TArray<Model::FDcrModelFunctionPin> Inputs;
		TArray<Model::FDcrModelFunctionPin> Outputs;
		TArray<Model::FDcrModelVariable> Locals;
		/** Inline: how many calls were made. */
		int32 Calls = 0;
		/** The index in Model.Functions; none for an inline function. */
		int32 ModelIndex = INDEX_NONE;
	};

	struct FExternalFunction
	{
		Model::FDcrModelFunction Function;
		/** The execute pin the function's nodes show. */
		FString ExecuteName;
	};

	/** A rig named in `Uses`. */
	struct FLibrary
	{
		/** The asset's object path, which is the host of its functions. */
		FString Host;
		/** The asset's name, which qualifies its functions (`StandardFunctionLibrary.Func`). */
		FString Name;
		TArray<FExternalFunction> Public;
		TArray<FString> PrivateNames;
	};

	struct FGlobals
	{
		FGlobals(const Lang::FDabCompilationUnit& InUnit, Lang::FDabDiagnosticSink& InSink, Model::FDcrModel& InModel)
			: Unit(InUnit), Sink(InSink), Model(InModel)
		{
		}

		const Lang::FDabCompilationUnit& Unit;
		Lang::FDabDiagnosticSink& Sink;
		Model::FDcrModel& Model;
		TArray<TUniquePtr<FFunctionDecl>> Functions;
		TArray<TUniquePtr<FLibrary>> Libraries;

		const Model::FDcrModelVariable* FindVariable(const FString& Name) const;
		FFunctionDecl* FindFunction(const FString& Name) const;
		FLibrary* FindLibrary(const FString& Name) const;

		/**
		 * The object path an asset expression names: a bare name (one registered asset of that name, of Class or
		 * a subclass), or `Path(Root, "Rel")`. Empty, with OutProblem set, when there is none or several.
		 */
		FString ResolveAssetPath(const Lang::FDabExpr& Expr, const UClass* Class, FString& OutProblem, bool& bOutAmbiguous) const;
	};

	/** What the value codec reports when it refuses a literal, as a diagnostic code. */
	const TCHAR* ClassifyValueError(const FString& Error);

	// ------------------------------------------------------------------------------------------
	// A graph being built
	// ------------------------------------------------------------------------------------------

	/** A dispatch whose types may not be settled when its call is read: its arguments wait here. */
	struct FPendingDispatch
	{
		FNodeRec* Rec = nullptr;
		const UScriptStruct* Struct = nullptr;
		TArray<TPair<FString, FVal>> Args;
		/** The types the arguments are known to have, by pin. */
		TMap<FString, Model::FDcrType> Known;
		Lang::FDabSpan Span;
		bool bResolved = false;
		bool bFailed = false;
	};

	class FBody
	{
	public:
		FBody(FGlobals& InGlobals, Model::FDcrModelGraph& InGraph, FFunctionDecl* InFunction, const FString& InWhere);

		/** A function's graph: the Entry and Return nodes, the parameters as names. */
		void BeginFunction();
		/** Runs the function's code (mutable) or builds its value (pure) and joins the chain to Return. */
		void RunFunction();
		/** An event: its node, and the chain that starts at it. False when the event cannot be made. */
		bool BeginEvent(const Lang::FDabNamedBlockStmt& Stmt, const FString& Word);
		void RunCode(const Lang::FDabCodeBlock& Code);
		/** Every let of Code, in any block, before any node is made. */
		void ReserveLetNames(const Lang::FDabCodeBlock& Code);
		/** The comments above Node as comment boxes around the nodes made since FirstRec (none: boxes on their own). */
		void AddCommentBoxes(const Lang::FDabNode& Node, int32 FirstRec);
		int32 NumRecs() const { return Recs.Num(); }
		/** Detached code at the top of a graph: nothing runs into its first statement. */
		void BeginDetached();
		/** Moves the nodes into the graph; reports the wildcards still open. */
		void Finish();

		/** Whether a name stands for a let, a loop name, a parameter, a local or a member variable here. */
		bool HasName(const FString& Name) const;

		/** The events made so far, by word: a second one of the same is DAB9010. */
		TSet<FString> EventWords;

		/** Entry and Return of a function body; the collapse node's interface reads them. */
		FNodeRec* EntryRec = nullptr;
		FNodeRec* ReturnRec = nullptr;

		FGlobals& G;
		Lang::FDabDiagnosticSink& Sink;
		Model::FDcrModelGraph& Graph;
		FFunctionDecl* Function;
		/** The body an Inline function's body is called from: a collapse node reads its function's locals. */
		const FBody* Outer = nullptr;
		/** A local of this function, or of the function an Inline body is called from. */
		const Model::FDcrModelVariable* FindLocal(const FString& Name) const;
		FString Where;

	private:
		struct FScope
		{
			TMap<FString, FVal> Names;
		};

		TArray<TUniquePtr<FNodeRec>> Recs;
		TArray<TUniquePtr<FPendingDispatch>> Pendings;
		TArray<FScope> Scopes;
		TSet<FString> UsedNames;
		/** The names the code's lets give their nodes: a node named after its kind does not take one first. */
		TSet<FString> LetNames;
		TMap<FString, int32> NameCounters;
		/** The execute pin (`Node.Pin`) the next statement runs from; empty at the head of detached code. */
		FString Cursor;
		/** The statement being run, for messages about the nodes it makes. */
		Lang::FDabSpan StatementSpan;
		/** Regions open in the code being run: the label, and how many nodes were made before it. */
		struct FRegionStart
		{
			FString Label;
			int32 FirstRec = 0;
		};
		TArray<FRegionStart> Regions;
		/** The function's `return` was written (its last statement). */
		bool bReturned = false;

		// -- Nodes and links (DcrSemanticCode.cpp)
		FNodeRec& NewRec(const FString& Preferred, const FString& Fallback);
		FString ReserveName(const FString& Preferred, const FString& Fallback);
		void AddLink(const FString& Source, const FString& Target);
		void Sequence(FNodeRec& Rec);
		FVal MakeOutput(FNodeRec& Rec, const Lang::FDabSpan& Span);
		FVal MakeAbsent(const Lang::FDabSpan& Span) const;
		void PushScope();
		void PopScope();
		void Bind(const FString& Name, const FVal& Value);
		const FVal* LookupScope(const FString& Name) const;

		// -- Statements (DcrSemanticCode.cpp)
		void RunBlock(const Lang::FDabCodeBlock& Code, bool bFunctionTop);
		void RunStatement(const Lang::FDabStmt& Stmt, bool bLast, bool bFunctionTop);
		void RunStatementKind(const Lang::FDabStmt& Stmt, bool bLast, bool bFunctionTop);
		void RunLet(const Lang::FDabLetStmt& Stmt);
		void RunSet(const Lang::FDabSetStmt& Stmt);
		void RunExpression(const Lang::FDabExprStmt& Stmt);
		/** The blocks after a statement's call: each runs from the execute output it names; execution then goes on from the call. */
		void RunFlowBlocks(FNodeRec* Rec, const TArray<Lang::FDabFlowBlock>& Blocks, const Lang::FDabSpan& CallSpan);
		void RunIf(const Lang::FDabIfStmt& Stmt);
		void RunFor(const Lang::FDabForStmt& Stmt);
		void RunReturn(const Lang::FDabReturnStmt& Stmt, bool bLast, bool bFunctionTop);
		void RunDetached(const Lang::FDabDetachedStmt& Stmt);
		void RunRegion(const Lang::FDabRegionStmt& Stmt);
		void CloseRegions(int32 Depth);
		void SetVariable(const Lang::FDabSetStmt& Stmt, const FString& Name, const Lang::FDabExpr& Value);
		void SetArrayElement(const Lang::FDabSetStmt& Stmt, const FString& Name, const Lang::FDabExpr& Index, const Lang::FDabExpr& Value);
		void JoinReturn();

		// -- Expressions (DcrSemanticExpr.cpp)
		FVal EvalOperand(const Lang::FDabExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal Eval(const Lang::FDabExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal EvalName(const Lang::FDabNameExpr& Expr);
		/** Whether Name, written where Enum goes, is the enum's value: it is one, and nothing named so here is of the enum. */
		bool IsEnumValueHere(const FString& Name, const UEnum& Enum) const;
		FVal EvalMember(const Lang::FDabMemberExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal EvalCall(const Lang::FDabCallExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal EvalUnary(const Lang::FDabUnaryExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal EvalBinary(const Lang::FDabBinaryExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal EvalConditional(const Lang::FDabConditionalExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal EvalIndex(const Lang::FDabIndexExpr& Expr, const Model::FDcrType* Expected, const FString& Preferred);
		FVal SelectMember(const FVal& Base, const FString& Member, const Lang::FDabSpan& Span);
		FVal MakeVariableGet(const Model::FDcrModelVariable& Variable, bool bLocal, const FString& Preferred, const Lang::FDabSpan& Span);
		bool IsLiteralShape(const Lang::FDabExpr& Expr) const;

		struct FCallee
		{
			enum class EKind : uint8 { None, Function, External, Unit, Dispatch, Template, Injected, Range, RunEvent };
			EKind Kind = EKind::None;
			FFunctionDecl* Function = nullptr;
			FLibrary* Library = nullptr;
			const FExternalFunction* External = nullptr;
			const UScriptStruct* Struct = nullptr;
			TArray<FName> Notations;
		};
		bool ResolveCallee(const Lang::FDabCallExpr& Call, FCallee& Out);

		struct FBoundArg
		{
			FString Pin;
			const Lang::FDabExpr* Expr = nullptr;
			const Lang::FDabArgument* Source = nullptr;
		};
		/** Skip: arguments another step takes (a unit's traits). */
		bool BindArgs(const Lang::FDabCallExpr& Call, const FSignature& Sig, const FString& What, bool bAggregate, TArray<FBoundArg>& OutArgs, TArray<const Lang::FDabExpr*>& OutExtra,
			const TSet<const Lang::FDabArgument*>* Skip = nullptr);

		FVal BuildUnit(const Lang::FDabCallExpr& Call, const UScriptStruct& Struct, const Model::FDcrType* Expected, const FString& Preferred);
		/** `RunEvent(Backward)`: an InvokeEntry node on the chain. */
		FVal BuildRunEvent(const Lang::FDabCallExpr& Call, const FString& Preferred);
		FVal BuildUnitFrom(const UScriptStruct& Struct, const TArray<TPair<FString, FVal>>& Args, const FString& Preferred, const Lang::FDabSpan& Span);
		FVal BuildDispatch(const Lang::FDabCallExpr& Call, const UScriptStruct& Struct, const Model::FDcrType* Expected, const FString& Preferred);
		FVal BuildDispatchFrom(const UScriptStruct& Struct, TArray<TPair<FString, FVal>> Args, const Model::FDcrType* Expected, const FString& Preferred, const Lang::FDabSpan& Span);
		FVal BuildTemplate(const Lang::FDabCallExpr& Call, const FCallee& Callee, const Model::FDcrType* Expected, const FString& Preferred);
		/** A template call whose arguments every version takes: the template node, no type picked (a warning says so). */
		FVal BuildUnresolvedTemplate(const Lang::FDabCallExpr& Call, const FName& Notation, const UScriptStruct& First, const TArray<TPair<FString, FVal>>& Args,
			const FString& Preferred);
		FVal BuildFunction(const Lang::FDabCallExpr& Call, const FCallee& Callee, const Model::FDcrType* Expected, const FString& Preferred);
		FVal BuildInline(const Lang::FDabCallExpr& Call, FFunctionDecl& Decl, const FString& Preferred);
		void BuildInjected(const Lang::FDabCallExpr& Wrapper, FNodeRec& Host, const FPin& HostPin);
		/** `Injected(Node(Other.Pin), Output)`: the node injected on the output that feeds it. */
		FVal BuildInjectedOutput(const Lang::FDabCallExpr& Wrapper, const Model::FDcrType* Expected, const FString& Preferred);
		void FinishDispatch(FPendingDispatch& Pending, const TMap<FString, Model::FDcrType>& Types);
		bool SettlePending(FVal& Value, const Model::FDcrType* Target);
		void Settle(FVal& Value);
		bool AddAggregates(FNodeRec& Rec, const UScriptStruct& Struct, const FSignature& Sig, const TArray<const Lang::FDabExpr*>& Extra);

		// -- Values (DcrSemanticValue.cpp)
		/** Evaluates Expr for a pin of Rec and connects it: a link, or a value. */
		void ApplyArg(FNodeRec& Rec, const FPin& Pin, const Lang::FDabExpr& Expr);
		void ApplyValue(FNodeRec& Rec, const FPin& Pin, FVal Value, const Lang::FDabSpan& Span);
		void ApplyLiteral(FNodeRec& Rec, const FPin& Pin, const Lang::FDabExpr& Expr);
		void ApplyDefault(FNodeRec& Rec, const FPin& Pin);
		/** Span: where one literal in the text says all of the value, for write-back; empty otherwise. */
		void SetPinValue(FNodeRec& Rec, const FString& PinName, const FString& Text, const Lang::FDabSpan& Span = Lang::FDabSpan());
		void LinkValue(const FVal& Value, const FString& TargetPath, const Model::FDcrType& TargetType, const Lang::FDabSpan& Span);
		bool IsConstantLeaf(const Lang::FDabExpr& Expr, const FProperty& Property) const;
		UObject* ResolveAssetLeaf(const Lang::FDabExpr& Expr, const UClass* Expected);
		bool FitsType(const Lang::FDabExpr& Expr, const Model::FDcrType& Type);
		/** Whether the value is the type's own default (nothing to keep). */
		static bool IsTypeDefaultText(const Model::FDcrType& Type, const FString& Text);

		bool bLastAssetAmbiguous = false;
		FString LastAssetProblem;
	};

	// ------------------------------------------------------------------------------------------
	// Sections (DcrSemantic.cpp, DcrSemanticHierarchy.cpp)
	// ------------------------------------------------------------------------------------------

	/** The asset path a document's header names; empty when the header has none. */
	FString MakeAssetPath(const FString& Name, const FString& Root);

	void ReadHierarchy(FGlobals& G, const Lang::FDabBlockExpr& Block, const FString& PreviewMeshPath);
	void ReadModuleSettings(FGlobals& G, const Lang::FDabBlockExpr& Block);
	void ReadVariablesBlock(FGlobals& G, const Lang::FDabBlockExpr& Block);
	void ReadSettings(FGlobals& G, const Lang::FDabBlockExpr& Block, FString& OutPreviewMeshPath);
	/** Resolves a type spelling; reports DAB9001 at Span and returns false when it names none. */
	bool ResolveTypeName(FGlobals& G, const FString& Spelling, const Lang::FDabSpan& Span, Model::FDcrType& OutType);
	/** A constant for a declaration (a variable's or parameter's default) as export text, empty for the type's default; false, reported, when it is not one. */
	bool ReadConstantText(FGlobals& G, const Model::FDcrType& Type, const Lang::FDabExpr& Expr, FString& OutText);
	/** Writes a constant over the value at Memory through reflection; false, reported, when it is not one or does not fit. */
	bool WriteConstantValue(FGlobals& G, const FProperty& Property, void* Memory, const Lang::FDabExpr& Expr);
	/** The string a metadata entry holds; false, reported, when it holds none. */
	bool ReadStringMeta(FGlobals& G, const Lang::FDabMetaEntry& Entry, FString& OutText);
}

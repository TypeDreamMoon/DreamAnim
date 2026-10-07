// Copyright DreamMoon. All Rights Reserved.
//
// What the write-back files share: the text being edited (a fresh parse of the source, with its comments,
// whose nodes are found by the spans the compiled tree recorded), and the comparison context.

#pragma once

#include "CoreMinimal.h"
#include "Driver/DabDriver.h"
#include "Generate/DabGenerate.h"
#include "Lang/DabAst.h"
#include "Lang/DabPrinter.h"
#include "Lower/DabLower.h"
#include "Model/DabModel.h"
#include "Semantic/DabSemantic.h"
#include "WriteBack/DabWriteBack.h"

namespace UE::DreamAnim::Compiler::WriteBackPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	/**
	 * The source as a tree that can be changed and printed back in pieces. Edits are recorded against the
	 * original spans and printed from the tree when the text is made, so an edit inside a reprinted region
	 * is carried by the reprint.
	 */
	class FEditableText
	{
	public:
		FEditableText(const FString& Path, const FString& Text);

		bool IsValid() const { return File.IsValid() && File->Document.IsValid(); }
		FDabFile& GetFile() { return *File; }
		const FString& GetText() const { return Source.GetText(); }

		/** The node at exactly Span of Kind in this text, or null. */
		FDabNode* Find(const FDabSpan& Span, EDabNodeKind Kind) const;
		/**
		 * The node of this text that Compiled (a node of the compiled, template-expanded tree) was parsed
		 * from: same span, same kind, same one-line text. Null for a node a template body made.
		 */
		FDabNode* FindSame(const FDabNode& Compiled) const;
		/** The node a node sits in, or null for the document. */
		FDabNode* GetParent(const FDabNode& Node) const;

		/** The text of Node's original span is replaced by Printed's canonical text (Printed may be Node itself, changed). */
		void ReprintExpr(const FDabSpan& OriginalSpan, const FDabExpr& Printed);
		void ReprintStmt(const FDabSpan& OriginalSpan, const FDabStmt& Printed);
		/** Printed goes on its own lines just before the `}` that closes Block. */
		void AppendStmt(const FDabBlockExpr& Block, const FDabStmt& Printed);
		/** The statement's lines go away. */
		void RemoveStmt(const FDabStmt& Stmt);

		/** The edited text. */
		FString MakeText() const;

	private:
		enum class EEditKind : uint8 { Expr, Stmt, Append, Remove };
		struct FEdit
		{
			EEditKind Kind = EEditKind::Expr;
			FDabSpan Span;
			const FDabNode* Printed = nullptr;
		};

		int32 GetIndentLevel(int32 Offset) const;
		int32 GetLineStart(int32 Offset) const;

		FDabSourceText Source;
		TUniquePtr<FDabFile> File;
		TMultiMap<int32, FDabNode*> ByOffset;
		TMap<const FDabNode*, FDabNode*> Parents;
		TArray<FEdit> Edits;
		/** The block each appended statement went into. */
		TMap<const FDabNode*, const FDabBlockExpr*> AppendBlocks;
	};

	/** One entry of a document section as one side has it: its printed value, and how to make its statement. */
	struct FSectionEntry
	{
		FString Name;
		FString Printed;
		TFunction<FDabStmtPtr()> Make;
	};

	/** Names of the templates the file and its libraries define. */
	TSet<FString> CollectTemplateNames(const FDabCompilationUnit& Unit);
	/** Node calls one of Names somewhere inside. */
	bool CallsAny(const FDabNode& Node, const TSet<FString>& Names);

	/** Everything one write-back compares and edits. */
	struct FWriteBack
	{
		FWriteBack(const FString& InFilePath, FDabWriteBackResult& InResult, const FDabWriteBackOptions& InOptions)
			: FilePath(InFilePath), Result(InResult), Options(InOptions)
		{
		}

		const FString FilePath;
		FDabWriteBackResult& Result;
		const FDabWriteBackOptions& Options;

		/** The model the text checks to, where its parts came from, and the compiled tree they point into. */
		FDabCompileResult Compiled;
		FDabProvenance Provenance;
		/** The model of the edited Blueprint, names made identifiers. */
		FDabModel Edited;
		TUniquePtr<FEditableText> Text;
		FDabLowerOptions LowerOptions;

		/** Edited's nodes by NodeGuid, with the graph each is in. */
		struct FEditedNode
		{
			const FDabModelGraph* Graph = nullptr;
			const FDabModelNode* Node = nullptr;
		};
		TMap<FGuid, FEditedNode> EditedNodes;

		/** Graphs already rewritten whole: nothing more to compare in them. */
		TSet<FString> RewrittenGraphs;
		/** What edits took out of the tree, kept alive: the span index and recorded edits still point at it. */
		TArray<FDabStmtPtr> Graveyard;
		TArray<FDabArgumentPtr> GraveyardArgs;
		TArray<FDabExprPtr> GraveyardExprs;

		/** Arg's value becomes Value, its old text replaced; false when another change already wrote it. */
		bool ReplaceArgumentValue(FDabArgument& Arg, const FDabExpr& Value);

		// -- results ------------------------------------------------------------------------------
		void Change(const FString& Location, const FString& What, bool bWritten, bool bRewroteBlock = false);
		void Refuse(const TCHAR* Code, const FDabSpan& Span, const FString& Message, const FString& Hint = FString());

		// -- graphs (DabWriteBackGraph.cpp) --------------------------------------------------------
		/** Compares and edits one graph of the text against its counterpart in the Blueprint. */
		void CompareGraph(const FString& GraphPath, const FDabModelGraph& TextGraph, const FDabModelGraph* EditedGraph, const FString& Location);
		bool HasStructuralChange(const FString& GraphPath, const FDabModelGraph& TextGraph, const FDabModelGraph& EditedGraph) const;
		void CompareNode(const FString& GraphPath, const FDabModelGraph& TextGraph, const FDabModelNode& TextNode,
			const FDabModelGraph& EditedGraph, const FDabModelNode& EditedNode, const FString& Location);
		/** Rewrites the statements of a graph's block from the Blueprint. */
		void RewriteGraph(const FString& GraphPath, const FDabModelGraph& EditedGraph, const FString& Location);
		/** The block that holds a graph's statements in the text, or null (a layer written `Layer X = Pose;`). */
		FDabBlockExpr* FindGraphBlock(const FString& GraphPath) const;

		// -- state machines (DabWriteBackMachine.cpp) ---------------------------------------------
		void CompareMachines();
		void CompareMachineValues(const FDabModelStateMachine& TextMachine, const FDabModelStateMachine& EditedMachine, FDabNamedBlockStmt& Fresh, const FString& Location);
		void RewriteMachine(const FDabModelStateMachine& TextMachine, const FDabModelStateMachine& EditedMachine, FDabNamedBlockStmt& Fresh, const FString& Location);
		bool StateSettingsDiffer(const FDabModelState& TextState, const FDabModelState& EditedState) const;
		/** A state statement's metadata and OnEnter / OnExit / OnBlended made what the Blueprint has. */
		void WriteStateSettings(FDabNamedBlockStmt& Fresh, const FDabModelState& EditedState);

		// -- document (DabWriteBackDocument.cpp) --------------------------------------------------
		void CompareDocument();
		/** Makes a section's entries After's, keeping the text of entries that did not change; Unmanaged statements stay as they are. */
		void RewriteSection(const FString& Section, const TArray<FSectionEntry>& Before, const TArray<FSectionEntry>& After, const TSet<FString>& Unmanaged);

		// -- helpers ------------------------------------------------------------------------------
		FString Print(const FDabExpr* Expr) const { return Expr ? PrintDabExpr(*Expr) : FString(); }
		FString PrintCall(const TOptional<FDabModelCall>& Call) const;
		FString PrintCall(const FDabModelCall& Call) const;
		FString PrintMeta(const TArray<FDabModelProperty>& Properties) const;
		FGuid TextGuid(const FString& GraphPath, const FString& Id) const { return MakeDabNodeGuid(Compiled.Model.AssetPath, GraphPath, Id); }
		const FDabModelGraph* FindEditedGraph(const FString& GraphPath) const;
	};
}

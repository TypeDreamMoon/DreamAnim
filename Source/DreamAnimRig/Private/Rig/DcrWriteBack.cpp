// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrWriteBack.h"

#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Model/DcrModel.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace
	{
		const FName GeneratedTextKey(TEXT("DreamAnim.DcrGeneratedText"));

		/** A text and the model it builds to. */
		struct FBuiltText
		{
			FDabCompilationUnit Unit;
			FDcrModel Model;
			TArray<FDabDiagnostic> Diagnostics;
			bool bOk = false;
		};

		TUniquePtr<FBuiltText> BuildText(const FString& FilePath, const FString& Text)
		{
			TUniquePtr<FBuiltText> Built = MakeUnique<FBuiltText>();
			FDabDiagnosticSink Sink(FilePath);
			Built->Unit.MainSource = FDabSourceText(FilePath, Text);
			Built->Unit.Main = ParseDab(Built->Unit.MainSource, Sink);
			bool bOk = Built->Unit.Main.IsValid() && Built->Unit.Main->Document.IsValid() && !Sink.HasErrors();
			if (bOk)
			{
				FDabFileSystemIncludeLoader Loader;
				bOk = ResolveDabIncludes(Built->Unit, Loader, Sink) && ExpandDabTemplates(Built->Unit, Sink);
			}
			if (bOk)
			{
				bOk = BuildDcrModel(Built->Unit, Built->Model, Sink) && !Sink.HasErrors();
			}
			Built->bOk = bOk;
			Built->Diagnostics = Sink.GetDiagnostics();
			return Built;
		}

		/** The rig's model, normalized; for comparing with a text's model also in the shape the text holds it. */
		bool ReadRig(const FDcrRig& Rig, FDcrModel& Out, bool bAsText, FString& OutError)
		{
			const FDcrExtractResult Extracted = ExtractDcrModel(Rig, Out);
			if (!Extracted.bSuccess)
			{
				OutError = Extracted.Error;
				return false;
			}
			NormalizeDcrModel(Out);
			if (bAsText)
			{
				MakeDcrModelAsTextHoldsIt(Out);
			}
			return true;
		}

		/** What a model says, for comparing: names, positions and comments aside, and the asset path the text gives. */
		FString DumpForComparing(FDcrModel& Model, const FString& AssetPath)
		{
			const FString Path = Model.AssetPath;
			Model.AssetPath = AssetPath;
			FDcrDumpOptions Options;
			Options.bCanonicalNodeNames = true;
			Options.bFloatAsDouble = true;
			Options.bSkipComments = true;
			FString Dump = DumpDcrModel(Model, Options);
			Model.AssetPath = Path;
			return Dump;
		}

		struct FPatch
		{
			FDabSpan Span;
			FString Text;
			FString Location;
		};

		/** Text with each patch's span replaced by its text; null when two patches overlap. */
		bool ApplyPatches(const FString& Text, TArray<FPatch> Patches, FString& OutText)
		{
			Patches.Sort([](const FPatch& A, const FPatch& B) { return A.Span.Offset > B.Span.Offset; });
			OutText = Text;
			int32 Limit = Text.Len();
			for (const FPatch& Patch : Patches)
			{
				if (Patch.Span.Offset < 0 || Patch.Span.End() > Limit)
				{
					return false;
				}
				OutText = OutText.Left(Patch.Span.Offset) + Patch.Text + OutText.Mid(Patch.Span.End());
				Limit = Patch.Span.Offset;
			}
			return true;
		}

		// ---- The blocks of a document --------------------------------------------------------------------------

		/** A top-level statement's key: `Section Settings`, `Event Forward|"ConstructionGraph"`, `Function Name|`, `Detached |`. */
		FString BlockKey(const FDabStmt& Stmt)
		{
			if (const FDabAssignStmt* Assign = Stmt.As<FDabAssignStmt>())
			{
				return TEXT("Section ") + Assign->Name;
			}
			if (const FDabNamedBlockStmt* Block = Stmt.As<FDabNamedBlockStmt>())
			{
				const FDabMetaEntry* Graph = Block->Meta ? Block->Meta->Find(TEXT("Graph")) : nullptr;
				return Block->Word + TEXT(" ") + Block->Name + TEXT("|") + (Graph && Graph->Value ? PrintDabExpr(*Graph->Value) : FString());
			}
			return FString();
		}

		TArray<TPair<FString, const FDabStmt*>> GetBlocks(const FDabFile& File)
		{
			TArray<TPair<FString, const FDabStmt*>> Blocks;
			if (!File.Document || !File.Document->Body)
			{
				return Blocks;
			}
			for (const FDabStmtPtr& Stmt : File.Document->Body->Statements)
			{
				const FString Key = Stmt ? BlockKey(*Stmt) : FString();
				if (!Key.IsEmpty())
				{
					Blocks.Emplace(Key, Stmt.Get());
				}
			}
			return Blocks;
		}

		const FDabStmt* FindBlock(const TArray<TPair<FString, const FDabStmt*>>& Blocks, const FString& Key)
		{
			const TPair<FString, const FDabStmt*>* Found = Blocks.FindByPredicate([&Key](const TPair<FString, const FDabStmt*>& Block) { return Block.Key == Key; });
			return Found ? Found->Value : nullptr;
		}

		/** What a block does: printed without comments, regions or layout. */
		FString CodeOf(const FDabStmt& Stmt)
		{
			const FString Printed = PrintDabStmt(Stmt, 1);
			TArray<FString> Lines;
			Printed.ParseIntoArrayLines(Lines);
			FString Code;
			for (const FString& Line : Lines)
			{
				const FString Trimmed = Line.TrimStartAndEnd();
				if (Trimmed.StartsWith(TEXT("#Region")) || Trimmed.StartsWith(TEXT("#EndRegion")))
				{
					continue;
				}
				for (const TCHAR Char : Trimmed)
				{
					if (!FChar::IsWhitespace(Char))
					{
						Code.AppendChar(Char);
					}
				}
				Code.AppendChar(TEXT('\n'));
			}
			return Code;
		}

		/** A section's entry statement by name (a variable's declaration, an element's block); null when absent. */
		const FDabStmt* FindEntry(const FDabStmt* Section, const FString& Word, const FString& Name)
		{
			const FDabAssignStmt* Assign = Section ? Section->As<FDabAssignStmt>() : nullptr;
			const FDabBlockExpr* Body = Assign && Assign->Value ? Assign->Value->As<FDabBlockExpr>() : nullptr;
			if (!Body)
			{
				return nullptr;
			}
			for (const FDabStmtPtr& Stmt : Body->Statements)
			{
				if (const FDabDeclStmt* Decl = Stmt ? Stmt->As<FDabDeclStmt>() : nullptr; Decl && Word.IsEmpty() && Decl->Name == Name)
				{
					return Decl;
				}
				if (const FDabNamedBlockStmt* Block = Stmt ? Stmt->As<FDabNamedBlockStmt>() : nullptr; Block && Block->Word == Word && Block->Name == Name)
				{
					return Block;
				}
			}
			return nullptr;
		}

		/** A variable's default as a lowered file writes it; empty when it writes none. */
		FString PrintVariableDefault(const TArray<TPair<FString, const FDabStmt*>>& Blocks, const FString& Name)
		{
			const FDabStmt* Entry = FindEntry(FindBlock(Blocks, TEXT("Section Variables")), FString(), Name);
			const FDabDeclStmt* Decl = Entry ? Entry->As<FDabDeclStmt>() : nullptr;
			return Decl && Decl->Value ? PrintDabExpr(*Decl->Value) : FString();
		}

		/** An element's property as a lowered file writes it; empty when it writes none. */
		FString PrintElementProperty(const TArray<TPair<FString, const FDabStmt*>>& Blocks, const FString& Kind, const FString& Name, const FString& Property)
		{
			const FDabStmt* Entry = FindEntry(FindBlock(Blocks, TEXT("Section Hierarchy")), Kind, Name);
			const FDabNamedBlockStmt* Element = Entry ? Entry->As<FDabNamedBlockStmt>() : nullptr;
			if (!Element || !Element->Body)
			{
				return FString();
			}
			for (const FDabStmtPtr& Stmt : Element->Body->Statements)
			{
				const FDabAssignStmt* Assign = Stmt ? Stmt->As<FDabAssignStmt>() : nullptr;
				if (Assign && Assign->Name == Property && Assign->Value)
				{
					return PrintDabExpr(*Assign->Value);
				}
			}
			return FString();
		}

		// ---- Values in place -----------------------------------------------------------------------------------

		/** The values the rig changed that the text writes, each with the text to put where the text has it. */
		class FInPlace
		{
		public:
			FInPlace(const FDcrModel& InTextModel, const FDcrModel& InRigModel, const FDabFile& InTextFile, const FDabFile& InRigFile)
				: TextModel(InTextModel), RigModel(InRigModel), TextBlocks(GetBlocks(InTextFile)), RigBlocks(GetBlocks(InRigFile))
			{
			}

			TArray<FPatch> Collect()
			{
				for (const FDcrModelGraph& TextGraph : TextModel.Graphs)
				{
					if (const FDcrModelGraph* RigGraph = RigModel.Graphs.FindByPredicate([&TextGraph](const FDcrModelGraph& Graph) { return Graph.Name == TextGraph.Name; }))
					{
						Graph(TextGraph, nullptr, *RigGraph, nullptr, TextGraph.Name);
					}
				}
				for (const FDcrModelFunction& TextFunction : TextModel.Functions)
				{
					if (const FDcrModelFunction* RigFunction = RigModel.Functions.FindByPredicate([&TextFunction](const FDcrModelFunction& Function) { return Function.Name == TextFunction.Name; }))
					{
						Graph(TextFunction.Graph, &TextFunction, RigFunction->Graph, RigFunction, TEXT("Function ") + TextFunction.Name);
					}
				}
				Variables();
				Hierarchy();
				return MoveTemp(Patches);
			}

		private:
			void Graph(const FDcrModelGraph& TextGraph, const FDcrModelFunction* TextEnclosing, const FDcrModelGraph& RigGraph, const FDcrModelFunction* RigEnclosing,
				const FString& Where)
			{
				for (const FDcrModelNode& TextNode : TextGraph.Nodes)
				{
					const FDcrModelNode* RigNode = RigGraph.FindNode(TextNode.Name);
					if (!RigNode || RigNode->Kind != TextNode.Kind || RigNode->StructPath != TextNode.StructPath || RigNode->Notation != TextNode.Notation)
					{
						continue;
					}
					if (TextNode.Contained.IsValid() && RigNode->Contained.IsValid())
					{
						Graph(*TextNode.Contained, TextEnclosing, *RigNode->Contained, RigEnclosing, Where + TEXT("/") + TextNode.Name);
					}
					for (const FDcrValue& TextValue : TextNode.Values)
					{
						if (TextValue.Span.IsEmpty())
						{
							continue;
						}
						// Both printed the way the text writes them: a value spelled otherwise but the same is no change.
						const FString Was = PrintDcrPinValue(TextModel, TextGraph, TextEnclosing, TextNode, TextValue.Name);
						const FString Now = PrintDcrPinValue(RigModel, RigGraph, RigEnclosing, *RigNode, TextValue.Name);
						if (!Now.IsEmpty() && Now != Was)
						{
							Patches.Add({ TextValue.Span, Now, Where + TEXT(": ") + TextNode.Name + TEXT(".") + TextValue.Name });
						}
					}
				}
			}

			void Variables()
			{
				for (const FDcrModelVariable& TextVariable : TextModel.Variables)
				{
					if (TextVariable.DefaultSpan.IsEmpty())
					{
						continue;
					}
					const FString Was = PrintVariableDefault(TextBlocks, TextVariable.Name);
					const FString Now = PrintVariableDefault(RigBlocks, TextVariable.Name);
					if (!Now.IsEmpty() && Now != Was)
					{
						Patches.Add({ TextVariable.DefaultSpan, Now, TEXT("Variables: ") + TextVariable.Name });
					}
				}
			}

			void Hierarchy()
			{
				for (const FDcrModelElement& TextElement : TextModel.Hierarchy)
				{
					const FString Kind = LexToString(TextElement.Kind);
					for (const FDcrValue& Property : TextElement.Properties)
					{
						if (Property.Span.IsEmpty())
						{
							continue;
						}
						const FString Was = PrintElementProperty(TextBlocks, Kind, TextElement.Name, Property.Name);
						const FString Now = PrintElementProperty(RigBlocks, Kind, TextElement.Name, Property.Name);
						if (!Now.IsEmpty() && Now != Was)
						{
							Patches.Add({ Property.Span, Now, FString::Printf(TEXT("Hierarchy: %s %s.%s"), *Kind, *TextElement.Name, *Property.Name) });
						}
					}
				}
			}

			const FDcrModel& TextModel;
			const FDcrModel& RigModel;
			TArray<TPair<FString, const FDabStmt*>> TextBlocks;
			TArray<TPair<FString, const FDabStmt*>> RigBlocks;
			TArray<FPatch> Patches;
		};

		// ---- Whole blocks -----------------------------------------------------------------------------------------

		/** The text with every block whose code differs from the rig's rewritten from the rig, and blocks added or removed. */
		bool RewriteBlocks(const FString& FilePath, const FString& Text, const FDabFile& RigFile, const FDabWriteBackOptions& Options, FString& OutText,
			FDabWriteBackResult& Result)
		{
			FDabDiagnosticSink ParseSink(FilePath);
			const FDabSourceText Source(FilePath, Text);
			const TUniquePtr<FDabFile> UserFile = ParseDab(Source, ParseSink);
			const TUniquePtr<FBuiltText> Built = BuildText(FilePath, Text);
			if (!UserFile || !UserFile->Document || !UserFile->Document->Body || !Built->bOk)
			{
				return false;
			}
			// What the text says, lowered: compared with the rig lowered, block by block, the same way.
			const FDcrLowerResult TextLowered = LowerDcrModel(Built->Model);
			const TArray<TPair<FString, const FDabStmt*>> TextBlocks = GetBlocks(*TextLowered.File);
			const TArray<TPair<FString, const FDabStmt*>> RigBlocks = GetBlocks(RigFile);
			const TArray<TPair<FString, const FDabStmt*>> UserBlocks = GetBlocks(*UserFile);

			TArray<FPatch> Patches;
			bool bRefused = false;
			for (const TPair<FString, const FDabStmt*>& Rig : RigBlocks)
			{
				const FDabStmt* TextBlock = FindBlock(TextBlocks, Rig.Key);
				if (TextBlock && CodeOf(*TextBlock) == CodeOf(*Rig.Value))
				{
					continue;
				}
				FDabWriteBackChange& Change = Result.Changes.AddDefaulted_GetRef();
				Change.Location = Rig.Key.Replace(TEXT("|"), TEXT(" "));
				Change.What = TextBlock ? TEXT("the block's code") : TEXT("a block the text does not have");
				Change.bRewroteBlock = true;
				if (!Options.bStructure)
				{
					bRefused = true;
					continue;
				}
				const FString Printed = PrintDabStmt(*Rig.Value, 1, &RigFile);
				if (const FDabStmt* UserBlock = FindBlock(UserBlocks, Rig.Key))
				{
					Patches.Add({ UserBlock->Span, Printed, Change.Location });
				}
				else
				{
					// A block the text does not have goes at the end of the document.
					const FDabSpan& Body = UserFile->Document->Body->Span;
					FDabSpan Before;
					Before.Offset = Body.End() - 1;
					Patches.Add({ Before, TEXT("\n    ") + Printed + TEXT("\n"), Change.Location });
				}
				Change.bWritten = true;
			}
			for (const TPair<FString, const FDabStmt*>& TextBlock : TextBlocks)
			{
				if (FindBlock(RigBlocks, TextBlock.Key))
				{
					continue;
				}
				FDabWriteBackChange& Change = Result.Changes.AddDefaulted_GetRef();
				Change.Location = TextBlock.Key.Replace(TEXT("|"), TEXT(" "));
				Change.What = TEXT("a block the rig no longer has");
				Change.bRewroteBlock = true;
				if (!Options.bStructure)
				{
					bRefused = true;
					continue;
				}
				if (const FDabStmt* UserBlock = FindBlock(UserBlocks, TextBlock.Key))
				{
					Patches.Add({ UserBlock->Span, FString(), Change.Location });
					Change.bWritten = true;
				}
			}
			if (bRefused)
			{
				FDabDiagnosticSink Sink(FilePath);
				Sink.Error(TEXT("DAB8003"), FDabSpan(), TEXT("The rig's nodes, links or elements changed, and Write Back Structure is off; the text keeps what it said."));
				Result.Diagnostics.Append(Sink.GetDiagnostics());
				Result.bRefused = true;
			}
			return ApplyPatches(Text, MoveTemp(Patches), OutText);
		}
	}

	FString GetDcrGeneratedText(const UObject& Asset)
	{
		const FString* Text = Asset.GetPackage()->GetMetaData().FindValue(&Asset, GeneratedTextKey);
		return Text ? *Text : FString();
	}

	void SetDcrGeneratedText(UObject& Asset, const FString& Text)
	{
		Asset.GetPackage()->GetMetaData().SetValue(&Asset, GeneratedTextKey, *Text);
	}

	FDabWriteBackResult WriteBackDcrRig(const FDcrRig& Rig, const FString& FilePath, const FString& Text, const FDabWriteBackOptions& Options)
	{
		FDabWriteBackResult Result;
		Result.Text = Text;

		const TUniquePtr<FBuiltText> Built = BuildText(FilePath, Text);
		if (!Built->bOk)
		{
			Result.Diagnostics = Built->Diagnostics;
			return Result;
		}
		FDcrModel RigModel;
		FDcrModel RigCompared;
		FString Error;
		if (!ReadRig(Rig, RigModel, false, Error) || !ReadRig(Rig, RigCompared, true, Error))
		{
			FDabDiagnosticSink Sink(FilePath);
			Sink.Error(TEXT("DAB8009"), FDabSpan(), FString::Printf(TEXT("The rig cannot be read: %s"), *Error));
			Result.Diagnostics = Sink.GetDiagnostics();
			return Result;
		}
		Result.bCompared = true;
		const FString AssetPath = Built->Model.AssetPath;
		const FString RigDump = DumpForComparing(RigCompared, AssetPath);
		if (DumpForComparing(Built->Model, AssetPath) == RigDump)
		{
			Result.bFixedPoint = true;
			return Result;
		}

		const FDcrLowerResult RigLowered = LowerDcrModel(RigModel);
		auto Reaches = [&FilePath, &AssetPath, &RigDump](const FString& Candidate)
		{
			TUniquePtr<FBuiltText> Check = BuildText(FilePath, Candidate);
			return Check->bOk && DumpForComparing(Check->Model, AssetPath) == RigDump;
		};

		// Values first, each where the text writes it: nothing else in the text moves.
		{
			const FDcrLowerResult TextLowered = LowerDcrModel(Built->Model);
			FInPlace InPlace(Built->Model, RigModel, *TextLowered.File, *RigLowered.File);
			const TArray<FPatch> Patches = InPlace.Collect();
			FString Patched;
			if (!Patches.IsEmpty() && ApplyPatches(Text, Patches, Patched))
			{
				FString Final = Patched;
				FDabWriteBackResult BlockResult;
				const bool bDone = Reaches(Patched) || (RewriteBlocks(FilePath, Patched, *RigLowered.File, Options, Final, BlockResult) && Reaches(Final));
				if (bDone)
				{
					for (const FPatch& Patch : Patches)
					{
						FDabWriteBackChange& Change = Result.Changes.AddDefaulted_GetRef();
						Change.Location = Patch.Location;
						Change.What = TEXT("a value");
						Change.bWritten = true;
					}
					Result.Changes.Append(BlockResult.Changes);
					Result.Diagnostics.Append(BlockResult.Diagnostics);
					Result.bRefused = BlockResult.bRefused;
					Result.Text = Final;
					Result.bFixedPoint = true;
					return Result;
				}
			}
		}

		// Otherwise the blocks that differ, from the rig.
		FString Rewritten;
		FDabWriteBackResult BlockResult;
		if (RewriteBlocks(FilePath, Text, *RigLowered.File, Options, Rewritten, BlockResult) && !BlockResult.bRefused && Reaches(Rewritten))
		{
			Result.Changes = MoveTemp(BlockResult.Changes);
			Result.Diagnostics.Append(BlockResult.Diagnostics);
			Result.Text = Rewritten;
			Result.bFixedPoint = true;
			return Result;
		}
		Result.Changes = MoveTemp(BlockResult.Changes);
		for (FDabWriteBackChange& Change : Result.Changes)
		{
			Change.bWritten = false;
		}
		Result.Diagnostics.Append(BlockResult.Diagnostics);
		if (!BlockResult.bRefused)
		{
			FDabDiagnosticSink Sink(FilePath);
			Sink.Error(TEXT("DAB8009"), FDabSpan(), TEXT("The rig's changes could not be written so that the text builds to the rig again; the text is left as it was."),
				TEXT("Decompile the rig again to write it out whole."));
			Result.Diagnostics.Append(Sink.GetDiagnostics());
		}
		Result.bRefused = true;
		return Result;
	}
}

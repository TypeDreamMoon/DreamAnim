// Copyright DreamMoon. All Rights Reserved.

#include "WriteBack/DabWriteBack.h"

#include "Animation/AnimBlueprint.h"
#include "Extract/DabExtract.h"
#include "WriteBack/DabWriteBackInternal.h"

namespace UE::DreamAnim::Compiler::WriteBackPrivate
{
	void FWriteBack::Change(const FString& Location, const FString& What, bool bWritten, bool bRewroteBlock)
	{
		FDabWriteBackChange& Entry = Result.Changes.AddDefaulted_GetRef();
		Entry.Location = Location;
		Entry.What = What;
		Entry.bWritten = bWritten;
		Entry.bRewroteBlock = bRewroteBlock;
	}

	void FWriteBack::Refuse(const TCHAR* Code, const FDabSpan& Span, const FString& Message, const FString& Hint)
	{
		// DAB8004 reports a successful template expansion; every other refusal loses information.
		Result.bRefused |= FCString::Strcmp(Code, TEXT("DAB8004")) != 0;
		FDabDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = Code;
		Diagnostic.Severity = EDabSeverity::Warning;
		Diagnostic.Message = Message;
		Diagnostic.Hint = Hint;
		Diagnostic.Span = Span;
		Diagnostic.FilePath = FilePath;
	}

	FString FWriteBack::PrintCall(const TOptional<FDabModelCall>& Call) const
	{
		return Call.IsSet() ? PrintCall(*Call) : FString();
	}

	FString FWriteBack::PrintCall(const FDabModelCall& Call) const
	{
		const FDabExprPtr Lowered = LowerDabCall(Compiled.Model, Call, LowerOptions);
		return Print(Lowered.Get());
	}

	FString FWriteBack::PrintMeta(const TArray<FDabModelProperty>& Properties) const
	{
		// Order-insensitive: the extractor and the text may list properties differently.
		TArray<FString> Lines;
		for (const FDabModelProperty& Property : Properties)
		{
			Lines.Add(Property.Name + TEXT("=") + Print(Property.Value.Get()));
		}
		Lines.Sort();
		return FString::Join(Lines, TEXT(";"));
	}

	const FDabModelGraph* FWriteBack::FindEditedGraph(const FString& GraphPath) const
	{
		if (GraphPath == TEXT("AnimGraph"))
		{
			return &Edited.AnimGraph;
		}
		FString Rest;
		if (GraphPath.Split(TEXT("Layer/"), nullptr, &Rest) && GraphPath.StartsWith(TEXT("Layer/")))
		{
			const FDabModelLayer* Layer = Edited.Layers.FindByPredicate([&Rest](const FDabModelLayer& Candidate) { return Candidate.Name == Rest; });
			return Layer ? &Layer->Graph : nullptr;
		}
		FString Machine;
		FString State;
		if (GraphPath.StartsWith(TEXT("Machine/")) && GraphPath.RightChop(8).Split(TEXT("/State/"), &Machine, &State))
		{
			const FDabModelStateMachine* Found = Edited.FindStateMachine(Machine);
			const FDabModelState* Found2 = Found ? Found->States.FindByPredicate([&State](const FDabModelState& Candidate) { return Candidate.Name == State; }) : nullptr;
			return Found2 ? &Found2->Graph : nullptr;
		}
		return nullptr;
	}

	/** The alias the text uses for `Self.GetParent()`, or empty when it has none. */
	FString FindParentAlias(const FDabProvenance& Provenance)
	{
		const FDabAssignStmt* const* Section = Provenance.Sections.Find(TEXT("Aliases"));
		const FDabBlockExpr* Block = Section && (*Section)->Value ? (*Section)->Value->As<FDabBlockExpr>() : nullptr;
		if (!Block)
		{
			return FString();
		}
		for (const FDabStmtPtr& Statement : Block->Statements)
		{
			const FDabAssignStmt* Alias = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
			if (Alias && Alias->Value && PrintDabExpr(*Alias->Value) == TEXT("Self.GetParent()"))
			{
				return Alias->Name;
			}
		}
		return FString();
	}

	void IndexEditedGraph(FWriteBack& WriteBack, const FDabModelGraph& Graph)
	{
		for (const FDabModelNode& Node : Graph.Nodes)
		{
			if (Node.Guid.IsValid())
			{
				WriteBack.EditedNodes.Add(Node.Guid, { &Graph, &Node });
			}
		}
	}

	/** The first line where two dumps differ, for a message. */
	FString FirstDumpDifference(const FString& Expected, const FString& Actual)
	{
		TArray<FString> E;
		TArray<FString> A;
		Expected.ParseIntoArrayLines(E, false);
		Actual.ParseIntoArrayLines(A, false);
		for (int32 Index = 0; Index < FMath::Max(E.Num(), A.Num()); ++Index)
		{
			const FString Left = E.IsValidIndex(Index) ? E[Index].TrimStartAndEnd() : FString(TEXT("<end>"));
			const FString Right = A.IsValidIndex(Index) ? A[Index].TrimStartAndEnd() : FString(TEXT("<end>"));
			if (!Left.Equals(Right, ESearchCase::CaseSensitive))
			{
				return FString::Printf(TEXT("the text gives `%s` where the Blueprint has `%s`"), *Left, *Right);
			}
		}
		return FString();
	}

	FDabWriteBackResult Run(const UAnimBlueprint& Blueprint, const FString& FilePath, const FString& Text, const FDabWriteBackOptions& Options)
	{
		FDabWriteBackResult Result;
		Result.Text = Text;
		FWriteBack WriteBack(FilePath, Result, Options);

		FDabCompileOptions CompileOptions;
		CompileOptions.bGenerate = false;
		CompileOptions.Provenance = &WriteBack.Provenance;
		WriteBack.Compiled = CompileDabText(FilePath, Text, CompileOptions);
		if (!WriteBack.Compiled.Succeeded() || !WriteBack.Compiled.Unit)
		{
			Result.Diagnostics.Append(WriteBack.Compiled.Diagnostics);
			WriteBack.Refuse(TEXT("DAB8002"), FDabSpan(), TEXT("The text has errors, so the editor's changes cannot be compared with it."),
				TEXT("Fix the errors in the text; the Blueprint keeps the changes until then."));
			return Result;
		}

		FDabDiagnosticSink ExtractSink(FilePath);
		if (!ExtractDabModel(Blueprint, WriteBack.Edited, ExtractSink))
		{
			Result.Diagnostics.Append(ExtractSink.GetDiagnostics());
			return Result;
		}
		NormalizeDabModelNames(WriteBack.Edited);
		// The Blueprint may live elsewhere (a test's copy); it was made from this text all the same.
		WriteBack.Edited.AssetPath = WriteBack.Compiled.Model.AssetPath;
		for (const FDabModelUnsupported& Unsupported : WriteBack.Edited.Unsupported)
		{
			WriteBack.Refuse(TEXT("DAB8006"), FDabSpan(), FString::Printf(TEXT("%s: %s; the text cannot hold it."), *Unsupported.Location, *Unsupported.Description),
				TEXT("Move that logic into the C++ anim instance."));
		}
		WriteBack.Edited.Unsupported.Reset();

		WriteBack.Text = MakeUnique<FEditableText>(FilePath, Text);
		if (!WriteBack.Text->IsValid())
		{
			return Result;
		}
		WriteBack.LowerOptions.ParentAlias = FindParentAlias(WriteBack.Provenance);
		WriteBack.LowerOptions.bUseAliases = !WriteBack.LowerOptions.ParentAlias.IsEmpty();

		IndexEditedGraph(WriteBack, WriteBack.Edited.AnimGraph);
		for (const FDabModelLayer& Layer : WriteBack.Edited.Layers)
		{
			IndexEditedGraph(WriteBack, Layer.Graph);
		}
		for (const FDabModelStateMachine& Machine : WriteBack.Edited.StateMachines)
		{
			for (const FDabModelState& State : Machine.States)
			{
				IndexEditedGraph(WriteBack, State.Graph);
			}
		}

		const FDabModel& Model = WriteBack.Compiled.Model;
		Result.bCompared = true;
		const FString Before = DumpDabModel(Model);
		const FString Wanted = DumpDabModel(WriteBack.Edited);
		if (Before.Equals(Wanted, ESearchCase::CaseSensitive))
		{
			Result.bFixedPoint = true;
			return Result;
		}

		WriteBack.CompareDocument();
		if (Model.Kind == EDabDocumentKind::AnimBlueprint)
		{
			WriteBack.CompareGraph(TEXT("AnimGraph"), Model.AnimGraph, &WriteBack.Edited.AnimGraph, TEXT("Graph"));
		}
		for (const FDabModelLayer& Layer : Model.Layers)
		{
			if (!Layer.bDeclarationOnly)
			{
				WriteBack.CompareGraph(TEXT("Layer/") + Layer.Name, Layer.Graph, WriteBack.FindEditedGraph(TEXT("Layer/") + Layer.Name),
					TEXT("Layer ") + Layer.Name);
			}
		}
		WriteBack.CompareMachines();
		Result.Text = WriteBack.Text->MakeText();

		// The new text must check to what the Blueprint holds; anything left over was not written.
		FDabCompileOptions CheckOptions;
		CheckOptions.bGenerate = false;
		const FDabCompileResult Checked = CompileDabText(FilePath, Result.Text, CheckOptions);
		if (!Checked.Succeeded())
		{
			for (const FDabDiagnostic& Diagnostic : Checked.Diagnostics)
			{
				if (Diagnostic.Severity == EDabSeverity::Error)
				{
					WriteBack.Refuse(TEXT("DAB8009"), Diagnostic.Span, FString::Printf(TEXT("Writing the changes broke the text (%s); nothing was written."),
						*FDabDiagnosticSink::ToWireString(Diagnostic)));
					break;
				}
			}
			Result.Text = Text;
			for (FDabWriteBackChange& Change : Result.Changes)
			{
				Change.bWritten = false;
			}
			return Result;
		}
		const FString After = DumpDabModel(Checked.Model);
		Result.bFixedPoint = After.Equals(Wanted, ESearchCase::CaseSensitive);
		if (!Result.bFixedPoint)
		{
			WriteBack.Refuse(TEXT("DAB8005"), FDabSpan(), FString::Printf(TEXT("The text still differs from the Blueprint: %s."), *FirstDumpDifference(After, Wanted)),
				TEXT("Rebuild the Blueprint from the text to drop the rest, or make the change in the text."));
		}
		return Result;
	}
}

namespace UE::DreamAnim::Compiler
{
	FDabWriteBackResult WriteBackDabBlueprint(const UAnimBlueprint& Blueprint, const FString& FilePath, const FString& Text, const FDabWriteBackOptions& Options)
	{
		return WriteBackPrivate::Run(Blueprint, FilePath, Text, Options);
	}
}

// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrDocumentHandler.h"

#include "ControlRigBlueprintLegacy.h"
#include "ControlRigRuntimeAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Lang/DabDiagnostic.h"
#include "Lang/DabPrinter.h"
#include "Misc/Paths.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExecPins.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrGenerate.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNames.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"
#include "Rig/DcrSemanticInternal.h"
#include "Rig/DcrWriteBack.h"
#include "RigVMCore/RigVMRegistry.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Lang;

	namespace
	{
		/**
		 * RigVM registers its dispatches (If, the array nodes...) at the end of engine init, which a commandlet runs
		 * before: until then the text cannot name them.
		 */
		void ReadyRigVM()
		{
			FRigVMRegistry::Get().RefreshEngineTypesIfRequired();
		}
	}

	void FDcrDocumentHandler::Compile(FDabCompileResult& Result, const FString& Text, const FDabCompileOptions& Options)
	{
		if (!Result.Unit || !Result.Unit->Main || !Result.Unit->Main->Document)
		{
			return;
		}
		const FDabSpan DocumentSpan = Result.Unit->Main->Document->KindSpan;
		ReadyRigVM();

		FDcrSemanticOptions SemanticOptions;
		if (!Options.OverridePackageName.IsEmpty())
		{
			SemanticOptions.AssetPath = Options.OverridePackageName + TEXT(".") + FPackageName::GetShortName(Options.OverridePackageName);
		}
		Model::FDcrModel Model;
		{
			FDabDiagnosticSink Sink(Result.FilePath);
			BuildDcrModel(*Result.Unit, Model, Sink, SemanticOptions);
			AddDabCompileDiagnostics(Result, Sink.GetDiagnostics());
		}
		if (Result.NumErrors > 0 || !Options.bGenerate)
		{
			return;
		}

		FDcrGenerateOptions GenerateOptions;
		GenerateOptions.bSave = Options.bSave;
		// What the rig is built from, kept with it: write-back compares the file with it.
		GenerateOptions.SourceText = Text;
		const FDcrGenerateResult Generated = GenerateDcrRig(Model, GenerateOptions);
		FDabDiagnosticSink Sink(Result.FilePath);
		for (const FString& Error : Generated.Errors)
		{
			Sink.Error(TEXT("DAB9101"), DocumentSpan, Error);
		}
		for (const FString& Warning : Generated.Warnings)
		{
			Sink.Warning(TEXT("DAB9102"), DocumentSpan, Warning);
		}
		for (const FString& Caller : Generated.BrokenCallers)
		{
			Sink.Warning(TEXT("DAB9103"), DocumentSpan,
				FString::Printf(TEXT("%s called a function this rig no longer has; the engine removed those calls."), *Caller),
				TEXT("Open that rig and replace the calls, or put the function back."));
		}
		AddDabCompileDiagnostics(Result, Sink.GetDiagnostics());
		Result.Asset = Generated.Rig.Asset;
	}

	TArray<FTopLevelAssetPath> FDcrDocumentHandler::GetDecompileClasses() const
	{
		return {
			UControlRigBlueprint::StaticClass()->GetClassPathName(),
			UControlRigRuntimeAsset::StaticClass()->GetClassPathName(),
		};
	}

	bool FDcrDocumentHandler::CanDecompile(const UObject& Asset) const
	{
		return WrapDcrRig(const_cast<UObject*>(&Asset)).IsValid();
	}

	FDabDecompileResult FDcrDocumentHandler::Decompile(UObject& Asset)
	{
		FDabDecompileResult Result;
		ReadyRigVM();
		const FDcrRig Rig = WrapDcrRig(&Asset);
		FDabDiagnosticSink Sink(Asset.GetPathName());
		Model::FDcrModel Model;
		const FDcrExtractResult Extracted = ExtractDcrModel(Rig, Model);
		if (!Extracted.bSuccess)
		{
			Sink.Error(TEXT("DAB9104"), FDabSpan(), FString::Printf(TEXT("Cannot read %s: %s"), *Asset.GetPathName(), *Extracted.Error));
			Result.Diagnostics = Sink.GetDiagnostics();
			return Result;
		}
		NormalizeDcrModel(Model);
		const FDcrLowerResult Lowered = LowerDcrModel(Model);
		Result.Text = PrintDab(*Lowered.File);
		Result.NumUnsupported = Lowered.Unsupported.Num();
		Result.Diagnostics = Sink.GetDiagnostics();
		Result.bSucceeded = true;
		return Result;
	}

	FString FDcrDocumentHandler::GetDefaultDecompilePath(const UObject& Asset) const
	{
		// After `/Game/`, the path; under any other mount, the mount too (as Animation Blueprints go).
		FString Relative = Asset.GetPackage()->GetName();
		if (!Relative.RemoveFromStart(TEXT("/Game/")))
		{
			Relative.RemoveFromStart(TEXT("/"));
		}
		FString Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT("DAnim"), Relative + GetSourceExtension());
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	FString FDcrDocumentHandler::GetSourceExtension() const
	{
		return TEXT(".dcr");
	}

	FDabWriteBackResult FDcrDocumentHandler::WriteBack(UObject& Asset, const FString& FilePath, const FString& Text, const FDabWriteBackOptions& Options)
	{
		ReadyRigVM();
		const FDcrRig Rig = WrapDcrRig(&Asset);
		if (!Rig.IsValid())
		{
			FDabWriteBackResult Result;
			Result.Text = Text;
			return Result;
		}
		return WriteBackDcrRig(Rig, FilePath, Text, Options);
	}

	FString FDcrDocumentHandler::GetGeneratedText(const UObject& Asset) const
	{
		return GetDcrGeneratedText(Asset);
	}

	void FDcrDocumentHandler::SetGeneratedText(UObject& Asset, const FString& Text) const
	{
		SetDcrGeneratedText(Asset, Text);
	}

	void FDcrDocumentHandler::AddVocabulary(FJsonObject& Out, const Compiler::FDabVocabularyOptions& Options) const
	{
		if (!Options.bRigUnits)
		{
			return;
		}
		ReadyRigVM();
		const auto Strings = [](TConstArrayView<FString> Items)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Item : Items)
			{
				Values.Add(MakeShared<FJsonValueString>(Item));
			}
			return Values;
		};

		TArray<TSharedPtr<FJsonValue>> Units;
		for (const FDcrNodeType& Type : GetDcrNodeTypes())
		{
			const UScriptStruct& Struct = *Type.Struct;
			const Sem::FSignature Signature = Type.bDispatch ? Sem::MakeDispatchSignature(Struct) : Sem::MakeUnitSignature(Struct);
			TSharedRef<FJsonObject> Unit = MakeShared<FJsonObject>();
			Unit->SetStringField(TEXT("name"), GetDcrNodeTypeName(Struct));
			Unit->SetStringField(TEXT("struct"), Struct.GetPathName());
			Unit->SetStringField(TEXT("kind"), Type.bDispatch ? TEXT("dispatch") : TEXT("unit"));
			Unit->SetBoolField(TEXT("deprecated"), Type.bDeprecated);
			for (const TCHAR* Key : { TEXT("DisplayName"), TEXT("Category"), TEXT("Keywords") })
			{
				const FString Value = Struct.GetMetaData(Key);
				if (!Value.IsEmpty())
				{
					Unit->SetStringField(FString(Key).Left(1).ToLower() + FString(Key).Mid(1), Value);
				}
			}

			// Execute pins: what runs it, what it runs, and the outputs a block after its call can hang off.
			Unit->SetBoolField(TEXT("mutable"), Signature.IsMutable());
			if (Signature.IsMutable())
			{
				Model::FDcrModelNode Probe;
				Probe.Kind = Type.bDispatch ? Model::EDcrNodeKind::Dispatch : Model::EDcrNodeKind::Unit;
				Probe.StructPath = Struct.GetPathName();
				Probe.ExecuteIn = Signature.ExecIn;
				Probe.ExecuteOut = Signature.ExecOut;
				const FString Continue = GetDcrContinuation(Probe);
				TArray<FString> Blocks = Signature.ExecOut.FilterByPredicate([&Continue](const FString& Pin) { return Pin != Continue; });
				Unit->SetArrayField(TEXT("executeIn"), Strings(Signature.ExecIn));
				Unit->SetArrayField(TEXT("executeOut"), Strings(Signature.ExecOut));
				Unit->SetStringField(TEXT("continuesBy"), Continue);
				Unit->SetArrayField(TEXT("blocks"), Strings(Blocks));
			}

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const Sem::FPin& Pin : Signature.Pins)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Pin.Name);
				Entry->SetStringField(TEXT("direction"), Pin.bInput && Pin.bOutput ? TEXT("io") : Pin.bOutput ? TEXT("output") : TEXT("input"));
				Entry->SetStringField(TEXT("type"), Pin.Type.IsSet() ? GetDcrTypeName(Pin.Type) : FString(TEXT("wildcard")));
				if (Pin.Type.IsSet())
				{
					Entry->SetStringField(TEXT("cppType"), Pin.Type.CPPType);
				}
				if (Pin.bWildcard)
				{
					// A dispatch argument that takes several types: the call picks one.
					Entry->SetBoolField(TEXT("wildcard"), true);
				}
				if (Pin.bAggregate)
				{
					Entry->SetBoolField(TEXT("aggregate"), true);
				}
				if (!Pin.Default.IsEmpty())
				{
					// The engine's export text, as a fresh node holds it.
					Entry->SetStringField(TEXT("default"), Pin.Default);
				}
				Pins.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Unit->SetArrayField(TEXT("pins"), Pins);
			Units.Add(MakeShared<FJsonValueObject>(Unit));
		}
		Out.SetArrayField(TEXT("rigUnits"), Units);
	}
}

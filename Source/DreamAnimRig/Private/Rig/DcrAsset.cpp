// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrAsset.h"

#include "AssetToolsModule.h"
#include "ControlRigAssetFactory.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigRuntimeAsset.h"
#include "Engine/Blueprint.h"
#include "IAssetTools.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/CoreMisc.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RigVMBlueprintLegacy.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/Script.h"

namespace UE::DreamAnim::Rig
{
	FDcrQuietCompileScope::FDcrQuietCompileScope()
		: Script(LogScript.GetVerbosity())
		, Blueprint(LogBlueprint.GetVerbosity())
		, Handler(MakeUnique<FScopedScriptExceptionHandler>([](ELogVerbosity::Type, const TCHAR*, const TCHAR*) {}))
	{
		LogScript.SetVerbosity(ELogVerbosity::Fatal);
		LogBlueprint.SetVerbosity(ELogVerbosity::Fatal);
	}

	FDcrQuietCompileScope::~FDcrQuietCompileScope()
	{
		Handler.Reset();
		LogScript.SetVerbosity(Script);
		LogBlueprint.SetVerbosity(Blueprint);
	}

	const TCHAR* LexToString(EDcrAssetKind Kind)
	{
		return Kind == EDcrAssetKind::Legacy ? TEXT("Legacy") : TEXT("Independent");
	}

	EDcrAssetKind FDcrRig::GetKind() const
	{
		return Cast<UControlRigBlueprint>(Asset) ? EDcrAssetKind::Legacy : EDcrAssetKind::Independent;
	}

	FDcrRig WrapDcrRig(UObject* Asset)
	{
		FDcrRig Rig;
		Rig.Asset = Asset;
		Rig.Editor = Asset;
		if (UControlRigRuntimeAsset* Runtime = Cast<UControlRigRuntimeAsset>(Asset))
		{
			Rig.Editor = Runtime->GetEditorOnlyData();
		}
		if (Rig.Editor)
		{
			Rig.ControlRig = FControlRigAssetInterfacePtr(Rig.Editor);
			Rig.RigVM = FRigVMEditorAssetInterfacePtr(Rig.Editor);
		}
		return Rig;
	}

	FDcrRig CreateDcrRig(const FString& PackagePath, EDcrAssetKind Kind)
	{
		UObject* Asset = nullptr;
		if (Kind == EDcrAssetKind::Legacy)
		{
			Asset = UControlRigBlueprintFactory::CreateNewControlRigAsset(PackagePath, false);
		}
		else
		{
			Asset = UControlRigAssetFactory::CreateNewControlRigAsset(PackagePath, false);
		}
		return WrapDcrRig(Asset);
	}

	FDcrRig LoadDcrRig(const FString& ObjectPath)
	{
		// A Blueprint compiles as it loads.
		FDcrQuietCompileScope Quiet;
		return WrapDcrRig(LoadObject<UObject>(nullptr, *ObjectPath));
	}

	FDcrRig DuplicateDcrRig(const FDcrRig& Source, const FString& Folder, const FString& Name)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		// A Blueprint's copy compiles as it is made.
		FDcrQuietCompileScope Quiet;
		return WrapDcrRig(Tools.DuplicateAsset(Name, Folder, Source.Asset));
	}

	bool SaveDcrRig(const FDcrRig& Rig)
	{
		UPackage* Package = Rig.Asset ? Rig.Asset->GetPackage() : nullptr;
		if (!Package)
		{
			return false;
		}
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Rig.Asset, *File, Args);
	}

	URigVMController* GetDcrController(const FDcrRig& Rig, URigVMGraph* Graph)
	{
		return Rig.RigVM->GetOrCreateController(Graph);
	}

	URigVMGraph* AddDcrModelGraph(const FDcrRig& Rig, const FString& Name)
	{
		// The client's FName overload takes the name as it is: the assets' AddModel(FString) puts
		// "RigVMModel " in front of it, and a rebuilt rig would not have the graph names it had.
		FRigVMClient* Client = Rig.RigVM ? Rig.RigVM->GetRigVMClient() : nullptr;
		return Client ? Client->AddModel(FName(*Name), false) : nullptr;
	}

	URigVMGraph* FindDcrModelGraph(const FDcrRig& Rig, const FString& Name)
	{
		for (URigVMGraph* Graph : Rig.RigVM->GetAllModels())
		{
			if (Graph && Graph->IsTopLevelGraph() && (Graph->GetGraphName() == Name || Graph->GetName() == Name))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	TArray<URigVMGraph*> GetAllDcrGraphs(const FDcrRig& Rig)
	{
		TArray<URigVMGraph*> Graphs = Rig.RigVM->GetAllModels();
		Graphs.RemoveAll([](const URigVMGraph* Graph) { return Graph == nullptr; });
		Graphs.Sort([](const URigVMGraph& A, const URigVMGraph& B) { return A.GetNodePath() < B.GetNodePath(); });
		return Graphs;
	}

	URigVMNode* FindDcrEventNode(URigVMGraph* Graph, const FName& EventName)
	{
		if (!Graph)
		{
			return nullptr;
		}
		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (Node && Node->IsEvent() && Node->GetEventName() == EventName)
			{
				return Node;
			}
		}
		return nullptr;
	}

	TArray<FString> CompileDcrRig(const FDcrRig& Rig, int32* OutWarnings)
	{
		// The compiler's messages are read from its own log below.
		FDcrLogCapture Capture;
		{
			FDcrQuietCompileScope Quiet;
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Rig.Asset))
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			}
			else
			{
				Rig.RigVM->RecompileVM();
			}
		}

		const FCompilerResultsLog& Log = Rig.RigVM->GetCompileLog();
		TArray<FString> Errors = Capture.GetErrors();
		int32 Warnings = Capture.GetWarnings().Num() + Log.NumWarnings;
		for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
		{
			if (Message->GetSeverity() != EMessageSeverity::Error)
			{
				continue;
			}
			// Code nothing runs into: the compiler leaves it out, which is what a rig with detached code means.
			const FString Text = Message->ToText().ToString();
			if (Text.Contains(TEXT("is not linked to execution")))
			{
				++Warnings;
				continue;
			}
			Errors.Add(Text);
		}
		if (OutWarnings)
		{
			*OutWarnings = Warnings;
		}
		return Errors;
	}

	FDcrLogCapture::FDcrLogCapture()
	{
		GLog->AddOutputDevice(this);
	}

	FDcrLogCapture::~FDcrLogCapture()
	{
		GLog->RemoveOutputDevice(this);
	}

	void FDcrLogCapture::Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category)
	{
		const ELogVerbosity::Type Level = ELogVerbosity::Type(Verbosity & ELogVerbosity::VerbosityMask);
		if (Level <= ELogVerbosity::Warning && Level != ELogVerbosity::NoLogging)
		{
			FScopeLock Scope(&Lock);
			Lines.Emplace(Level, FString::Printf(TEXT("%s: %s"), *Category.ToString(), Text));
		}
	}

	TArray<FString> FDcrLogCapture::GetErrors() const
	{
		FScopeLock Scope(&Lock);
		TArray<FString> Out;
		for (const TPair<ELogVerbosity::Type, FString>& Line : Lines)
		{
			if (Line.Key <= ELogVerbosity::Error)
			{
				Out.Add(Line.Value);
			}
		}
		return Out;
	}

	TArray<FString> FDcrLogCapture::GetWarnings() const
	{
		FScopeLock Scope(&Lock);
		TArray<FString> Out;
		for (const TPair<ELogVerbosity::Type, FString>& Line : Lines)
		{
			if (Line.Key == ELogVerbosity::Warning)
			{
				Out.Add(Line.Value);
			}
		}
		return Out;
	}
}

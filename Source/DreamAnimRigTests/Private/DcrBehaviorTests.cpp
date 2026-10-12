// Copyright DreamMoon. All Rights Reserved.
//
// A modular rig behaves the same whether its modules are the engine's or the ones DreamAnim rebuilt from text.
//
// A biped is put together twice on one mesh from the same list of modules: the root, the spine, the neck, an arm
// and a leg on each side and a foot on each leg. The first rig takes the modules as the engine ships them
// (ControlRigModules' Modules58 and the root module). The second takes the same modules after a trip through the
// text: each one decompiled to .dcr, read back and generated into /Game/DreamAnimRigTests/Behavior/Modules.
//
// Both rigs are built the way the editor builds one. The root connects to the root bone; every limb module then
// goes onto a socket that an earlier module made (the sockets are found by their description, name and side, as
// the modules' own text says which module goes where), and the modules' connector events resolve the rest. Both
// rigs must come out with the same module tree and the same connections; then both run 60 frames on the same
// input and must match bit for bit.

#include "DcrCorpus.h"
#include "DcrRunHarness.h"

#include "ControlRig.h"
#include "ControlRigAssetFactory.h"
#include "ControlRigBlueprintFactory.h"
#include "Engine/SkeletalMesh.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Model/DcrModel.h"
#include "ModularRig.h"
#include "ModularRigController.h"
#include "ModularRigModel.h"
#include "ModularRigRuleManager.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrGenerate.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigModuleDefines.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests::BehaviorPrivate
{
	/**
	 * The asset kind of the two modular rigs: what 5.8 creates by default. Both kinds hold modules; the independent
	 * one has no class default object to answer the editor's questions, so the running instance made by
	 * RecompileModularRig stands in for it (FindDebuggedRig below).
	 * The controller's checks and the auto resolve find that instance for the independent kind too
	 * (UModularRigController::GetDebuggedModularRig).
	 */
	constexpr EDcrAssetKind ModularRigKind = EDcrAssetKind::Independent;
	constexpr int32 BehaviorFrameCount = 60;
	constexpr int32 MaxReported = 20;
	/** How often the secondary connectors of a module are resolved again when some are still open. */
	constexpr int32 MaxResolveRounds = 3;

	enum class EBipedSide : uint8 { None, Left, Right };

	/** One module of the biped: where it comes from, what it is called in the modular rig, and how its socket is found. */
	struct FBipedModule
	{
		/** The module's name in the corpus (Root, Spine, ...). */
		const TCHAR* Source;
		/** Its name in the modular rig. */
		const TCHAR* Instance;
		EBipedSide Side;
		/**
		 * Words that mark the socket this module goes onto: looked for in the socket's description, name and parent bone.
		 * When no socket fits, the failure lists what the connector can go to.
		 */
		const TCHAR* Words[3];
	};

	/** In the order they are added: a module goes onto a socket an earlier one made. */
	TConstArrayView<FBipedModule> GetBipedModules()
	{
		static const FBipedModule Modules[] = {
			{ TEXT("Root"), TEXT("Root"), EBipedSide::None, { nullptr } },
			{ TEXT("Spine"), TEXT("Spine"), EBipedSide::None, { TEXT("spine"), nullptr } },
			{ TEXT("Neck"), TEXT("Neck"), EBipedSide::None, { TEXT("neck"), nullptr } },
			{ TEXT("Arm"), TEXT("ArmL"), EBipedSide::Left, { TEXT("arm"), TEXT("shoulder"), TEXT("clavicle") } },
			{ TEXT("Arm"), TEXT("ArmR"), EBipedSide::Right, { TEXT("arm"), TEXT("shoulder"), TEXT("clavicle") } },
			{ TEXT("Leg"), TEXT("LegL"), EBipedSide::Left, { TEXT("leg"), TEXT("thigh"), nullptr } },
			{ TEXT("Leg"), TEXT("LegR"), EBipedSide::Right, { TEXT("leg"), TEXT("thigh"), nullptr } },
			{ TEXT("Foot"), TEXT("FootL"), EBipedSide::Left, { TEXT("foot"), TEXT("ankle"), nullptr } },
			{ TEXT("Foot"), TEXT("FootR"), EBipedSide::Right, { TEXT("foot"), TEXT("ankle"), nullptr } },
		};
		return Modules;
	}

	struct FBipedMesh
	{
		const TCHAR* Path;
		const TCHAR* Label;
	};

	/**
	 * The meshes the biped is tried on, in order: the first on which the engine's own modules connect fully is used.
	 * The mannequin of the Mover examples is a UE5 mannequin skeleton, which the root module's skeleton detection
	 * knows; the biped template is what the modules were made on.
	 */
	/** Float math the text builds as its double twin, anywhere in the model. */
	bool HasFloatMath(const Model::FDcrModelGraph& Graph)
	{
		for (const Model::FDcrModelNode& Node : Graph.Nodes)
		{
			if (Node.StructPath.Contains(TEXT("RigVMFunction_MathFloat")) || (Node.Contained && HasFloatMath(*Node.Contained)))
			{
				return true;
			}
		}
		return false;
	}

	bool HasFloatMath(const Model::FDcrModel& InModel)
	{
		return InModel.Graphs.ContainsByPredicate([](const Model::FDcrModelGraph& Graph) { return HasFloatMath(Graph); })
			|| InModel.Functions.ContainsByPredicate([](const Model::FDcrModelFunction& Function) { return HasFloatMath(Function.Graph); });
	}

	TConstArrayView<FBipedMesh> GetBipedMeshes()
	{
		static const FBipedMesh Meshes[] = {
			{ TEXT("/MoverExamples/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"), TEXT("Manny") },
			{ TEXT("/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template"), TEXT("BipedTemplate") },
		};
		return Meshes;
	}

	/** What a piece of work found: errors fail the test, warnings and notes only tell. */
	struct FBehaviorReport
	{
		TArray<FString> Problems;
		TArray<FString> Warnings;
		TArray<FString> Notes;
	};

	/** A modular rig put together from a set of module assets. */
	struct FBehaviorAssembly
	{
		FDcrRig Rig;
		FBehaviorReport Report;
		/** The module tree and every connection, sorted: two assemblies of the same biped list alike. */
		TArray<FString> Layout;
	};

	void FlushReport(FAutomationTestBase& Test, const FString& Label, const FBehaviorReport& Report)
	{
		for (const FString& Note : Report.Notes)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Label, *Note));
		}
		for (const FString& Warning : Report.Warnings)
		{
			Test.AddWarning(FString::Printf(TEXT("%s: %s"), *Label, *Warning));
		}
		for (int32 Index = 0; Index < Report.Problems.Num() && Index < MaxReported; ++Index)
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *Label, *Report.Problems[Index]));
		}
		if (Report.Problems.Num() > MaxReported)
		{
			Test.AddError(FString::Printf(TEXT("%s: %d more problems"), *Label, Report.Problems.Num() - MaxReported));
		}
	}

	const FDcrCorpusEntry* FindModuleEntry(const FString& Name)
	{
		for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
		{
			if (Name == Entry.Name && (Entry.Group == EDcrCorpusGroup::Module || Entry.Group == EDcrCorpusGroup::Library))
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	TArray<FString> LinesOnlyIn(const TArray<FString>& A, const TArray<FString>& B)
	{
		TArray<FString> Out;
		for (const FString& Line : A)
		{
			if (!B.Contains(Line))
			{
				Out.Add(Line);
			}
		}
		return Out;
	}

	FString DescribeConnectors(const FRigModuleSettings& Settings)
	{
		TArray<FString> Parts;
		for (const FRigModuleConnector& Connector : Settings.ExposedConnectors)
		{
			Parts.Add(FString::Printf(TEXT("%s%s"), *Connector.Name, Connector.IsPrimary() ? TEXT(" (primary)") : (Connector.IsOptional() ? TEXT(" (optional)") : TEXT(""))));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	// ---------------------------------------------------------------- Modules through the text

	/**
	 * One module of the engine through the text and back, the way the round trip test does it: decompiled, printed,
	 * parsed, checked and generated at /Game/DreamAnimRigTests/Behavior/Modules/<Name>. Returns the rebuilt asset
	 * (invalid when it could not be made). What a rebuilt module lacks of the original is told in the report.
	 */
	FDcrRig RebuildThroughText(const FDcrCorpusEntry& Entry, const FDcrRig& Original, FBehaviorReport& Report)
	{
		Model::FDcrModel OriginalModel;
		const FDcrExtractResult Extracted = ExtractDcrModel(Original, OriginalModel);
		if (!Extracted.bSuccess)
		{
			Report.Problems.Add(FString::Printf(TEXT("extract failed: %s"), *Extracted.Error));
			return FDcrRig();
		}
		NormalizeDcrModel(OriginalModel);
		// The header names the original asset, as it does when the round trip writes it.
		OriginalModel.AssetPath = Entry.ObjectPath;
		const FDcrLowerResult Lowered = LowerDcrModel(OriginalModel);
		const FString Text = Lang::PrintDab(*Lowered.File);
		FFileHelper::SaveStringToFile(Text, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests"), TEXT("Behavior"), FString(Entry.Name) + TEXT(".dcr")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!Lowered.Unsupported.IsEmpty())
		{
			Report.Warnings.Add(FString::Printf(TEXT("the module holds %d things the text cannot say, so the rebuilt module lacks them (first: %s: %s)"),
				Lowered.Unsupported.Num(), *Lowered.Unsupported[0].Location, *Lowered.Unsupported[0].Description));
		}

		// Back to a model, into an asset of the same kind under the test folder.
		const FString RebuiltPath = FString::Printf(TEXT("/Game/DreamAnimRigTests/Behavior/Modules/%s.%s"), Entry.Name, Entry.Name);
		Lang::FDabCompilationUnit Unit;
		Unit.MainSource = Lang::FDabSourceText(FString(Entry.Name) + TEXT(".dcr"), Text);
		Lang::FDabDiagnosticSink Sink(Unit.MainSource.GetPath());
		Unit.Main = Lang::ParseDab(Unit.MainSource, Sink);
		Lang::ExpandDabTemplates(Unit, Sink);
		Model::FDcrModel RebuiltModel;
		FDcrSemanticOptions SemanticOptions;
		SemanticOptions.AssetPath = RebuiltPath;
		BuildDcrModel(Unit, RebuiltModel, Sink, SemanticOptions);
		int32 Errors = 0;
		for (const Lang::FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error && Errors++ < MaxReported)
			{
				Report.Problems.Add(FString::Printf(TEXT("(%d,%d) %s: %s"), Diagnostic.Span.Line, Diagnostic.Span.Column, *Diagnostic.Code, *Diagnostic.Message));
			}
		}
		if (Errors > 0)
		{
			Report.Problems.Add(FString::Printf(TEXT("%d error(s) reading the text back"), Errors));
			return FDcrRig();
		}
		RebuiltModel.AssetKind = OriginalModel.AssetKind;

		const FDcrGenerateResult Generated = GenerateDcrRig(RebuiltModel);
		for (const FString& Error : Generated.Errors)
		{
			Report.Problems.Add(TEXT("generate: ") + Error);
		}
		for (int32 Index = 0; Index < Generated.Warnings.Num() && Index < MaxReported; ++Index)
		{
			Report.Warnings.Add(TEXT("generate: ") + Generated.Warnings[Index]);
		}
		if (!Generated.Rig.IsValid())
		{
			return FDcrRig();
		}

		// A module the modular rig can use says so by its settings: same identifier, same connectors.
		const FRigModuleSettings& WantedSettings = Original.ControlRig->GetRigModuleSettings();
		const FRigModuleSettings& GotSettings = Generated.Rig.ControlRig->GetRigModuleSettings();
		if (!Generated.Rig.ControlRig->IsControlRigModule())
		{
			Report.Problems.Add(TEXT("the rebuilt asset is not a rig module"));
		}
		else if (!(WantedSettings == GotSettings))
		{
			Report.Warnings.Add(FString::Printf(TEXT("the module settings differ; connectors were [%s] and are [%s]"),
				*DescribeConnectors(WantedSettings), *DescribeConnectors(GotSettings)));
		}
		return Generated.Rig;
	}

	// ---------------------------------------------------------------- Putting a biped together

	FRigElementKey MakeConnectorKey(const FName& ModuleName, const FString& ConnectorName)
	{
		return FRigElementKey(FRigHierarchyModulePath(ModuleName.ToString(), ConnectorName).GetPathFName(), ERigElementType::Connector);
	}

	/** A modular rig on the mesh: the skeleton's bones and curves imported, as the editor does when it makes one from a mesh. */
	FDcrRig CreateModularRig(const FString& PackagePath, USkeletalMesh* Mesh, FBehaviorReport& Report)
	{
		UObject* Asset = nullptr;
		{
			FDcrQuietCompileScope Quiet;
			if constexpr (ModularRigKind == EDcrAssetKind::Legacy)
			{
				Asset = UControlRigBlueprintFactory::CreateNewControlRigAsset(PackagePath, true);
			}
			else
			{
				Asset = UControlRigAssetFactory::CreateNewControlRigAsset(PackagePath, true);
			}
		}
		const FDcrRig Rig = WrapDcrRig(Asset);
		if (!Rig.IsValid() || !Rig.ControlRig->IsModularRig())
		{
			Report.Problems.Add(FString::Printf(TEXT("a modular rig could not be created at %s"), *PackagePath));
			return FDcrRig();
		}

		// The mesh's bones and curves, as the editor imports them for a modular rig.
		URigHierarchyController* Controller = Rig.ControlRig->GetHierarchyController();
		Controller->ImportBones(Mesh->GetRefSkeleton(), NAME_None, false, false, false, false);
		Controller->ImportCurvesFromSkeletalMesh(Mesh, NAME_None, false, false);
		Rig.ControlRig->SetPreviewMesh(Mesh);
		Rig.ControlRig->PropagateHierarchyFromBPToInstances();
		Report.Notes.Add(FString::Printf(TEXT("modular rig %s (%s kind) on %s: %d bones"), *Rig.Asset->GetPathName(), LexToString(Rig.GetKind()),
			*Mesh->GetName(), Rig.ControlRig->GetHierarchy()->GetBoneKeys(true).Num()));
		return Rig;
	}

	/**
	 * The rig as it runs in the editor: the instance the asset keeps to answer questions about the hierarchy (which
	 * elements exist, what a connector may resolve to). Adding a module makes it; it is made here if that did not.
	 */
	UModularRig* FindDebuggedRig(const FDcrRig& Rig, TStrongObjectPtr<UControlRig>& KeepAlive)
	{
		UControlRig* Debugged = Rig.ControlRig->GetDebuggedControlRig();
		if (!Debugged)
		{
			// A fallback: RecompileModularRig has usually made the instance by the time a module is added.
			Rig.RigVM->SetObjectBeingDebugged(Rig.ControlRig->CreateControlRig());
			Debugged = Rig.ControlRig->GetDebuggedControlRig();
		}
		KeepAlive.Reset(Debugged);
		return Cast<UModularRig>(Debugged);
	}

	/** Lets the running instance construct itself, so the elements the modules make (their sockets above all) exist. */
	void SettleRig(UControlRig& Debugged)
	{
		// One evaluation runs the whole construction of every module, as the editor's ticking does.
		Debugged.RequestConstruction();
		Debugged.SetDeltaTime(1.f / 30.f);
		Debugged.Evaluate_AnyThread();
	}

	EBipedSide DetectSide(const FString& Text)
	{
		const FString Lowered = Text.ToLower();
		bool bLeft = Lowered.Contains(TEXT("left"));
		bool bRight = Lowered.Contains(TEXT("right"));
		// A lone l or r between separators: arm_l, Arm R, thigh.l
		FString Token;
		auto EndToken = [&]()
		{
			if (Token == TEXT("l"))
			{
				bLeft = true;
			}
			if (Token == TEXT("r"))
			{
				bRight = true;
			}
			Token.Reset();
		};
		for (const TCHAR Char : Lowered)
		{
			if (FChar::IsAlnum(Char))
			{
				Token.AppendChar(Char);
			}
			else
			{
				EndToken();
			}
		}
		EndToken();
		if (bLeft == bRight)
		{
			return EBipedSide::None;
		}
		return bLeft ? EBipedSide::Left : EBipedSide::Right;
	}

	FString DescribeElement(const URigHierarchy& Hierarchy, const FRigElementKey& Key)
	{
		FString Description;
		if (const FRigSocketElement* Socket = Hierarchy.Find<FRigSocketElement>(Key))
		{
			Description = Socket->GetDescription(&Hierarchy);
		}
		return FString::Printf(TEXT("%s (parent %s, \"%s\")"), *Key.ToString(), *Hierarchy.GetFirstParent(Key).ToString(), *Description);
	}

	FString DescribeMatches(const URigHierarchy& Hierarchy, const FModularRigResolveResult& Matches)
	{
		TArray<FString> Lines;
		for (const FRigElementResolveResult& Match : Matches.GetMatches())
		{
			if (Lines.Num() >= 3 * MaxReported)
			{
				Lines.Add(TEXT("..."));
				break;
			}
			Lines.Add(DescribeElement(Hierarchy, Match.GetKey()));
		}
		return Lines.IsEmpty() ? FString(TEXT("none")) : FString::Join(Lines, TEXT("; "));
	}

	/**
	 * Picks the socket a module goes onto from what its primary connector may resolve to. A socket scores for each of
	 * the module's words found in its description (the root and the limbs label their sockets "Drag and drop the Arm
	 * module here"), its name and its parent bone; the side counts when the module has one. Sockets taken by an
	 * earlier module are left out, so equal sockets are given out one by one, in key order.
	 */
	bool ChooseTarget(const URigHierarchy& Hierarchy, const FModularRigResolveResult& Matches, const FBipedModule& Part,
		const TSet<FRigElementKey>& Used, FRigElementKey& OutTarget)
	{
		int32 BestScore = 0;
		FString BestText;
		for (const FRigElementResolveResult& Match : Matches.GetMatches())
		{
			const FRigElementKey& Key = Match.GetKey();
			if (Used.Contains(Key))
			{
				continue;
			}
			FString Description;
			if (const FRigSocketElement* Socket = Hierarchy.Find<FRigSocketElement>(Key))
			{
				Description = Socket->GetDescription(&Hierarchy);
			}
			const FString Name = Key.Name.ToString();
			const FString Parent = Hierarchy.GetFirstParent(Key).Name.ToString();

			int32 Score = 0;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Part.Words) && Part.Words[Index]; ++Index)
			{
				Score += Description.Contains(Part.Words[Index]) ? 4 : 0;
				Score += Name.Contains(Part.Words[Index]) ? 2 : 0;
				Score += Parent.Contains(Part.Words[Index]) ? 1 : 0;
			}
			if (Score == 0)
			{
				continue;
			}
			if (Part.Side != EBipedSide::None)
			{
				const EBipedSide Side = DetectSide(Name + TEXT(" ") + Description + TEXT(" ") + Parent);
				if (Side != EBipedSide::None && Side != Part.Side)
				{
					continue;
				}
				Score += Side == Part.Side ? 8 : 0;
			}
			Score += Match.GetState() == ERigElementResolveState::DefaultTarget ? 1 : 0;

			const FString Text = Key.ToString();
			if (Score > BestScore || (Score == BestScore && Text < BestText))
			{
				BestScore = Score;
				BestText = Text;
				OutTarget = Key;
			}
		}
		return BestScore > 0;
	}

	/** The names of the connectors of a module that must be resolved and are not. */
	TArray<FString> UnresolvedConnectors(const FDcrRig& Rig, const FControlRigAssetStrongReference& Source, const FName& ModuleName)
	{
		TArray<FString> Missing;
		const FModularRigConnections& Connections = Rig.ControlRig->GetModularRigModel().Connections;
		for (const FRigModuleConnector& Exposed : Source.GetRigModuleSettings().ExposedConnectors)
		{
			if (!Exposed.IsOptional() && !Connections.HasConnection(MakeConnectorKey(ModuleName, Exposed.Name)))
			{
				Missing.Add(Exposed.Name);
			}
		}
		return Missing;
	}

	TArray<FString> DescribeLayout(const FDcrRig& Rig)
	{
		TArray<FString> Lines;
		const FModularRigModel& ModularModel = Rig.ControlRig->GetModularRigModel();
		for (const FRigModuleReference& Module : ModularModel.Modules)
		{
			Lines.Add(FString::Printf(TEXT("module %s under %s"), *Module.Name.ToString(), Module.ParentModuleName.IsNone() ? TEXT("the top") : *Module.ParentModuleName.ToString()));
		}
		for (const FModularRigSingleConnection& Connection : ModularModel.Connections)
		{
			TArray<FString> Targets;
			for (const FRigElementKey& Target : Connection.Targets)
			{
				Targets.Add(Target.ToString());
			}
			Lines.Add(FString::Printf(TEXT("%s connects to %s"), *Connection.Connector.ToString(), *FString::Join(Targets, TEXT(", "))));
		}
		Lines.Sort();
		return Lines;
	}

	/**
	 * A biped put together on the mesh from the given module assets (by corpus name). The root goes onto the root
	 * bone; every other module is added, its primary connector is connected to the socket ChooseTarget picks, and
	 * the modules' connector events resolve the secondary connectors. Whatever is left open is in the report.
	 */
	FBehaviorAssembly AssembleBiped(const FString& RigPath, USkeletalMesh* Mesh, const TMap<FString, FDcrRig>& Sources)
	{
		FBehaviorAssembly Out;
		FBehaviorReport& Report = Out.Report;
		// The modules compile and construct as they are added; their messages are not this test's.
		FDcrQuietCompileScope Quiet;

		Out.Rig = CreateModularRig(RigPath, Mesh, Report);
		if (!Out.Rig.IsValid())
		{
			return Out;
		}
		UModularRigController* Controller = Out.Rig.ControlRig->GetModularRigController();
		if (!Controller)
		{
			Report.Problems.Add(TEXT("the modular rig has no module controller"));
			return Out;
		}

		TSet<FRigElementKey> UsedTargets;
		TStrongObjectPtr<UControlRig> KeepAlive;
		for (const FBipedModule& Part : GetBipedModules())
		{
			const FDcrRig* Source = Sources.Find(FString(Part.Source));
			if (!Source || !Source->IsValid())
			{
				Report.Problems.Add(FString::Printf(TEXT("no asset for the %s module"), Part.Source));
				return Out;
			}
			const FControlRigAssetStrongReference Reference(Source->Asset);
			const bool bIsRoot = FCString::Strcmp(Part.Source, TEXT("Root")) == 0;

			// The new modular rig comes with the project's default root module; the root of this biped replaces it.
			FName ModuleName(Part.Instance);
			if (bIsRoot && Controller->FindModule(ModuleName))
			{
				if (!Controller->SwapModuleSource(ModuleName, Reference, false))
				{
					Report.Problems.Add(FString::Printf(TEXT("%s could not take the place of the default root module"), *Source->Asset->GetPathName()));
					return Out;
				}
			}
			else
			{
				ModuleName = Controller->AddModuleFromAssetReference(ModuleName, Reference, NAME_None, false);
				if (ModuleName.IsNone())
				{
					Report.Problems.Add(FString::Printf(TEXT("%s could not be added as %s (is it a rig module?)"), *Source->Asset->GetPathName(), Part.Instance));
					return Out;
				}
			}

			const FRigModuleConnector* Primary = Reference.GetRigModuleSettings().FindPrimaryConnector();
			if (!Primary)
			{
				Report.Problems.Add(FString::Printf(TEXT("module %s has no primary connector"), *ModuleName.ToString()));
				return Out;
			}
			const FRigElementKey PrimaryKey = MakeConnectorKey(ModuleName, Primary->Name);

			UModularRig* Debugged = FindDebuggedRig(Out.Rig, KeepAlive);
			if (!Debugged)
			{
				Report.Problems.Add(TEXT("the modular rig has no running instance to resolve connectors against"));
				return Out;
			}
			SettleRig(*Debugged);

			// Where the primary connector goes: the root bone for the root, a socket for the others.
			FRigElementKey Target;
			if (bIsRoot)
			{
				const TArray<FRigElementKey> Bones = Out.Rig.ControlRig->GetHierarchy()->GetBoneKeys(true);
				if (Bones.IsEmpty())
				{
					Report.Problems.Add(TEXT("the modular rig has no bones for the root module to connect to"));
					return Out;
				}
				Target = Bones[0];
			}
			else
			{
				URigHierarchy* Hierarchy = Debugged->GetHierarchy();
				const FRigConnectorElement* Connector = Hierarchy->Find<FRigConnectorElement>(PrimaryKey);
				const FRigModuleInstance* Instance = Debugged->FindModule(ModuleName);
				UModularRigRuleManager* Rules = Hierarchy->GetRuleManager();
				if (!Connector || !Instance || !Rules)
				{
					Report.Problems.Add(FString::Printf(TEXT("connector %s is not in the running rig (connector %s, module instance %s, rules %s)"), *PrimaryKey.ToString(),
						Connector ? TEXT("found") : TEXT("missing"), Instance ? TEXT("found") : TEXT("missing"), Rules ? TEXT("found") : TEXT("missing")));
					return Out;
				}
				const FModularRigResolveResult Matches = Rules->FindMatches(Connector, Instance, Debugged->GetElementKeyRedirector());
				if (!ChooseTarget(*Hierarchy, Matches, Part, UsedTargets, Target))
				{
					Report.Problems.Add(FString::Printf(TEXT("no socket fits module %s; its primary connector %s can go to: %s"), *ModuleName.ToString(),
						*PrimaryKey.ToString(), *DescribeMatches(*Hierarchy, Matches)));
					return Out;
				}
				UsedTargets.Add(Target);
				Report.Notes.Add(FString::Printf(TEXT("%s goes onto %s"), *ModuleName.ToString(), *DescribeElement(*Hierarchy, Target)));
			}

			// As the editor does: the root without a check (nothing is there to check against until it is connected),
			// the others checked against the rules, with the secondary connectors resolved by their modules.
			// The root is connected as the editor connects it in OnPostConstruction (ControlRigEditor.cpp).
			if (!Controller->ConnectConnectorToElement(PrimaryKey, Target, false, !bIsRoot, !bIsRoot))
			{
				Report.Problems.Add(FString::Printf(TEXT("connector %s could not be connected to %s"), *PrimaryKey.ToString(), *Target.ToString()));
				return Out;
			}
			Debugged = FindDebuggedRig(Out.Rig, KeepAlive);
			if (!Debugged)
			{
				Report.Problems.Add(TEXT("the modular rig lost its running instance"));
				return Out;
			}
			SettleRig(*Debugged);

			// Secondary connectors that were not resolved on the connect: ask the module for them again, as the editor does.
			for (int32 Round = 0; Round < MaxResolveRounds && !UnresolvedConnectors(Out.Rig, Reference, ModuleName).IsEmpty(); ++Round)
			{
				Controller->AutoConnectModules(TArray<FName>{ ModuleName }, false, false);
				SettleRig(*Debugged);
			}
			const TArray<FString> Missing = UnresolvedConnectors(Out.Rig, Reference, ModuleName);
			if (!Missing.IsEmpty())
			{
				Report.Problems.Add(FString::Printf(TEXT("module %s is left with connectors nothing resolved: %s"), *ModuleName.ToString(), *FString::Join(Missing, TEXT(", "))));
			}

			if (bIsRoot)
			{
				Report.Notes.Add(FString::Printf(TEXT("after the root: %d sockets"), Debugged->GetHierarchy()->GetSocketKeys(true).Num()));
				if (Debugged->GetHierarchy()->GetSocketKeys(true).IsEmpty())
				{
					Report.Problems.Add(TEXT("the root module made no sockets, so there is nothing for the other modules to connect to"));
					return Out;
				}
			}
		}

		Out.Layout = DescribeLayout(Out.Rig);
		return Out;
	}

	// ---------------------------------------------------------------- Looking at a run

	int32 CountKeys(const FDcrRun& Run, ERigElementType Type)
	{
		int32 Count = 0;
		for (const FRigElementKey& Key : Run.Keys)
		{
			Count += Key.Type == Type ? 1 : 0;
		}
		return Count;
	}

	/** How many elements each module of a run made, by module name; elements outside any module are counted under (rig). */
	TMap<FString, int32> CountElementsByModule(const FDcrRun& Run)
	{
		TMap<FString, int32> Counts;
		for (const FRigElementKey& Key : Run.Keys)
		{
			// A module's elements are named Module:Element in the running hierarchy.
			const FRigHierarchyModulePath Path(Key.Name);
			++Counts.FindOrAdd(Path.IsValid() ? FString(Path.GetModuleNameString()) : FString(TEXT("(rig)")));
		}
		return Counts;
	}

	FString DescribeModuleCounts(const TMap<FString, int32>& Counts)
	{
		TArray<FString> Parts;
		for (const TPair<FString, int32>& Pair : Counts)
		{
			Parts.Add(FString::Printf(TEXT("%s %d"), *Pair.Key, Pair.Value));
		}
		Parts.Sort();
		return FString::Join(Parts, TEXT(", "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrBehaviorModularTest, "DreamAnim.Rig.Behavior.Modular",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * A biped of the engine's modules and the same biped of the modules rebuilt from text: the same module tree and
 * connections, and 60 frames of the same input move every bone, null and control to the same place bit for bit.
 */
bool FDcrBehaviorModularTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::BehaviorPrivate;

	// The engine's modules, and each of them through the text.
	TMap<FString, FDcrRig> Originals;
	TMap<FString, FDcrRig> Rebuilts;
	bool bAllModules = true;
	for (const FBipedModule& Part : GetBipedModules())
	{
		const FString Name = Part.Source;
		if (Originals.Contains(Name))
		{
			continue;
		}
		const FDcrCorpusEntry* Entry = FindModuleEntry(Name);
		const FDcrRig FromEngine = Entry ? LoadDcrRig(Entry->ObjectPath) : FDcrRig();
		if (!FromEngine.IsValid())
		{
			AddError(FString::Printf(TEXT("Cannot load the %s module."), *Name));
			bAllModules = false;
			continue;
		}
		Originals.Add(Name, FromEngine);

		FBehaviorReport Report;
		const FDcrRig FromText = RebuildThroughText(*Entry, FromEngine, Report);
		FlushReport(*this, Name + TEXT(" rebuilt"), Report);
		if (FromText.IsValid() && Report.Problems.IsEmpty())
		{
			Rebuilts.Add(Name, FromText);
		}
		else
		{
			bAllModules = false;
		}
	}
	if (!bAllModules)
	{
		AddError(TEXT("Not every module could be loaded and rebuilt, so no rig was put together."));
		return false;
	}

	// The biped from the engine's modules, on the first mesh it connects fully on; then the same biped on that mesh from the rebuilt ones.
	FBehaviorAssembly Original;
	FBehaviorAssembly Rebuilt;
	FString MeshLabel;
	TArray<FString> Attempts;
	for (const FBipedMesh& Choice : GetBipedMeshes())
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, Choice.Path);
		if (!Mesh)
		{
			Attempts.Add(FString::Printf(TEXT("%s: the mesh cannot be loaded (%s)"), Choice.Label, Choice.Path));
			continue;
		}
		Original = AssembleBiped(FString::Printf(TEXT("/Game/DreamAnimRigTests/Behavior/Rigs/Original_%s"), Choice.Label), Mesh, Originals);
		if (!Original.Rig.IsValid() || !Original.Report.Problems.IsEmpty())
		{
			for (const FString& Problem : Original.Report.Problems)
			{
				Attempts.Add(FString::Printf(TEXT("%s: %s"), Choice.Label, *Problem));
			}
			continue;
		}
		MeshLabel = Choice.Label;
		Rebuilt = AssembleBiped(FString::Printf(TEXT("/Game/DreamAnimRigTests/Behavior/Rigs/Rebuilt_%s"), Choice.Label), Mesh, Rebuilts);
		break;
	}
	if (MeshLabel.IsEmpty())
	{
		AddError(TEXT("The engine's own modules could not be put together into a biped on any mesh:"));
		for (const FString& Attempt : Attempts)
		{
			AddError(TEXT("    ") + Attempt);
		}
		return false;
	}
	for (const FString& Attempt : Attempts)
	{
		AddInfo(TEXT("Another mesh was tried first. ") + Attempt);
	}
	AddInfo(FString::Printf(TEXT("The biped is on %s."), *MeshLabel));
	FlushReport(*this, TEXT("Original biped"), Original.Report);
	FlushReport(*this, TEXT("Rebuilt biped"), Rebuilt.Report);
	if (!Rebuilt.Rig.IsValid() || !Rebuilt.Report.Problems.IsEmpty())
	{
		AddError(TEXT("The modules rebuilt from text could not be put together into the same biped."));
		return false;
	}

	// The same modules in the same places, connected the same way.
	const TArray<FString> OnlyOriginal = LinesOnlyIn(Original.Layout, Rebuilt.Layout);
	const TArray<FString> OnlyRebuilt = LinesOnlyIn(Rebuilt.Layout, Original.Layout);
	for (int32 Index = 0; Index < OnlyOriginal.Num() && Index < MaxReported; ++Index)
	{
		AddError(TEXT("Only in the engine's biped: ") + OnlyOriginal[Index]);
	}
	for (int32 Index = 0; Index < OnlyRebuilt.Num() && Index < MaxReported; ++Index)
	{
		AddError(TEXT("Only in the rebuilt biped: ") + OnlyRebuilt[Index]);
	}
	if (OnlyOriginal.IsEmpty() && OnlyRebuilt.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("Both bipeds have the same module tree and %d lines of connections."), Original.Layout.Num()));
	}

	// Run both on the same input.
	const FDcrRun Expected = RunDcrRig(Original.Rig, BehaviorFrameCount, DriveDcrDefault);
	const FDcrRun Actual = RunDcrRig(Rebuilt.Rig, BehaviorFrameCount, DriveDcrDefault);
	if (!Expected.Error.IsEmpty() || !Actual.Error.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Running failed: %s %s"), *Expected.Error, *Actual.Error));
		return false;
	}
	const TMap<FString, int32> ExpectedCounts = CountElementsByModule(Expected);
	AddInfo(FString::Printf(TEXT("The engine's biped has %d elements (%d controls, %d nulls, %d bones, %d curves): %s"), Expected.Keys.Num(),
		CountKeys(Expected, ERigElementType::Control), CountKeys(Expected, ERigElementType::Null), CountKeys(Expected, ERigElementType::Bone),
		Expected.CurveKeys.Num(), *DescribeModuleCounts(ExpectedCounts)));
	AddInfo(FString::Printf(TEXT("The rebuilt biped has %d elements (%d controls, %d nulls, %d bones, %d curves): %s"), Actual.Keys.Num(),
		CountKeys(Actual, ERigElementType::Control), CountKeys(Actual, ERigElementType::Null), CountKeys(Actual, ERigElementType::Bone),
		Actual.CurveKeys.Num(), *DescribeModuleCounts(CountElementsByModule(Actual))));

	// A run that made nothing would match anything: the modules must have made controls.
	if (CountKeys(Expected, ERigElementType::Control) == 0)
	{
		AddError(TEXT("The engine's biped made no controls when it ran, so a match would prove nothing."));
		return false;
	}
	for (const FBipedModule& Part : GetBipedModules())
	{
		if (!ExpectedCounts.Contains(Part.Instance))
		{
			AddWarning(FString::Printf(TEXT("Module %s made no elements when the engine's biped ran."), Part.Instance));
		}
	}

	// The text builds float math as its double twin, which rounds otherwise: modules that have float math hold
	// to 1e-4 (and 1e-3 of a degree, which the rotations wound up through a chain reach); the others bit for bit.
	TArray<FString> WithFloatMath;
	for (const FBipedModule& Part : GetBipedModules())
	{
		const FDcrCorpusEntry* Entry = GetDcrCorpus().FindByPredicate([&Part](const FDcrCorpusEntry& Candidate) { return FCString::Strcmp(Candidate.Name, Part.Source) == 0; });
		UE::DreamAnim::Model::FDcrModel RigModel;
		if (Entry && ExtractDcrModel(LoadDcrRig(Entry->ObjectPath), RigModel).bSuccess && HasFloatMath(RigModel))
		{
			WithFloatMath.AddUnique(Part.Source);
		}
	}
	const FDcrRunDiff Diff = CompareDcrRuns(Expected, Actual);
	if (Diff.IsExact())
	{
		AddInfo(FString::Printf(TEXT("The rebuilt biped runs bit for bit as the engine's does over %d frames: %s"), BehaviorFrameCount, *Diff.Describe()));
	}
	else if (!WithFloatMath.IsEmpty() && Diff.bComparable && Diff.MaxTranslation <= 1e-4 && Diff.MaxRotationDegrees <= 1e-3 && Diff.MaxScale <= 1e-4 && Diff.MaxCurve <= 1e-4)
	{
		AddInfo(FString::Printf(TEXT("The rebuilt biped runs as the engine's does within the rounding of float math built in double (%s): %s"),
			*FString::Join(WithFloatMath, TEXT(", ")), *Diff.Describe()));
	}
	else
	{
		AddError(FString::Printf(TEXT("The rebuilt biped runs differently from the engine's%s: %s"),
			WithFloatMath.IsEmpty() ? TEXT(" (no float math, so it should be bit for bit)") : TEXT(" past the rounding of float math"), *Diff.Describe()));
	}
	return true;
}

#endif

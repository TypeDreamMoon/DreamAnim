// Copyright DreamMoon. All Rights Reserved.
//
// The driver: the source index over files in a scratch folder, a compile through an included library,
// formatting, the decompiler against its golden, where decompiled text goes by default, the two
// diagnostic forms tools read, and the command line's check mode run in this process.

#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimStateNode.h"
#include "Commandlets/Commandlet.h"
#include "Driver/DabDriver.h"
#include "DreamAnimSet.h"
#include "EdGraphNode_Comment.h"
#include "Generate/DabGenerate.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::DriverTests
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Compiler;

	/** A clean folder under <project>/Saved/DreamAnimTests/Driver/, spelled as the driver spells paths. */
	FString MakeDriverScratchDir(const TCHAR* Name)
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests"), TEXT("Driver"), Name));
		FPaths::NormalizeFilename(Dir);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}

	bool WriteDriverFile(FAutomationTestBase& Test, const FString& Path, const FString& Text)
	{
		if (FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return true;
		}
		Test.AddError(FString::Printf(TEXT("Cannot write %s"), *Path));
		return false;
	}

	FString WithLfLineEnds(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
	}

	/** The first line two texts differ on, for a failure message; empty when they are the same. */
	FString DescribeDriverDifference(const FString& Expected, const FString& Actual)
	{
		TArray<FString> ExpectedLines;
		TArray<FString> ActualLines;
		WithLfLineEnds(Expected).ParseIntoArray(ExpectedLines, TEXT("\n"), false);
		WithLfLineEnds(Actual).ParseIntoArray(ActualLines, TEXT("\n"), false);
		for (int32 Index = 0; Index < FMath::Max(ExpectedLines.Num(), ActualLines.Num()); ++Index)
		{
			const FString Left = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index] : TEXT("<end>");
			const FString Right = ActualLines.IsValidIndex(Index) ? ActualLines[Index] : TEXT("<end>");
			if (!Left.Equals(Right, ESearchCase::CaseSensitive))
			{
				return FString::Printf(TEXT("line %d: expected `%s`, got `%s`"), Index + 1, *Left.TrimStart(), *Right.TrimStart());
			}
		}
		return FString();
	}

	/** Paths one per line, so a failed comparison shows them. */
	FString JoinPaths(const TArray<FString>& Paths)
	{
		return FString::Join(Paths, TEXT("\n"));
	}

	FString GetDreamAnimPluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FString();
	}

	void ReportCompileErrors(FAutomationTestBase& Test, const FDabCompileResult& Result)
	{
		for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverSourceIndexTest, "DreamAnim.Driver.SourceIndex",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverSourceIndexTest::RunTest(const FString& Parameters)
	{
		const FString Root = MakeDriverScratchDir(TEXT("Index"));
		const FString Common = Root / TEXT("Lib/Common.dat");
		const FString Extra = Root / TEXT("Lib/Extra.dat");
		const FString Hero = Root / TEXT("Chars/AB_Hero.dab");
		const FString Other = Root / TEXT("Chars/AB_Other.dab");
		const FString Layers = Root / TEXT("ALI_Layers.dai");
		const FString Added = Root / TEXT("Chars/AB_Added.dab");

		bool bWritten = WriteDriverFile(*this, Common, TEXT(
			"AnimTemplates(Name=\"DreamAnimTests/Driver/Common\")\n"
			"{\n"
			"    Template Frame(Seq, N) = SequenceEvaluator(Sequence = Seq, ExplicitFrame = N);\n"
			"}\n"));
		// Extra includes Common next to it; Hero reaches Extra only through the root.
		bWritten &= WriteDriverFile(*this, Extra, TEXT(
			"#include \"Common.dat\"\n"
			"AnimTemplates(Name=\"DreamAnimTests/Driver/Extra\")\n"
			"{\n"
			"    Template Still(Seq) = Frame(Seq, 0);\n"
			"}\n"));
		bWritten &= WriteDriverFile(*this, Hero, TEXT(
			"#include \"Lib/Extra.dat\"\n"
			"AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Hero\", Root=\"Game\")\n"
			"{\n"
			"    Graph = { Output = LocalRefPose(); }\n"
			"}\n"));
		bWritten &= WriteDriverFile(*this, Other, TEXT("AnimBlueprint(Name=\"Driver/AB_Other\", Root=\"Plugin.DreamAnimDriverTest\") { }\n"));
		bWritten &= WriteDriverFile(*this, Layers, TEXT("AnimLayerInterface(Name=\"DreamAnimTests/Driver/ALI_Layers\") { }\n"));
		if (!bWritten)
		{
			return false;
		}

		FDabSourceIndex Index;
		Index.Scan({ Root + TEXT("/") });
		TestEqualSensitive(TEXT("one root, without its trailing slash"), JoinPaths(Index.GetRoots()), Root);
		TestEqual(TEXT("every source under the root"), Index.GetEntries().Num(), 5);

		const FDabSourceEntry* HeroEntry = Index.FindByFile(Hero);
		if (!TestNotNull(TEXT("the blueprint is indexed"), HeroEntry))
		{
			return false;
		}
		TestEqual(TEXT("blueprint kind"), HeroEntry->Kind, EDabDocumentKind::AnimBlueprint);
		TestEqualSensitive(TEXT("blueprint asset"), HeroEntry->AssetPath, FString(TEXT("/Game/DreamAnimTests/Driver/AB_Hero.AB_Hero")));
		TestEqualSensitive(TEXT("an include found through the root"), JoinPaths(HeroEntry->Includes), Extra);

		const FDabSourceEntry* ExtraEntry = Index.FindByFile(Extra);
		if (TestNotNull(TEXT("the library is indexed"), ExtraEntry))
		{
			TestEqual(TEXT("library kind"), ExtraEntry->Kind, EDabDocumentKind::AnimTemplates);
			TestTrue(TEXT("a library makes no asset"), ExtraEntry->AssetPath.IsEmpty());
			TestEqualSensitive(TEXT("an include next to the library"), JoinPaths(ExtraEntry->Includes), Common);
		}
		const FDabSourceEntry* LayersEntry = Index.FindByFile(Layers);
		if (TestNotNull(TEXT("the interface is indexed"), LayersEntry))
		{
			TestEqual(TEXT("interface kind"), LayersEntry->Kind, EDabDocumentKind::AnimLayerInterface);
			TestEqualSensitive(TEXT("no Root means Game"), LayersEntry->AssetPath, FString(TEXT("/Game/DreamAnimTests/Driver/ALI_Layers.ALI_Layers")));
		}

		TestEqualSensitive(TEXT("a library's dependents, through another library"), JoinPaths(Index.FindDependents(Common)), Hero);
		TestEqualSensitive(TEXT("a library's direct dependents"), JoinPaths(Index.FindDependents(Extra)), Hero);
		TestTrue(TEXT("nothing includes a blueprint"), Index.FindDependents(Hero).IsEmpty());

		TestTrue(TEXT("by object path"), Index.FindByAsset(TEXT("/Game/DreamAnimTests/Driver/AB_Hero.AB_Hero")) == HeroEntry);
		TestTrue(TEXT("by package path"), Index.FindByAsset(TEXT("/Game/DreamAnimTests/Driver/AB_Hero")) == HeroEntry);
		TestTrue(TEXT("by export text path"), Index.FindByAsset(TEXT("AnimBlueprint'/Game/DreamAnimTests/Driver/AB_Hero.AB_Hero'")) == HeroEntry);
		const FDabSourceEntry* OtherByAsset = Index.FindByAsset(TEXT("/DreamAnimDriverTest/Driver/AB_Other"));
		TestTrue(TEXT("a plugin root is the plugin's mount"), OtherByAsset && OtherByAsset->FilePath == Other);
		TestNull(TEXT("an asset no source makes"), Index.FindByAsset(TEXT("/Game/DreamAnimTests/Driver/AB_Nobody")));
		TestEqualSensitive(TEXT("by bare asset name"), Index.FindByAssetName(TEXT("AB_Hero")), FString(TEXT("/Game/DreamAnimTests/Driver/AB_Hero.AB_Hero")));
		TestTrue(TEXT("no source makes an asset of that name"), Index.FindByAssetName(TEXT("AB_Nobody")).IsEmpty());

		// An edit: the include goes and the asset is renamed.
		WriteDriverFile(*this, Hero, TEXT("AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Hero2\") { Graph = { Output = LocalRefPose(); } }\n"));
		Index.Rescan(Hero);
		TestNull(TEXT("the old asset is gone after the edit"), Index.FindByAsset(TEXT("/Game/DreamAnimTests/Driver/AB_Hero")));
		TestNotNull(TEXT("the new asset is found after the edit"), Index.FindByAsset(TEXT("/Game/DreamAnimTests/Driver/AB_Hero2")));
		TestTrue(TEXT("the library has no dependents after the edit"), Index.FindDependents(Common).IsEmpty());

		// A file appears.
		WriteDriverFile(*this, Added, TEXT(
			"#include \"../Lib/Common.dat\"\n"
			"AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Added\") { Graph = { Output = LocalRefPose(); } }\n"));
		Index.Rescan(Added);
		TestNotNull(TEXT("a new file is indexed"), Index.FindByFile(Added));
		TestEqualSensitive(TEXT("the new file depends on the library"), JoinPaths(Index.FindDependents(Common)), Added);

		// A file goes away.
		IFileManager::Get().Delete(*Other);
		Index.Rescan(Other);
		TestNull(TEXT("a removed file leaves the index"), Index.FindByFile(Other));
		TestEqual(TEXT("entries after one added and one removed"), Index.GetEntries().Num(), 5);

		// A library goes away: the document still names it, so a rebuild of what waits for it can be found.
		IFileManager::Get().Delete(*Common);
		Index.Rescan(Common);
		TestNull(TEXT("a removed library leaves the index"), Index.FindByFile(Common));
		TestEqualSensitive(TEXT("a removed library still has its dependents"), JoinPaths(Index.FindDependents(Common)), Added);
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverCompileTest, "DreamAnim.Driver.Compile",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverCompileTest::RunTest(const FString& Parameters)
	{
		const FString Dir = MakeDriverScratchDir(TEXT("Compile"));
		const FString Library = Dir / TEXT("Lib/Frames.dat");
		const FString BrokenLibrary = Dir / TEXT("Lib/Broken.dat");
		if (!WriteDriverFile(*this, Library, TEXT(
				"AnimTemplates(Name=\"DreamAnimTests/Driver/Frames\")\n"
				"{\n"
				"    Template Frame(Seq, N) = SequenceEvaluator(Sequence = Seq, ExplicitFrame = N);\n"
				"}\n"))
			|| !WriteDriverFile(*this, BrokenLibrary, TEXT(
				"AnimTemplates(Name=\"DreamAnimTests/Driver/Broken\")\n"
				"{\n"
				"    Template Frame(Seq, N) = SequenceEvaluator(Sequence = Seq ExplicitFrame = N);\n"
				"}\n")))
		{
			return false;
		}

		const TCHAR* const Body = TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Compile\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
			"    Assets = { AnimSequence Pose; }\n"
			"    Graph = { Output = Frame(Assets.Pose, 0); }\n"
			"}\n");

		FDabCompileOptions Options;
		Options.bGenerate = false;
		Options.IncludeRoots = { Dir };
		FDabProvenance Provenance;
		Options.Provenance = &Provenance;

		// The file itself is never written: the text is what an editor buffer would hold.
		const FString MainPath = Dir / TEXT("AB_Compile.dab");
		{
			const FDabCompileResult Result = CompileDabText(MainPath, FString(TEXT("#include \"Lib/Frames.dat\"\n")) + Body, Options);
			ReportCompileErrors(*this, Result);
			TestEqual(TEXT("no errors"), Result.NumErrors, 0);
			TestEqualSensitive(TEXT("the result names the file"), Result.FilePath, MainPath);
			TestEqualSensitive(TEXT("asset path from the header"), Result.Model.AssetPath, FString(TEXT("/Game/DreamAnimTests/Driver/AB_Compile.AB_Compile")));
			TestNull(TEXT("nothing generated"), Result.Blueprint);
			if (TestTrue(TEXT("the unit is kept"), Result.Unit.IsValid()))
			{
				TestEqual(TEXT("one library"), Result.Unit->Libraries.Num(), 1);
				TestTrue(TEXT("the library by its path"), Result.Unit->LibrarySources.Num() == 1 && Result.Unit->LibrarySources[0].GetPath() == Library);
			}
		}

		// An error inside a library is reported against the library.
		{
			const FDabCompileResult Result = CompileDabText(MainPath, FString(TEXT("#include \"Lib/Broken.dat\"\n")) + Body, Options);
			TestTrue(TEXT("a broken library fails the compile"), Result.NumErrors > 0);
			const FDabDiagnostic* InLibrary = Result.Diagnostics.FindByPredicate([&BrokenLibrary](const FDabDiagnostic& Diagnostic)
			{
				return Diagnostic.Severity == EDabSeverity::Error && Diagnostic.FilePath == BrokenLibrary;
			});
			if (TestNotNull(TEXT("an error carries the library's path"), InLibrary))
			{
				TestEqualSensitive(TEXT("its code"), InLibrary->Code, FString(TEXT("DAB2001")));
				TestEqual(TEXT("its line in the library"), InLibrary->Span.Line, 3);
				TestTrue(TEXT("the clickable line names the library"), FormatDabDiagnosticLine(*InLibrary).StartsWith(BrokenLibrary + TEXT("(3,")));
			}
			TestTrue(TEXT("every diagnostic names a file"), !Result.Diagnostics.ContainsByPredicate([](const FDabDiagnostic& Diagnostic) { return Diagnostic.FilePath.IsEmpty(); }));
		}

		// A template called wrongly is reported once, not again as an unknown node type.
		{
			const FDabCompileResult Result = CompileDabText(MainPath, FString(TEXT("#include \"Lib/Frames.dat\"\n")) + FString(Body).Replace(TEXT("Frame(Assets.Pose, 0)"), TEXT("Frame(Assets.Pose, 0, Extra = 1)")), Options);
			TestTrue(TEXT("the unknown argument is reported"), Result.Diagnostics.ContainsByPredicate([](const FDabDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DAB3102"); }));
			TestFalse(TEXT("and nothing more about the call"), Result.Diagnostics.ContainsByPredicate([](const FDabDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DAB3001"); }));
		}

		// A library on its own: its syntax and templates are checked, and that it builds nothing is no error.
		{
			const FDabCompileResult Good = CompileDabFile(Library, Options);
			TestEqual(TEXT("a sound library alone has no errors"), Good.NumErrors, 0);
			const FDabCompileResult Broken = CompileDabFile(BrokenLibrary, Options);
			TestTrue(TEXT("a broken library alone reports its error"), Broken.Diagnostics.ContainsByPredicate([](const FDabDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DAB2001"); }));
		}

		// A file that is not there.
		{
			const FDabCompileResult Result = CompileDabFile(Dir / TEXT("Missing.dab"), Options);
			TestEqual(TEXT("a missing file is one error"), Result.NumErrors, 1);
			TestFalse(TEXT("a missing file has no unit"), Result.Unit.IsValid());
			TestTrue(TEXT("a missing file says so"), Result.Diagnostics.Num() == 1 && Result.Diagnostics[0].Code == TEXT("DAB6014"));
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverFormatTest, "DreamAnim.Driver.Format",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverFormatTest::RunTest(const FString& Parameters)
	{
		const FString Dir = FPaths::Combine(GetDreamAnimPluginDir(), TEXT("Tests"), TEXT("Corpus"), TEXT("Format"));
		const FString MessyPath = Dir / TEXT("fmt_messy.dab");
		FString Messy;
		FString Expected;
		if (!FFileHelper::LoadFileToString(Messy, *MessyPath) || !FFileHelper::LoadFileToString(Expected, *(Dir / TEXT("fmt_messy.formatted"))))
		{
			AddError(FString::Printf(TEXT("Cannot read the format corpus in %s"), *Dir));
			return false;
		}

		FString Formatted;
		TArray<FDabDiagnostic> Diagnostics;
		TestTrue(TEXT("messy text formats"), FormatDabText(MessyPath, Messy, Formatted, Diagnostics));
		TestTrue(TEXT("no errors"), !Diagnostics.ContainsByPredicate([](const FDabDiagnostic& Diagnostic) { return Diagnostic.Severity == EDabSeverity::Error; }));
		const FString Difference = DescribeDriverDifference(Expected, Formatted);
		TestTrue(FString::Printf(TEXT("the canonical text (%s)"), *Difference), Difference.IsEmpty());

		FString Again;
		TArray<FDabDiagnostic> AgainDiagnostics;
		TestTrue(TEXT("canonical text formats"), FormatDabText(MessyPath, Formatted, Again, AgainDiagnostics));
		TestEqualSensitive(TEXT("formatting twice changes nothing"), Again, Formatted);

		// A text that does not parse has no canonical form: printing what parsed would drop what the author wrote.
		const FString BrokenPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests/Driver/Broken.dab")));
		FString BrokenOut = TEXT("left over");
		TArray<FDabDiagnostic> BrokenDiagnostics;
		TestFalse(TEXT("broken text does not format"), FormatDabText(BrokenPath, TEXT("AnimBlueprint(Name=\"X\") { Graph = { Output = ; } }\n"), BrokenOut, BrokenDiagnostics));
		TestTrue(TEXT("no text for broken input"), BrokenOut.IsEmpty());
		TestTrue(TEXT("its errors name the file"), BrokenDiagnostics.Num() > 0 && BrokenDiagnostics[0].FilePath == BrokenPath.Replace(TEXT("\\"), TEXT("/")));
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverDecompileTest, "DreamAnim.Driver.Decompile",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverDecompileTest::RunTest(const FString& Parameters)
	{
		const UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, TEXT("/ALS/ALS/Character/AB_Als.AB_Als"));
		if (!TestNotNull(TEXT("AB_Als loads"), Blueprint))
		{
			return false;
		}
		const FString GoldenPath = FPaths::Combine(GetDreamAnimPluginDir(), TEXT("Tests"), TEXT("Corpus"), TEXT("Decompiled"), TEXT("Character_AB_Als.dab"));
		FString Golden;
		if (!FFileHelper::LoadFileToString(Golden, *GoldenPath))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *GoldenPath));
			return false;
		}

		const FDabDecompileResult Result = DecompileDabBlueprint(*Blueprint);
		TestTrue(TEXT("decompiles"), Result.bSucceeded);
		for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			AddInfo(FormatDabDiagnosticLine(Diagnostic));
		}
		const FString Difference = DescribeDriverDifference(Golden, Result.Text);
		TestTrue(FString::Printf(TEXT("the text is the golden's (%s)"), *Difference), Difference.IsEmpty());
		// The golden lists nothing unsupported at its top.
		TestEqual(TEXT("nothing unsupported"), Result.NumUnsupported, 0);
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverCompilerMessagePlaceTest, "DreamAnim.Driver.CompilerMessagePlace",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	/**
	 * A node the language accepts but the Blueprint compiler rejects (a player set to sync with no group):
	 * the compiler's error lands on the node's call in the text, in the AnimGraph and inside a state.
	 */
	bool FDabDriverCompilerMessagePlaceTest::RunTest(const FString& Parameters)
	{
		const FString Path = MakeDriverScratchDir(TEXT("CompilerMessages")) / TEXT("AB_Messages.dab");
		const FString Text = TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/CompilerMessages/AB_Messages\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
			"\n"
			"    Graph = {\n"
			"        Output = TwoWayBlend(\n"
			"            Alpha = 0.5,\n"
			"            A     = Locomotion,\n"
			"            B     = SequencePlayer(Sequence = A_Als_Stop_Left, Method = SyncGroup)\n"
			"        );\n"
			"    }\n"
			"\n"
			"    StateMachine Locomotion\n"
			"    {\n"
			"        Entry = Idle;\n"
			"\n"
			"        State Idle\n"
			"        {\n"
			"            Pose = SequencePlayer(Sequence = A_Als_Stop_Right, Method = SyncGroup);\n"
			"        }\n"
			"    }\n"
			"}\n");
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(Path, Text, Options);

		TArray<FString> Places;
		for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			AddInfo(FormatDabDiagnosticLine(Diagnostic));
			if (Diagnostic.Code == TEXT("DAB7100"))
			{
				Places.AddUnique(FString::Printf(TEXT("%d,%d"), Diagnostic.Span.Line, Diagnostic.Span.Column));
			}
		}
		Places.Sort();
		TestEqual(TEXT("the compiler's errors land on both players' calls"), FString::Join(Places, TEXT(" ")), FString(TEXT("19,20 9,21")));
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverRegionsTest, "DreamAnim.Driver.Regions",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	/** `#Region` becomes a comment box around what its statements made: named poses in a graph, states in a machine. */
	bool FDabDriverRegionsTest::RunTest(const FString& Parameters)
	{
		const FString Path = MakeDriverScratchDir(TEXT("Regions")) / TEXT("AB_Regions.dab");
		const FString Text = TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/Regions/AB_Regions\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
			"\n"
			"    Graph = {\n"
			"        #Region \"Stops\"\n"
			"        Left  = SequencePlayer(Sequence = A_Als_Stop_Left);\n"
			"        Right = SequencePlayer(Sequence = A_Als_Stop_Right);\n"
			"        #EndRegion\n"
			"\n"
			"        Output = TwoWayBlend(Alpha = 0.5, A = TwoWayBlend(Alpha = 0.5, A = Left, B = Right), B = Locomotion);\n"
			"    }\n"
			"\n"
			"    StateMachine Locomotion\n"
			"    {\n"
			"        Entry = Idle;\n"
			"\n"
			"        #Region \"Standing\"\n"
			"        State Idle { Pose = SequencePlayer(Sequence = A_Als_Stand_Pose); }\n"
			"        #EndRegion\n"
			"\n"
			"        State Walk { Pose = SequencePlayer(Sequence = A_Als_Walk_Forward); }\n"
			"\n"
			"        Transitions = { Idle -> Walk : false; }\n"
			"    }\n"
			"}\n");
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(Path, Text, Options);
		ReportCompileErrors(*this, Result);
		if (!Result.Blueprint)
		{
			return false;
		}

		// Whether a comment box holds a node's position.
		const auto Holds = [](const UEdGraphNode_Comment& Comment, const UEdGraphNode& Node)
		{
			return Node.NodePosX >= Comment.NodePosX && Node.NodePosX < Comment.NodePosX + Comment.NodeWidth
				&& Node.NodePosY >= Comment.NodePosY && Node.NodePosY < Comment.NodePosY + Comment.NodeHeight;
		};

		TArray<UEdGraphNode_Comment*> Comments;
		FBlueprintEditorUtils::GetAllNodesOfClass(Result.Blueprint, Comments);
		UEdGraphNode_Comment* const* Stops = Comments.FindByPredicate([](const UEdGraphNode_Comment* Comment) { return Comment->NodeComment == TEXT("Stops"); });
		UEdGraphNode_Comment* const* Standing = Comments.FindByPredicate([](const UEdGraphNode_Comment* Comment) { return Comment->NodeComment == TEXT("Standing"); });
		for (const UEdGraphNode_Comment* Comment : Comments)
		{
			AddInfo(FString::Printf(TEXT("comment `%s` in %s at %d,%d size %dx%d"), *Comment->NodeComment, *Comment->GetGraph()->GetName(),
				Comment->NodePosX, Comment->NodePosY, Comment->NodeWidth, Comment->NodeHeight));
		}
		TestEqual(TEXT("one comment box per region"), Comments.Num(), 2);
		if (!TestTrue(TEXT("the graph's region is a comment box"), Stops != nullptr) || !TestTrue(TEXT("the machine's region is a comment box"), Standing != nullptr))
		{
			return false;
		}

		TArray<UAnimGraphNode_SequencePlayer*> Players;
		FBlueprintEditorUtils::GetAllNodesOfClass(Result.Blueprint, Players);
		int32 NumInStops = 0;
		for (const UAnimGraphNode_SequencePlayer* Player : Players)
		{
			if (Player->GetGraph() == (*Stops)->GetGraph())
			{
				const UAnimationAsset* Asset = Player->GetAnimationAsset();
				const bool bStop = Asset && Asset->GetName().Contains(TEXT("Stop"));
				TestEqual(FString::Printf(TEXT("the box holds %s only if it is a stop"), *Player->GetName()), Holds(**Stops, *Player), bStop);
				NumInStops += Holds(**Stops, *Player) ? 1 : 0;
			}
		}
		TestEqual(TEXT("both stops are in the box"), NumInStops, 2);

		TArray<UAnimStateNode*> States;
		FBlueprintEditorUtils::GetAllNodesOfClass(Result.Blueprint, States);
		for (const UAnimStateNode* State : States)
		{
			TestEqual(FString::Printf(TEXT("the machine's box holds %s only if it is Idle"), *State->GetStateName()),
				Holds(**Standing, *State), State->GetStateName() == TEXT("Idle"));
		}

		// The boxes are not part of what the text says: the Blueprint decompiles with no trace of them.
		const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Result.Blueprint);
		TestEqual(TEXT("nothing unsupported"), Decompiled.NumUnsupported, 0);
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverDecompileToAnimSetTest, "DreamAnim.Driver.DecompileToAnimSet",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	/**
	 * Decompiling into an animation set: the animations in pins and in a state function's call become slots,
	 * one per asset; the set gets the Blueprint's group with each slot filled; the text builds a Blueprint
	 * that reads them from the set, and decompiles back to the same text.
	 */
	bool FDabDriverDecompileToAnimSetTest::RunTest(const FString& Parameters)
	{
		const FString Path = MakeDriverScratchDir(TEXT("AnimSet")) / TEXT("AB_Move.dab");
		const FString Text = TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/AnimSetMove/AB_Move\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AlsLinkedAnimationInstance; Skeleton = SK_Als; }\n"
			"\n"
			"    Aliases = { Main = Self.GetParent(); }\n"
			"\n"
			"    Graph = { Output = Locomotion; }\n"
			"\n"
			"    StateMachine Locomotion\n"
			"    {\n"
			"        Entry = Walk;\n"
			"\n"
			"        State Walk { Pose = BlendSpacePlayer(BlendSpace = BS_Als_WalkRun_Forward, X = Main.StandingState.StrideBlendAmount); }\n"
			"\n"
			"        State Stop\n"
			"        {\n"
			"            OnEnter = Main.PlayTransitionAnimation(Sequence = A_Als_Stop_Left, PlayRate = 1.5);\n"
			"\n"
			"            Pose = SequencePlayer(Sequence = A_Als_Stop_Left);\n"
			"        }\n"
			"\n"
			"        Transitions = {\n"
			"            Walk -> Stop : Main.FeetState.FootPlantedAmount <= -0.5;\n"
			"        }\n"
			"    }\n"
			"}\n");
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Built = CompileDabText(Path, Text, Options);
		ReportCompileErrors(*this, Built);
		if (!Built.Blueprint)
		{
			return false;
		}

		FDabAnimSetMigration Migration;
		Migration.AnimSetPath = TEXT("/Game/DreamAnimTests/AnimSetMove/DAS_Move");
		Migration.bSave = false;
		const FDabDecompileResult Moved = DecompileDabBlueprint(*Built.Blueprint, FDabLowerOptions(), &Migration);
		for (const FDabDiagnostic& Diagnostic : Moved.Diagnostics)
		{
			AddError(FormatDabDiagnosticLine(Diagnostic));
		}
		if (!TestTrue(TEXT("decompiles into the set"), Moved.bSucceeded))
		{
			return false;
		}

		// One slot per asset: the stop animation is read twice.
		TArray<FString> Slots;
		for (const FDabLiftedAsset& Lifted : Moved.LiftedAssets)
		{
			Slots.Add(Lifted.Slot + TEXT(":") + Lifted.Type);
		}
		TestEqual(TEXT("one slot per asset"), FString::Join(Slots, TEXT(" ")), FString(TEXT("A_Als_Stop_Left:AnimSequence BS_Als_WalkRun_Forward:BlendSpace")));
		for (const TCHAR* Expected : {
			TEXT("DefaultAnimSet"),
			TEXT("DreamAnimTests/AnimSetMove/DAS_Move"),
			TEXT("Assets = {"),
			TEXT("AnimSequence A_Als_Stop_Left;"),
			TEXT("BlendSpace   BS_Als_WalkRun_Forward;"),
			TEXT("BlendSpace = Assets.BS_Als_WalkRun_Forward"),
			TEXT("Sequence = Assets.A_Als_Stop_Left"),
			TEXT("PlayTransitionAnimation(Sequence = Assets.A_Als_Stop_Left, PlayRate = 1.5)") })
		{
			TestTrue(FString::Printf(TEXT("the text has `%s`"), Expected), Moved.Text.Contains(Expected, ESearchCase::CaseSensitive));
		}
		TestFalse(TEXT("no asset is read directly any more"), Moved.Text.Contains(TEXT("= A_Als_Stop_Left"), ESearchCase::CaseSensitive));

		const UDreamAnimSet* Set = LoadObject<UDreamAnimSet>(nullptr, TEXT("/Game/DreamAnimTests/AnimSetMove/DAS_Move.DAS_Move"));
		if (!TestNotNull(TEXT("the set is made"), Set))
		{
			return false;
		}
		const UObject* Stop = Set->Find(TEXT("AB_Move"), TEXT("A_Als_Stop_Left"));
		const UObject* Walk = Set->Find(TEXT("AB_Move"), TEXT("BS_Als_WalkRun_Forward"));
		TestTrue(TEXT("the set holds the stop animation"), Stop && Stop->GetName() == TEXT("A_Als_Stop_Left"));
		TestTrue(TEXT("the set holds the blend space"), Walk && Walk->GetName() == TEXT("BS_Als_WalkRun_Forward"));
		TestTrue(TEXT("the set takes the Blueprint's skeleton"), Set->Skeleton && Set->Skeleton->GetName() == TEXT("SK_Als"));

		// The text builds a Blueprint that reads the set, and that Blueprint decompiles back to the same text.
		const FDabCompileResult Rebuilt = CompileDabText(Path, Moved.Text, Options);
		ReportCompileErrors(*this, Rebuilt);
		if (!Rebuilt.Blueprint)
		{
			return false;
		}
		const FDabDecompileResult Again = DecompileDabBlueprint(*Rebuilt.Blueprint);
		TestEqual(TEXT("nothing unsupported in the rebuilt Blueprint"), Again.NumUnsupported, 0);
		const FString Difference = DescribeDriverDifference(Moved.Text, Again.Text);
		TestTrue(FString::Printf(TEXT("the rebuilt Blueprint decompiles to the same text (%s)"), *Difference), Difference.IsEmpty());

		// Again into the set: nothing left to move, nothing in the set changes.
		const FDabDecompileResult Twice = DecompileDabBlueprint(*Rebuilt.Blueprint, FDabLowerOptions(), &Migration);
		TestEqual(TEXT("nothing left to move"), Twice.LiftedAssets.Num(), 0);
		TestTrue(TEXT("and the text stays"), DescribeDriverDifference(Moved.Text, Twice.Text).IsEmpty());
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverDecompilePathTest, "DreamAnim.Driver.DefaultDecompilePath",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverDecompilePathTest::RunTest(const FString& Parameters)
	{
		FString DAnim = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("DAnim")));
		FPaths::NormalizeFilename(DAnim);

		const UAnimBlueprint* Plugin = LoadObject<UAnimBlueprint>(nullptr, TEXT("/ALS/ALS/Character/AB_Als.AB_Als"));
		if (TestNotNull(TEXT("AB_Als loads"), Plugin))
		{
			TestEqualSensitive(TEXT("a plugin asset keeps its mount"), GetDefaultDecompilePath(*Plugin), DAnim / TEXT("ALS/ALS/Character/AB_Als.dab"));
		}
		const UAnimBlueprint* Interface = LoadObject<UAnimBlueprint>(nullptr, TEXT("/ALS/ALS/Character/ALI_Overlay.ALI_Overlay"));
		if (TestNotNull(TEXT("ALI_Overlay loads"), Interface))
		{
			TestEqualSensitive(TEXT("an interface is a .dai"), GetDefaultDecompilePath(*Interface), DAnim / TEXT("ALS/ALS/Character/ALI_Overlay.dai"));
		}

		// Only the package name and the Blueprint type matter, so a transient object stands in for a /Game asset.
		UPackage* Package = CreatePackage(TEXT("/Game/DreamAnimTests/Driver/AB_PathProbe"));
		UAnimBlueprint* Probe = NewObject<UAnimBlueprint>(Package, TEXT("AB_PathProbe"), RF_Transient);
		TestEqualSensitive(TEXT("a /Game asset drops the mount"), GetDefaultDecompilePath(*Probe), DAnim / TEXT("DreamAnimTests/Driver/AB_PathProbe.dab"));
		Probe->MarkAsGarbage();
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverDiagnosticFormsTest, "DreamAnim.Driver.DiagnosticForms",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverDiagnosticFormsTest::RunTest(const FString& Parameters)
	{
		FDabDiagnostic Error;
		Error.Code = TEXT("DAB2001");
		Error.Severity = EDabSeverity::Error;
		Error.Message = TEXT("Expected \";\" after \"a\\b\"\nnext");
		Error.Hint = TEXT("Add\tit.");
		Error.FilePath = TEXT("C:/Project/DAnim/AB_Test.dab");
		Error.Span.Offset = 40;
		Error.Span.Length = 4;
		Error.Span.Line = 3;
		Error.Span.Column = 7;

		FDabDiagnostic Warning;
		Warning.Code = TEXT("DAB6004");
		Warning.Severity = EDabSeverity::Warning;
		Warning.Message = TEXT("The named pose `Idle` is never used.");
		Warning.FilePath = TEXT("C:/Project/DAnim/Lib.dat");
		Warning.Span.Line = 12;
		Warning.Span.Column = 1;

		FDabDiagnostic Info;
		Info.Code = TEXT("DAB0000");
		Info.Severity = EDabSeverity::Info;
		Info.Message = TEXT("bell");
		Info.Message.AppendChar(TCHAR(7));
		Info.FilePath = TEXT("D:/X/中文.dab");

		TestEqualSensitive(TEXT("error line"), FormatDabDiagnosticLine(Error), FString(TEXT("C:/Project/DAnim/AB_Test.dab(3,7): error DAB2001: Expected \";\" after \"a\\b\"\nnext")));
		TestEqualSensitive(TEXT("warning line"), FormatDabDiagnosticLine(Warning), FString(TEXT("C:/Project/DAnim/Lib.dat(12,1): warning DAB6004: The named pose `Idle` is never used.")));
		TestEqualSensitive(TEXT("info line"), FormatDabDiagnosticLine(Info), TEXT("D:/X/中文.dab(1,1): info DAB0000: ") + Info.Message);

		TestEqualSensitive(TEXT("no diagnostics"), DabDiagnosticsToJson({}), FString(TEXT("[]")));
		TestEqualSensitive(TEXT("json"), DabDiagnosticsToJson({ Error, Warning, Info }), FString(TEXT(R"JSON([{"file":"C:/Project/DAnim/AB_Test.dab", "line":3, "column":7, "length":4, "severity":"error", "code":"DAB2001", "message":"Expected \";\" after \"a\\b\"\nnext", "hint":"Add\tit."}, {"file":"C:/Project/DAnim/Lib.dat", "line":12, "column":1, "length":0, "severity":"warning", "code":"DAB6004", "message":"The named pose `Idle` is never used.", "hint":""}, {"file":"D:/X/中文.dab", "line":1, "column":1, "length":0, "severity":"info", "code":"DAB0000", "message":"bell\u0007", "hint":""}])JSON")));
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverCommandletCheckTest, "DreamAnim.Driver.CommandletCheck",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverCommandletCheckTest::RunTest(const FString& Parameters)
	{
		UClass* CommandletClass = FindObject<UClass>(nullptr, TEXT("/Script/DreamAnimCompiler.DreamAnimCommandlet"));
		if (!TestNotNull(TEXT("the commandlet class"), CommandletClass))
		{
			return false;
		}

		const FString Dir = MakeDriverScratchDir(TEXT("Commandlet"));
		const FString Good = Dir / TEXT("AB_Good.dab");
		const FString Bad = Dir / TEXT("AB_Bad.dab");
		const FString GoodJson = Dir / TEXT("good.json");
		const FString BadJson = Dir / TEXT("bad.json");
		if (!WriteDriverFile(*this, Good, TEXT(
				"AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Good\", Root=\"Game\")\n"
				"{\n"
				"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
				"    Graph = { Output = LocalRefPose(); }\n"
				"}\n"))
			|| !WriteDriverFile(*this, Bad, TEXT(
				"AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Bad\", Root=\"Game\")\n"
				"{\n"
				"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
				"    Graph = { Output = LocalRefPose() }\n"
				"}\n")))
		{
			return false;
		}

		// The same object the engine makes for -run=DreamAnim, run on this process's command line.
		const auto RunCommandlet = [CommandletClass](const FString& File, const FString& Json)
		{
			UCommandlet* Commandlet = NewObject<UCommandlet>(GetTransientPackage(), CommandletClass);
			return Commandlet->Main(FString::Printf(TEXT("-Check -Root=\"%s\" -File=\"%s\" -Json=\"%s\""),
				*FPaths::GetPath(File), *File, *Json));
		};

		TestEqual(TEXT("a clean file is no error"), RunCommandlet(Good, GoodJson), 0);
		FString Json;
		TestTrue(TEXT("the clean file's json is written"), FFileHelper::LoadFileToString(Json, *GoodJson));
		TestTrue(TEXT("and holds no error"), Json.StartsWith(TEXT("[")) && !Json.Contains(TEXT("\"severity\":\"error\"")));

		AddExpectedErrorPlain(TEXT("error DAB"), EAutomationExpectedErrorFlags::Contains, 0);
		const int32 Errors = RunCommandlet(Bad, BadJson);
		TestTrue(TEXT("a broken file fails"), Errors >= 1);
		TestTrue(TEXT("the broken file's json is written"), FFileHelper::LoadFileToString(Json, *BadJson));
		TestTrue(TEXT("it names the syntax error"), Json.Contains(TEXT("\"code\":\"DAB2001\"")));
		TestTrue(TEXT("it names the file"), Json.Contains(FString::Printf(TEXT("\"file\":\"%s\""), *Bad)));
		const FString ErrorField = TEXT("\"severity\":\"error\"");
		const int32 ErrorsInJson = (Json.Len() - Json.Replace(*ErrorField, TEXT(""), ESearchCase::CaseSensitive).Len()) / ErrorField.Len();
		TestEqual(TEXT("the exit code is the number of errors"), Errors, ErrorsInJson);
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDriverLayoutTest, "DreamAnim.Driver.Layout",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabDriverLayoutTest::RunTest(const FString& Parameters)
	{
		const FString Dir = MakeDriverScratchDir(TEXT("Layout"));
		const FString Path = Dir / TEXT("AB_Layout.dab");
		const FString Text = TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/Driver/AB_Layout\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
			"    Graph = { Output = Locomotion; }\n"
			"    StateMachine Locomotion\n"
			"    {\n"
			"        Entry = Idle;\n"
			"        State Idle { Pose = SequencePlayer(Sequence = A_Als_Idle); }\n"
			"    }\n"
			"}\n");
		FDabCompileOptions Options;
		Options.bSave = false;
		const auto Generate = [this, &Path, &Text, &Options]() -> UAnimBlueprint*
		{
			const FDabCompileResult Result = CompileDabText(Path, Text, Options);
			ReportCompileErrors(*this, Result);
			return Result.Blueprint;
		};
		const auto FindIdle = [](UAnimBlueprint& Blueprint) -> UAnimStateNode*
		{
			TArray<UAnimStateNode*> States;
			FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, States);
			return States.IsEmpty() ? nullptr : States[0];
		};

		UAnimBlueprint* Blueprint = Generate();
		UAnimStateNode* Idle = Blueprint ? FindIdle(*Blueprint) : nullptr;
		if (!TestNotNull(TEXT("the state"), Idle))
		{
			return false;
		}
		const FIntPoint Automatic(Idle->NodePosX, Idle->NodePosY);
		const FGuid IdleGuid = Idle->NodeGuid;
		Idle->NodePosX = 1234;
		Idle->NodePosY = -56;

		const FString Layout = MakeDabLayout(*Blueprint);
		TestTrue(TEXT("the layout says what it is"), Layout.StartsWith(TEXT("# Where the editor put the nodes of AB_Layout")));
		const TMap<FGuid, FIntPoint> Positions = ParseDabLayout(Layout);
		TestTrue(TEXT("the layout has the moved state"), Positions.FindRef(IdleGuid) == FIntPoint(1234, -56));
		TestTrue(TEXT("and the other nodes"), Positions.Num() > 3);
		TestTrue(TEXT("lines that are not positions are skipped"), ParseDabLayout(TEXT("# a comment\nnot a line\n")).IsEmpty());

		// A rebuild puts the node where the layout file says; without the file it goes back to the automatic place.
		FFileHelper::SaveStringToFile(Layout, *GetDabLayoutPath(Path));
		Blueprint = Generate();
		Idle = Blueprint ? FindIdle(*Blueprint) : nullptr;
		if (TestNotNull(TEXT("the state after a rebuild"), Idle))
		{
			TestEqual(TEXT("the same GUID after a rebuild"), Idle->NodeGuid, IdleGuid);
			TestEqual(TEXT("where the layout put it"), FIntPoint(Idle->NodePosX, Idle->NodePosY), FIntPoint(1234, -56));
		}
		IFileManager::Get().Delete(*GetDabLayoutPath(Path));
		Blueprint = Generate();
		Idle = Blueprint ? FindIdle(*Blueprint) : nullptr;
		if (TestNotNull(TEXT("the state without a layout"), Idle))
		{
			TestEqual(TEXT("the automatic place"), FIntPoint(Idle->NodePosX, Idle->NodePosY), Automatic);
		}
		return true;
	}
}

#endif

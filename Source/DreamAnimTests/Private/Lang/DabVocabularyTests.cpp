// Copyright DreamMoon. All Rights Reserved.
//
// The word table (Lang/DabVocabulary.h) against the language reference: every word the references list as
// meaning something where it is written is in the table, every word of the table is in its reference, and
// the grammar version Grammar.md states and Changelog.md describes is the one the code has.

#include "Interfaces/IPluginManager.h"
#include "Lang/DabVocabulary.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::VocabularyPrivate
{
	using namespace UE::DreamAnim::Lang;

	FString ReadDoc(FAutomationTestBase& Test, const TCHAR* Name)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
		FString Text;
		if (!Plugin.IsValid() || !FFileHelper::LoadFileToString(Text, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Docs"), Name)))
		{
			Test.AddError(FString::Printf(TEXT("Cannot read Docs/%s"), Name));
		}
		return Text.Replace(TEXT("\r\n"), TEXT("\n"));
	}

	/** The bullet (a line starting `- `, with the indented lines after it) that contains Marker; empty when there is none. */
	FString FindBullet(const FString& Doc, const TCHAR* Marker)
	{
		const int32 At = Doc.Find(Marker, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE)
		{
			return FString();
		}
		int32 Start = Doc.Find(TEXT("\n- "), ESearchCase::CaseSensitive, ESearchDir::FromEnd, At);
		Start = Start == INDEX_NONE ? 0 : Start + 1;
		int32 End = Doc.Find(TEXT("\n- "), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		const int32 Blank = Doc.Find(TEXT("\n\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		End = End == INDEX_NONE ? Blank : Blank == INDEX_NONE ? End : FMath::Min(End, Blank);
		return Doc.Mid(Start, (End == INDEX_NONE ? Doc.Len() : End) - Start);
	}

	/** The words written in backticks in Text. */
	TArray<FString> GetBacktickedWords(const FString& Text)
	{
		TArray<FString> Words;
		int32 Open = Text.Find(TEXT("`"));
		while (Open != INDEX_NONE)
		{
			const int32 Close = Text.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
			if (Close == INDEX_NONE)
			{
				break;
			}
			Words.Add(Text.Mid(Open + 1, Close - Open - 1));
			Open = Text.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Close + 1);
		}
		return Words;
	}

	/** Whether Doc writes Word in code: a backtick, the word, and then no more of an identifier. */
	bool DocWritesWord(const FString& Doc, const FString& Word)
	{
		const FString Needle = TEXT("`") + Word;
		for (int32 At = Doc.Find(Needle, ESearchCase::CaseSensitive); At != INDEX_NONE;
			At = Doc.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			const int32 After = At + Needle.Len();
			if (After >= Doc.Len() || !(FChar::IsAlnum(Doc[After]) || Doc[After] == TEXT('_')))
			{
				return true;
			}
		}
		return false;
	}

	bool TableHas(EDabDocumentKind Kind, const FString& Text)
	{
		for (const FDabWord& Word : GetDabWords(Kind))
		{
			if (Text.Equals(Word.Text, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabVocabularyDocsTest, "DreamAnim.Lang.Vocabulary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDabVocabularyDocsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::VocabularyPrivate;
	const FString Language = ReadDoc(*this, TEXT("Language.md"));
	const FString ControlRig = ReadDoc(*this, TEXT("ControlRig.md"));
	const FString Grammar = ReadDoc(*this, TEXT("Grammar.md"));
	const FString Changelog = ReadDoc(*this, TEXT("Changelog.md"));

	// The version the docs state and describe.
	TestTrue(TEXT("Grammar.md states the grammar version"), Grammar.Contains(FString::Printf(TEXT("\nGrammar version: %d\n"), DabGrammarVersion)));
	TestTrue(TEXT("Changelog.md describes it"), Changelog.Contains(FString::Printf(TEXT("\n## %d ("), DabGrammarVersion)));
	TestFalse(TEXT("Changelog.md has no version past it"), Changelog.Contains(FString::Printf(TEXT("\n## %d ("), DabGrammarVersion + 1)));

	// What the references list as meaning something where it is written is in the table.
	struct FListed
	{
		const FString* Doc;
		const TCHAR* Marker;
		EDabDocumentKind Kind;
	};
	const FListed Lists[] =
	{
		{ &Language, TEXT("These words mean something where they are written"), EDabDocumentKind::AnimBlueprint },
		{ &ControlRig, TEXT("At the start of a code statement these words mean something"), EDabDocumentKind::ControlRig },
		{ &ControlRig, TEXT("These names mean something where they are written as values"), EDabDocumentKind::ControlRig },
		{ &ControlRig, TEXT("start a block. In `Hierarchy`"), EDabDocumentKind::ControlRig },
	};
	for (const FListed& List : Lists)
	{
		const FString Bullet = FindBullet(*List.Doc, List.Marker);
		if (!TestFalse(FString::Printf(TEXT("the reference has the list '%s'"), List.Marker), Bullet.IsEmpty()))
		{
			continue;
		}
		for (const FString& Word : GetBacktickedWords(Bullet))
		{
			// `` `true` `` in a sentence about backticks is an example, not a word.
			if (Word.StartsWith(TEXT("`")) || Word.Contains(TEXT(" ")))
			{
				continue;
			}
			TestTrue(FString::Printf(TEXT("`%s` (%s) is in the table"), *Word, LexToString(List.Kind)), TableHas(List.Kind, Word));
		}
	}

	// Every word of the table is written in its reference.
	for (const FDabDocumentWords& Document : GetDabDocumentWords())
	{
		const FString& Doc = Document.Kind == EDabDocumentKind::ControlRig ? ControlRig : Language;
		for (const FDabWord& Word : Document.Words)
		{
			TestTrue(FString::Printf(TEXT("`%s` (%s) is in its reference"), Word.Text, LexToString(Document.Kind)),
				DocWritesWord(Doc, Word.Text) || DocWritesWord(Grammar, Word.Text));
		}
	}

	// The parser's block words are the table's.
	TestTrue(TEXT("StateMachine starts a block in an Animation Blueprint"), IsDabBlockWord(EDabDocumentKind::AnimBlueprint, TEXT("StateMachine")));
	TestTrue(TEXT("Event starts a block in a Control Rig"), IsDabBlockWord(EDabDocumentKind::ControlRig, TEXT("Event")));
	TestFalse(TEXT("Event does not in an Animation Blueprint"), IsDabBlockWord(EDabDocumentKind::AnimBlueprint, TEXT("Event")));
	return true;
}

#endif

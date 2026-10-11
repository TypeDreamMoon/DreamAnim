// Copyright DreamMoon. All Rights Reserved.

#include "Driver/DabDocumentHandler.h"

#include "DabDriverPrivate.h"

namespace UE::DreamAnim::Compiler
{
	namespace DocumentHandlerPrivate
	{
		TMap<Lang::EDabDocumentKind, TSharedRef<IDabDocumentHandler>>& GetHandlers()
		{
			static TMap<Lang::EDabDocumentKind, TSharedRef<IDabDocumentHandler>> Handlers;
			return Handlers;
		}
	}

	void RegisterDabDocumentHandler(Lang::EDabDocumentKind Kind, TSharedRef<IDabDocumentHandler> Handler)
	{
		check(IsInGameThread());
		DocumentHandlerPrivate::GetHandlers().Add(Kind, MoveTemp(Handler));
	}

	void UnregisterDabDocumentHandler(Lang::EDabDocumentKind Kind)
	{
		check(IsInGameThread());
		DocumentHandlerPrivate::GetHandlers().Remove(Kind);
	}

	IDabDocumentHandler* FindDabDocumentHandler(Lang::EDabDocumentKind Kind)
	{
		TSharedRef<IDabDocumentHandler>* Found = DocumentHandlerPrivate::GetHandlers().Find(Kind);
		return Found ? &Found->Get() : nullptr;
	}

	IDabDocumentHandler* FindDabDecompileHandler(const UObject& Asset)
	{
		for (TPair<Lang::EDabDocumentKind, TSharedRef<IDabDocumentHandler>>& Entry : DocumentHandlerPrivate::GetHandlers())
		{
			if (Entry.Value->CanDecompile(Asset))
			{
				return &Entry.Value.Get();
			}
		}
		return nullptr;
	}

	TArray<IDabDocumentHandler*> GetDabDocumentHandlers()
	{
		TArray<IDabDocumentHandler*> Out;
		for (TPair<Lang::EDabDocumentKind, TSharedRef<IDabDocumentHandler>>& Entry : DocumentHandlerPrivate::GetHandlers())
		{
			Out.Add(&Entry.Value.Get());
		}
		return Out;
	}

	void AddDabCompileDiagnostics(FDabCompileResult& Result, TConstArrayView<Lang::FDabDiagnostic> Diagnostics)
	{
		for (const Lang::FDabDiagnostic& Diagnostic : Diagnostics)
		{
			Lang::FDabDiagnostic& Added = Result.Diagnostics.Add_GetRef(Diagnostic);
			Added.FilePath = Added.FilePath.IsEmpty() ? Result.FilePath : DriverPrivate::NormalizeDabPath(Added.FilePath);
			if (Added.Severity == Lang::EDabSeverity::Error)
			{
				++Result.NumErrors;
			}
		}
	}
}

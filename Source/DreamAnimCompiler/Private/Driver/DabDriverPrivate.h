// Copyright DreamMoon. All Rights Reserved.
//
// What the driver and the command line share beyond the public driver interface: path spelling, the
// document-header-to-asset rule and reading one source as far as the index needs.

#pragma once

#include "CoreMinimal.h"
#include "Driver/DabDriver.h"

class UAnimBlueprint;

namespace UE::DreamAnim::Compiler::DriverPrivate
{
	/**
	 * Absolute (a relative path is taken from the project directory, not the engine's working directory),
	 * `/`-separated, `..` collapsed, no trailing slash. The include loader resolves to the same spelling,
	 * so paths from either compare equal.
	 */
	FString NormalizeDabPath(const FString& Path);

	/**
	 * The object path of the asset a document header names (Docs/Mapping.md, Document): `/<mount>/A/B.B`.
	 * Empty without a Name, or with a Root that is not Game, Engine or Plugin.X.
	 */
	FString MakeDabDocumentAssetPath(const FString& Name, const FString& Root);

	/** An object path from an object path, a package path or an export text path (`AnimBlueprint'/Game/A.A'`). */
	FString ToDabObjectPath(const FString& AssetPath);

	/**
	 * Where a package's text goes below a `DAnim/` folder, without extension: the path after `/Game/`, the
	 * whole path (mount included) for any other mount.
	 */
	FString GetDabDecompileRelativePath(const FString& PackageName);

	/** `.dai` for an Animation Layer Interface, `.dab` for anything else. */
	const TCHAR* GetDabDecompileExtension(const UAnimBlueprint& Blueprint);

	/**
	 * Where `#include IncludePath` written in FromFile points, by the include loader's rule (the including
	 * file's folder, then each root). True when the file exists; otherwise OutPath is where it would be next
	 * to the including file, so a library that is missing still names the documents waiting for it.
	 */
	bool ResolveDabIncludePath(const FString& FromFile, const FString& IncludePath, const TArray<FString>& Roots, FString& OutPath);

	/** Reads one source as far as the index needs (kind, asset, direct includes). False when it cannot be read. */
	bool ReadDabSourceEntry(const FString& FilePath, const TArray<FString>& Roots, FDabSourceEntry& OutEntry);
}

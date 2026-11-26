<#
.SYNOPSIS
    Sets up the AstroDuel2 Overlay mod, and runs the game.

.DESCRIPTION
    .\CopyAndRun.ps1 "<Astro Duel 2 Directory>" [-Configuration <RelWithDebInfo|Release>] [-NoRun]
#>

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$TargetPath, # required: the Astro Duel 2 directory

    [Parameter(Position = 1)]
    [ValidateSet('RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo', # build configuration of the replacement DLL

    [switch]$NoRun
)

# Options:
$SteamDLL       = "steam_api64.dll"
$SteamDLLRename = "steam_api64Hooked.dll"
$SteamPDB       = "steam_api64.pdb"
$CopySource     = "build\$Configuration\$SteamDLL"
$CopySourcePDB  = "build\$Configuration\$SteamPDB"
$Executable     = "AstroDuel2.exe"
$SDLDLL         = "SDL3.dll"
$SteamDLLHash   = "E082BF5C9F881C822B1540A76B74F9D15E18019A73EBD206A559595BADCB7F65"
$ExecutableHash = "E6C30ABFD242B3F74907643B64E65ECCA2BC4E3F49D68DA745926E55C58EBAC7"
$SDLDLLHash     = "55F17C87B32E07EF46365D604131C063014E4E47CE58DA9EC061720DDFB90D82"

function Test-FileHash {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$ExpectedHash
    )

    $displayName = Split-Path -Leaf $Path
    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedHash) {
        Write-Error "'$displayName' hash mismatch. Expected: $ExpectedHash != Found: $actualHash"
        exit 1
    }
}

# Validate path
if (-not (Test-Path -LiteralPath $TargetPath -PathType Container)) {
    Write-Error "Not a valid directory: $TargetPath"
    exit 1
}
$TargetPath = (Resolve-Path -LiteralPath $TargetPath).Path
Write-Host "Working in: $TargetPath"

# Verify the executables's hash
$exePath = Join-Path $TargetPath $Executable
if (-not (Test-Path -LiteralPath $exePath)) {
    Write-Error "Executable not found: $exePath"
    exit 1
}
Test-FileHash $exePath $ExecutableHash

# Verify the SDL DLL's hash
$sdlPath = Join-Path $TargetPath $SDLDLL
if (-not (Test-Path -LiteralPath $sdlPath)) {
    Write-Error "SDL DLL not found: $sdlPath"
    exit 1
}
Test-FileHash $sdlPath $SDLDLLHash

# Rename SteamDLL
$renameSrc = Join-Path $TargetPath $SteamDLL
$renameDst = Join-Path $TargetPath $SteamDLLRename
if (-not (Test-Path -LiteralPath $renameDst)) {
    if (-not (Test-Path -LiteralPath $renameSrc)) {
        Write-Error "File to rename not found: $renameSrc"
        exit 1
    }

    # Verify the source file's hash
    Test-FileHash $renameSrc $SteamDLLHash

    Rename-Item -LiteralPath $renameSrc -NewName $SteamDLLRename
    Write-Host "Renamed '$SteamDLL' -> '$SteamDLLRename'."
} else {
    Write-Host "'$SteamDLLRename' already exists - rename skipped."

    # Verify the destination file's hash
    Test-FileHash $renameDst $SteamDLLHash
}

# Copy replacement DLL from build
if (-not (Test-Path -LiteralPath $CopySource -PathType Leaf)) {
    Write-Error "Copy source not found: $CopySource"
    exit 1
}
$copyDest = Join-Path $TargetPath $SteamDLL
Copy-Item -LiteralPath $CopySource -Destination $copyDest -Force
Write-Host "Copied '$CopySource' -> '$copyDest'."

# Copy the PDB
$pdbDest = Join-Path $TargetPath $SteamPDB
if (Test-Path -LiteralPath $CopySourcePDB -PathType Leaf) {
    Copy-Item -LiteralPath $CopySourcePDB -Destination $pdbDest -Force
    Write-Host "Copied '$CopySourcePDB' -> '$pdbDest'."
} elseif (Test-Path -LiteralPath $pdbDest -PathType Leaf) {
    Remove-Item -LiteralPath $pdbDest
    Write-Host "PDB not found - deleted existing: $pdbDest"
} else {
    Write-Host "PDB not found - skipped"
}

if (-not $NoRun) {
    # Run game
    Start-Process -FilePath "$exePath"
}

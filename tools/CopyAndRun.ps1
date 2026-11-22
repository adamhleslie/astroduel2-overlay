<#
.SYNOPSIS
    Sets up the AstroDuel2 Overlay mod, and runs the game.

.DESCRIPTION
    .\script.ps1 "<Astro Duel 2 Directory>" [-Configuration <RelWithDebInfo|Release>] [-NoRun]
#>

# TODO: Add a hash test of the .exe and .dll to ensure no updates have occurred
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
$SteamDLLRename = "steam_api64_real.dll"
$SteamPDB       = "steam_api64.pdb"
$CopySource     = "build\$Configuration\$SteamDLL"
$CopySourcePDB  = "build\$Configuration\$SteamPDB"
$Executable     = "AstroDuel2.exe"

# Validate path
if (-not (Test-Path -LiteralPath $TargetPath -PathType Container)) {
    Write-Error "Not a valid directory: $TargetPath"
    exit 1
}
$TargetPath = (Resolve-Path -LiteralPath $TargetPath).Path
Write-Host "Working in: $TargetPath"

# Rename SteamDLL
$renameSrc = Join-Path $TargetPath $SteamDLL
$renameDst = Join-Path $TargetPath $SteamDLLRename

if (-not (Test-Path -LiteralPath $renameSrc)) {
    Write-Error "File to rename not found: $renameSrc"
    exit 1
}

if (Test-Path -LiteralPath $renameDst) {
    Write-Host "'$SteamDLLRename' already exists - rename skipped."
} else {
    Rename-Item -LiteralPath $renameSrc -NewName $SteamDLLRename
    Write-Host "Renamed '$SteamDLL' -> '$SteamDLLRename'."
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
    $exePath = Join-Path $TargetPath $Executable
    if (-not (Test-Path -LiteralPath $exePath)) {
        Write-Error "Executable not found: $exePath"
        exit 1
    }

    Start-Process -FilePath "$exePath"
}

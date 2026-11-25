<#
.SYNOPSIS
    Installs the AstroDuel2 Overlay mod.

.DESCRIPTION
    .\Install.ps1 "<Astro Duel 2 Directory>"
#>

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$TargetPath # required: the Astro Duel 2 directory
)

# Options:
$SteamDLL       = "steam_api64.dll"
$SteamDLLRename = "steam_api64_real.dll"
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
        pause
        exit 1
    }
}

# Validate path
if (-not (Test-Path -LiteralPath $TargetPath -PathType Container)) {
    Write-Error "Not a valid directory: $TargetPath"
    pause
    exit 1
}
$TargetPath = (Resolve-Path -LiteralPath $TargetPath).Path
Write-Host "Working in: $TargetPath"

# Verify the executables's hash
$exePath = Join-Path $TargetPath $Executable
if (-not (Test-Path -LiteralPath $exePath)) {
    Write-Error "Executable not found: $exePath"
    pause
    exit 1
}
Test-FileHash $exePath $ExecutableHash

# Verify the SDL DLL's hash
$sdlPath = Join-Path $TargetPath $SDLDLL
if (-not (Test-Path -LiteralPath $sdlPath)) {
    Write-Error "SDL DLL not found: $sdlPath"
    pause
    exit 1
}
Test-FileHash $sdlPath $SDLDLLHash

# Rename SteamDLL
$renameSrc = Join-Path $TargetPath $SteamDLL
$renameDst = Join-Path $TargetPath $SteamDLLRename
if (-not (Test-Path -LiteralPath $renameDst)) {
    if (-not (Test-Path -LiteralPath $renameSrc)) {
        Write-Error "File to rename not found: $renameSrc"
        pause
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

# Copy replacement DLL from working directory
$copySource = $SteamDLL
if (-not (Test-Path -LiteralPath $copySource -PathType Leaf)) {
    Write-Error "Copy source not found: $copySource"
    pause
    exit 1
}
$copyDest = Join-Path $TargetPath $SteamDLL
Copy-Item -LiteralPath $copySource -Destination $copyDest -Force
Write-Host "Copied '$copySource' -> '$copyDest'."
Write-Host "Install complete."
pause

## ${project_name_versioned}
### Installation
#### Installer
If you can run PowerShell scripts:
1. Download and unzip.
2. Right-click the `Install.ps1` PowerShell script and select "Run with PowerShell".
3. Copy the full path to your Astro Duel 2 install directory into the terminal.
4. Press enter.

Note: To enable running PowerShell scripts, open PowerShell and run the command `Set-ExecutionPolicy Bypass -Scope CurrentUser`.
Note: The installer script will fail if a compatible version of Astro Duel 2 is not found.

#### Manual
1. Download and unzip.
2. Navigate to your Astro Duel 2 install directory.
3. Rename the existing `steam_api64.dll` file in the Astro Duel 2 directory to `steam_api64Hooked.dll`.
4. Copy the mod's `steam_api64.dll` file into the Astro Duel 2 directory.

### Checksum

| File            | SHA256                                                       |
| --------------- | ------------------------------------------------------------ |
| steam_api64.dll | ${steam_api64.dll_sha256} |
| Install.ps1   | ${Install.ps1_sha256}|


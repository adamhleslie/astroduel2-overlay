// Import Address Table (IAT) hooks used for runtime logging
#include "hooks_iat.h"

#include <fileapi.h>
#include <shlobj_core.h>

#include "ad2.h"
#include "ad2_addresses.h"
#include "mod.h"

namespace
{
	constexpr std::string_view category = "iat-hook";
	constexpr std::string_view gameCategory = "game";

	// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
	decltype(&CreateFileW) createFileWOrig = nullptr;

	// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesw
	decltype(&GetFileAttributesW) getFileAttributesWOrig = nullptr;

	// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath
	decltype(&SHGetKnownFolderPath) shGetKnownFolderOrig = nullptr;

	// TODO: Make more performant
	std::string LPCWSTRToString(LPCWSTR lpStr)
	{
		int strLength = WideCharToMultiByte(CP_UTF8, 0, lpStr, -1, nullptr, 0, nullptr, nullptr);
		std::string str(strLength, 0);
		WideCharToMultiByte(CP_UTF8, 0, lpStr, -1, &str[0], strLength, nullptr, nullptr);
		return str;
	}

	HANDLE WINAPI CreateFileWHook(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
		DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		HANDLE result = createFileWOrig(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		mod::Log(gameCategory, "CreateFileW {} (result={}, error={})", LPCWSTRToString(lpFileName), result, GetLastError());
		return result;
	}

	DWORD WINAPI GetFileAttributesWHook(LPCWSTR lpFileName)
	{
		DWORD result = getFileAttributesWOrig(lpFileName);
		mod::Log(gameCategory, "GetFileAttributesW {} (result={:#x}{}, error={})", LPCWSTRToString(lpFileName), result, (result == INVALID_FILE_ATTRIBUTES ? " NOT-FOUND" : ""), GetLastError());
		return result;
	}

	HRESULT WINAPI SHGetKnownFolderPathHook(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, PWSTR* ppszPath)
	{
		// Convert rfid to string
		constexpr int guidCharMax = 40;
		OLECHAR guidStringW[guidCharMax] = {};
		StringFromGUID2(rfid, guidStringW, guidCharMax);
		char guidString[guidCharMax] = {};
		WideCharToMultiByte(CP_ACP, 0, guidStringW, -1, guidString, guidCharMax, nullptr, nullptr);

		HRESULT result = shGetKnownFolderOrig(rfid, dwFlags, hToken, ppszPath);
		if (result == S_OK && ppszPath && *ppszPath)
		{
			mod::Log(gameCategory, "SHGetKnownFolderPath {} -> {}", guidString, LPCWSTRToString(*ppszPath));
		}
		else
		{
			mod::Log(gameCategory, "SHGetKnownFolderPath {} FAILED hr={:#010X}", guidString, static_cast<unsigned long>(result));
		}

		return result;
	}
}

void hooks::iat::InstallHooks()
{
	HookIatForKernel(NAME_OF(CreateFileW), CreateFileWHook, createFileWOrig);
	HookIatForKernel(NAME_OF(GetFileAttributesW), GetFileAttributesWHook, getFileAttributesWOrig);
	HookIatForModule("shell32.dll", NAME_OF(SHGetKnownFolderPath), SHGetKnownFolderPathHook, shGetKnownFolderOrig);
	mod::Log(category, " Original Addresses - CreateFileW={} GetFileAttributesW={} SHGetKnownFolderPath={}",
		reinterpret_cast<void*>(createFileWOrig), reinterpret_cast<void*>(getFileAttributesWOrig), reinterpret_cast<void*>(shGetKnownFolderOrig));
}

bool hooks::iat::HookIat(uintptr_t address, uintptr_t hookAddress, const std::string& name)
{
	if (!address)
	{
		mod::Log(category, " {} - address is null", name);
		return false;
	}

	// TODO: Investigate smarter IAT reading
	// https://f3real.github.io/iat_hooking.html
	// https://medium.com/@s12deff/import-address-table-hooking-68d519a1da43
	// https://skr1x.github.io/portable-executable-format/ & https://skr1x.github.io/reflective-loading-portable-executable-memory/
	// Scan the whole IAT region
	for (ad2::MemoryOffset slotOffset = ad2::addresses::iatRegionStart; slotOffset < ad2::addresses::iatRegionEnd; slotOffset += 8)
	{
		uintptr_t* slotAddress = slotOffset.ToAddress<uintptr_t>();
		if (*slotAddress == address)
		{
			DWORD newProtect = PAGE_READWRITE;
			DWORD oldProtect = 0;
			if (!VirtualProtect(slotAddress, 8, newProtect, &oldProtect))
			{
				mod::Log(category, " {} - VirtualProtect FAILED {}", name, GetLastError());
				return false;
			}
			*slotAddress = hookAddress;
			VirtualProtect(slotAddress, 8, oldProtect, &newProtect);
			mod::Log(category, " {} - PATCHED slot @base0x+{} (orig={})", name, slotOffset, reinterpret_cast<void*>(address));
			return true;
		}
	}
	mod::Log(category, " {} - slot NOT FOUND in IAT {}..{}", name, ad2::addresses::iatRegionStart, ad2::addresses::iatRegionEnd);
	return false;
}
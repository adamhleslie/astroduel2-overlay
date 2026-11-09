// Import Address Table (IAT) hooks used for runtime logging
#include "hooks_iat.h"

#include <cstdio>

#include "mod.h"
#include "shlobj_core.h"

namespace
{
	constexpr std::string_view category = "iat-hook";
	constexpr std::string_view gameCategory = "game";

	constexpr uintptr_t iatRegionStart = 0x47e000;
	constexpr uintptr_t iatRegionEnd = 0x495a00;

	// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
	typedef HANDLE (WINAPI *FnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
	FnCreateFileW createFileWOrig = nullptr;

	// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesw
	typedef DWORD (WINAPI *FnGetFileAttributesW)(LPCWSTR);
	FnGetFileAttributesW getFileAttributesWOrig = nullptr;

	// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath
	typedef HRESULT (WINAPI *FnShellGetKnownFolder)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
	FnShellGetKnownFolder shellGetKnownFolderOrig = nullptr;

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

	HRESULT WINAPI ShellGetKnownFolderPathHook(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, PWSTR* ppszPath)
	{
		// Convert rfid to string
		constexpr int guidCharMax = 40;
		OLECHAR guidStringW[guidCharMax] = {};
		StringFromGUID2(rfid, guidStringW, guidCharMax);
		char guidString[guidCharMax] = {};
		WideCharToMultiByte(CP_ACP, 0, guidStringW, -1, guidString, guidCharMax, nullptr, nullptr);

		HRESULT result = shellGetKnownFolderOrig(rfid, dwFlags, hToken, ppszPath);
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

	// Overwrites an address in the game's import address table.
	bool HookIat(uintptr_t address, uintptr_t hookAddress, const char* name)
	{
		if (!address)
		{
			mod::Log(category, " {} - address is null", name);
			return false;
		}

		// Scan the whole IAT region
		for (uintptr_t slotOffset = iatRegionStart; slotOffset < iatRegionEnd; slotOffset += 8)
		{
			uintptr_t* slotAddress = reinterpret_cast<uintptr_t*>(mod::ad2ModuleBase + slotOffset);
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
				mod::Log(category, " {} - PATCHED slot @base0x+{:x} (orig={})", name, static_cast<unsigned>(slotOffset), reinterpret_cast<void*>(address));
				return true;
			}
		}
		mod::Log(category, " {} - slot NOT FOUND in IAT {:#x}..{:#x}", name, static_cast<unsigned>(iatRegionStart), static_cast<unsigned>(iatRegionEnd));
		return false;
	}

	template <typename T>
	bool HookIatForModule(LPCSTR module, LPCSTR name, T hookAddress, T& replacedAddress)
	{
		HMODULE handle = GetModuleHandleA(module);
		uintptr_t address = handle ? reinterpret_cast<uintptr_t>(GetProcAddress(handle, name)) : 0;
		if (HookIat(address, reinterpret_cast<uintptr_t>(hookAddress), name))
		{
			replacedAddress = reinterpret_cast<T>(address);
			return true;
		}
		else
		{
			replacedAddress = nullptr;
			return false;
		}
	}

	// Kernel DLL Hook variant: the game's slots may hold the kernelbase target or the kernel32 forwarder
	template <typename T>
	bool HookIatForKernelDll(LPCSTR name, T hookAddress, T& replacedAddress)
	{
		return HookIatForModule("kernel32.dll", name, hookAddress, replacedAddress) ||
			   HookIatForModule("kernelbase.dll", name, hookAddress, replacedAddress);
	}
}

void hooks::iat::InstallHooks()
{
	HookIatForKernelDll("CreateFileW", CreateFileWHook, createFileWOrig);
	HookIatForKernelDll("GetFileAttributesW", GetFileAttributesWHook, getFileAttributesWOrig);
	HookIatForModule("shell32.dll", "SHGetKnownFolderPath", ShellGetKnownFolderPathHook, shellGetKnownFolderOrig);
	mod::Log(category, " Original Addresses - createFileW={} getFileAttributesW={} getKnownFolderPath={}",
		reinterpret_cast<void*>(createFileWOrig), reinterpret_cast<void*>(getFileAttributesWOrig), reinterpret_cast<void*>(shellGetKnownFolderOrig));
}

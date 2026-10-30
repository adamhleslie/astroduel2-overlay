// Import Address Table (IAT) hooks used for runtime logging
#include "hooks_iat.h"

#include <cstdio>

#include "mod.h"
#include "shlobj_core.h"

namespace
{
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

	HANDLE WINAPI CreateFileWHook(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
										DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		HANDLE result = createFileWOrig(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		mod::Log("GAME CreateFileW: %ls (result=%p, error=%lu)", lpFileName, result, GetLastError());
		return result;
	}

	DWORD WINAPI GetFileAttributesWHook(LPCWSTR lpFileName)
	{
		DWORD result = getFileAttributesWOrig(lpFileName);
		mod::Log("GAME GetFileAttributesW: %ls (result=0x%lx%s, error=%lu)", lpFileName, result, (result == INVALID_FILE_ATTRIBUTES ? " NOT-FOUND" : ""), GetLastError());
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
			mod::Log("GAME SHGetKnownFolderPath %s -> %ls", guidString, *ppszPath);
		}
		else
		{
			mod::Log("GAME SHGetKnownFolderPath %s FAILED hr=0x%08lX", guidString, static_cast<unsigned long>(result));
		}

		return result;
	}

	// Overwrites an address in the game's import address table.
	bool HookIat(uintptr_t address, uintptr_t hookAddress, const char* name)
	{
		if (!address)
		{
			mod::Log("iat-hook %s: address is null", name);
			return false;
		}

		// Scan the whole IAT region
		for (uintptr_t slotOffset = iatRegionStart; slotOffset < iatRegionEnd; slotOffset += 8)
		{
			uintptr_t* slotAddress = reinterpret_cast<uintptr_t*>(mod::moduleBase + slotOffset);
			if (*slotAddress == address)
			{
				DWORD newProtect = PAGE_READWRITE;
				DWORD oldProtect = 0;
				if (!VirtualProtect(slotAddress, 8, newProtect, &oldProtect))
				{
					mod::Log("iat-hook %s: VirtualProtect FAILED %lu", name, GetLastError());
					return false;
				}
				*slotAddress = hookAddress;
				VirtualProtect(slotAddress, 8, oldProtect, &newProtect);
				mod::Log("iat-hook %s: PATCHED slot @base0x+%x (orig=%p)", name, static_cast<unsigned>(slotOffset), reinterpret_cast<void*>(address));
				return true;
			}
		}
		mod::Log("iat-hook %s: slot NOT FOUND in IAT 0x%x..0x%x", name, static_cast<unsigned>(iatRegionStart), static_cast<unsigned>(iatRegionEnd));
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
	mod::Log("iat-hook Original Addresses: createFileW=%p getFileAttributesW=%p getKnownFolderPath=%p",
		reinterpret_cast<void*>(createFileWOrig), reinterpret_cast<void*>(getFileAttributesWOrig), reinterpret_cast<void*>(shellGetKnownFolderOrig));
}

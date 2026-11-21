#include "hooks_iat.h"

#include <windows.h>
#include <shlobj_core.h>

#include "ad2_iat.h"
#include "mod.h"

namespace
{
	constexpr std::string_view category = "game";

	// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
	decltype(&CreateFileW) createFileWOrig = nullptr;

	// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileattributesw
	decltype(&GetFileAttributesW) getFileAttributesWOrig = nullptr;

	// https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath
	decltype(&SHGetKnownFolderPath) shGetKnownFolderOrig = nullptr;

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
		assert(createFileWOrig);
		HANDLE result = createFileWOrig(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		mod::Log(category, "CreateFileW {} (result={}, error={})", LPCWSTRToString(lpFileName), result, GetLastError());
		return result;
	}

	DWORD WINAPI GetFileAttributesWHook(LPCWSTR lpFileName)
	{
		assert(getFileAttributesWOrig);
		DWORD result = getFileAttributesWOrig(lpFileName);
		mod::Log(category, "GetFileAttributesW {} (result={:#x}{}, error={})", LPCWSTRToString(lpFileName), result, (result == INVALID_FILE_ATTRIBUTES ? " NOT-FOUND" : ""), GetLastError());
		return result;
	}

	HRESULT WINAPI SHGetKnownFolderPathHook(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, PWSTR* ppszPath)
	{
		assert(shGetKnownFolderOrig);

		// Convert rfid to string
		constexpr int guidCharMax = 40;
		OLECHAR guidStringW[guidCharMax] = {};
		StringFromGUID2(rfid, guidStringW, guidCharMax);
		char guidString[guidCharMax] = {};
		WideCharToMultiByte(CP_ACP, 0, guidStringW, -1, guidString, guidCharMax, nullptr, nullptr);

		HRESULT result = shGetKnownFolderOrig(rfid, dwFlags, hToken, ppszPath);
		if (result == S_OK && ppszPath && *ppszPath)
		{
			mod::Log(category, "SHGetKnownFolderPath {} -> {}", guidString, LPCWSTRToString(*ppszPath));
		}
		else
		{
			mod::Log(category, "SHGetKnownFolderPath {} FAILED hr={:#010X}", guidString, static_cast<unsigned long>(result));
		}

		return result;
	}
}

bool hooks::iat::InstallHooks()
{
	return ad2::iat::HookIatForKernel(NAME_OF(CreateFileW), CreateFileWHook, createFileWOrig) &&
		ad2::iat::HookIatForKernel(NAME_OF(GetFileAttributesW), GetFileAttributesWHook, getFileAttributesWOrig) &&
		ad2::iat::HookIatForModule("shell32.dll", NAME_OF(SHGetKnownFolderPath), SHGetKnownFolderPathHook, shGetKnownFolderOrig);
}

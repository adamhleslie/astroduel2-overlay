/* file-API IAT hooks: runtime path log (match-start diagnostics)      */
/*                                                                     */
/* IAT-hooks the game's own file imports (kernel32/shell32, not VMP   */
/* code) and logs every dir scan, mkdir, and open on "interesting"    */
/* paths (levels/*.tmx, *_data.xml, res/...). Ground truth for WHICH   */
/* level files a match actually loads.                                  */
/* ------------------------------------------------------------------ */

#include "hooks_file.h"

#include <cstdio>

#include "mod.h"

static volatile LONG hookActive = 0;

typedef HANDLE (WINAPI *FnFindFirstFileW)(const wchar_t*, WIN32_FIND_DATAW*);

typedef BOOL (WINAPI *FnCreateDirectoryW)(const wchar_t*, void*);

typedef HANDLE (WINAPI *FnCreateFileW)(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, void*);

typedef BOOL (WINAPI *FnCreateFile2)(const wchar_t*, void*, DWORD, DWORD, void*);

typedef DWORD (WINAPI *FnGetFileAttributesW)(const wchar_t*);

typedef HRESULT (WINAPI *FnShellGetKnownFolder)(const GUID*, DWORD, HANDLE, PWSTR*);

static FnFindFirstFileW findFirstFileOrig = nullptr;
static FnCreateDirectoryW createDirectoryOrig = nullptr;
static FnCreateFileW createFileOrig = nullptr;
static FnCreateFile2 createFile2Orig = nullptr;
static FnGetFileAttributesW getFileAttributesOrig = nullptr;
static FnShellGetKnownFolder shellGetKnownFolderOrig = nullptr;

/* case-insensitive substring test (hooks run at game call rates; keep it
   cheap -- a handful of short needles, early-out) */
static bool IContains(const wchar_t* hay, const wchar_t* needle)
{
	for (const wchar_t* h = hay; *h; ++h)
	{
		const wchar_t *a = h, *b = needle;
		while (*a && *b)
		{
			wchar_t la = (*a >= L'A' && *a <= L'Z') ? (wchar_t) (*a + 32) : *a;
			wchar_t lb = (*b >= L'A' && *b <= L'Z') ? (wchar_t) (*b + 32) : *b;
			if (la != lb) break;
			++a;
			++b;
		}
		if (*b == 0) return true;
	}
	return false;
}

/* Only interesting paths reach the log for the high-volume file APIs:
   custom-levels names, the candidate base dirs, and anything level-ish. */
static bool PathInteresting(const wchar_t* p)
{
	if (!p) return false;
	static const wchar_t* const kFilters[] = {
		L"tmx", L"_data.xml", L"bounty", L"ironcactus", L"dropbox",
		L"\\games\\", L"astro duel",
	};
	for (size_t i = 0; i < sizeof(kFilters) / sizeof(kFilters[0]); i++)
		if (IContains(p, kFilters[i])) return true;
	return false;
}

static HANDLE WINAPI Ad2FindFirstFileW(const wchar_t* pattern, WIN32_FIND_DATAW* fd)
{
	if (pattern && hookActive) mod::Log("GAME FindFirstFileW: %ls", pattern);
	return findFirstFileOrig(pattern, fd);
}

static BOOL WINAPI Ad2CreateDirectoryW(const wchar_t* path, void* sa)
{
	BOOL r = createDirectoryOrig(path, sa);
	if (path && hookActive) mod::Log("GAME CreateDirectoryW: %ls  (ret=%d, err=%lu)", path, r, GetLastError());
	return r;
}

/* CreateFileW: catches level-file opens AND failures (the "lists but won't
   load" case). Filtered to interesting paths so res-loading noise stays out. */
static HANDLE WINAPI Ad2CreateFileW(const wchar_t* path, DWORD acc, DWORD share,
                                    void* sa, DWORD disp, DWORD flags, void* tmpl)
{
	HANDLE h = createFileOrig(path, acc, share, sa, disp, flags, tmpl);
	if (hookActive && PathInteresting(path))
		mod::Log("GAME CreateFileW: %ls  (ret=%p%s)", path, (void*) h,
		         h == INVALID_HANDLE_VALUE ? " FAILED" : "");
	return h;
}

static BOOL WINAPI Ad2CreateFile2(const wchar_t* path, void* ph, DWORD acc,
                                  DWORD opts, void* info)
{
	BOOL r = createFile2Orig(path, ph, acc, opts, info);
	if (hookActive && PathInteresting(path))
		mod::Log("GAME CreateFile2: %ls  (ret=%d%s)", path, r, r ? "" : " FAILED");
	return r;
}

/* GetFileAttributesW: the dir-exists pre-checks probe through this --
   a NOT-FOUND result on a base dir shows up here. */
static DWORD WINAPI Ad2GetFileAttributesW(const wchar_t* path)
{
	DWORD r = getFileAttributesOrig(path);
	if (hookActive && PathInteresting(path))
		mod::Log("GAME GetFileAttributesW: %ls  (ret=0x%lx%s)", path, r,
		         r == INVALID_FILE_ATTRIBUTES ? " NOT-FOUND" : "");
	return r;
}

/* fopen: the real level-load path. cocos FileUtils::fillFileData
   (_stat64i32 + fopen(path,"rb")) loads every .tmx and *_data.xml through
   the CRT import, invisible to the kernel32 IAT hooks. Hook the game's own
   fopen import (ucrtbase or msvcrt, resolved at runtime; only the one
   actually in the game IAT will match a slot). */
typedef FILE* (__cdecl *FnFopen)(const char*, const char*);

static FnFopen fopenOrig = nullptr;

static bool PathInterestingA(const char* p)
{
	if (!p) return false;
	struct
	{
		const char* s;
		size_t n;
	} f[] = {
		{"tmx", 3}, {"_data.xml", 9}, {"res/levels", 10},
		{"bounty", 6}, {"dropbox", 7},
	};
	/* case-insensitive substring scan */
	for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
	{
		for (const char* h = p; *h; ++h)
		{
			const char* a = h;
			size_t j = 0;
			for (; *a; ++a, ++j)
			{
				char la = (*a >= 'A' && *a <= 'Z') ? (char) (*a + 32) : *a;
				char lb = (f[i].s[j] >= 'A' && f[i].s[j] <= 'Z') ? (char) (f[i].s[j] + 32) : f[i].s[j];
				if (la != lb) break;
				if (j + 1 == f[i].n) return true;
			}
		}
	}
	return false;
}

static FILE* __cdecl Ad2Fopen(const char* path, const char* mode)
{
	FILE* f = fopenOrig(path, mode);
	if (hookActive && PathInterestingA(path))
		mod::Log("GAME fopen: %s  [%s]  (ret=%p%s)", path, mode ? mode : "?",
		         (void*) f, f ? "" : " FAILED");
	return f;
}

/* SHGetKnownFolderPath: the base-path getter calls this with
   FOLDERID_UserGameLocations. Logging every request + result is direct
   ground truth for the true base path. */
static void LogGuid(const GUID* g, char* out, size_t n)
{
	snprintf(out, n, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
	         g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
	         g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static HRESULT WINAPI Ad2ShellGetKnownFolder(const GUID* rfid, DWORD flags, HANDLE tok, PWSTR* out)
{
	HRESULT hr = shellGetKnownFolderOrig(rfid, flags, tok, out);
	if (hookActive)
	{
		char g[64] = "(null)";
		if (rfid) LogGuid(rfid, g, sizeof g);
		if (hr == S_OK && out && *out)
			mod::Log("GAME SHGetKnownFolderPath %s -> %ls", g, *out);
		else
			mod::Log("GAME SHGetKnownFolderPath %s FAILED hr=0x%08lX", g, (unsigned long) hr);
	}
	return hr;
}

/* IAT-slot hook core (same mechanism as the SDL_GL_SwapWindow hook:
   overwrite the game's import-table slot, no trampolines, no patching of
   system-DLL code). `expected` is the runtime-resolved address the slot
   should hold. */
static uintptr_t HookIatSlot(uintptr_t expected, void* hook, const char* tag)
{
	if (!expected)
	{
		mod::Log("iat-hook %s: expected address NOT resolvable", tag);
		return 0;
	}
	mod::Log("iat-hook %s: expected=%p", tag, (void*) expected);
	/* scan the whole IAT region (SDL slot at +0x47e3c0) */
	for (uintptr_t va = 0x47e000; va < 0x495a00; va += 8)
	{
		uintptr_t* slot = (uintptr_t*) (mod::moduleBase + va);
		if (*slot == expected)
		{
			uintptr_t orig = *slot;
			DWORD old = 0;
			if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old))
			{
				mod::Log("iat-hook %s: VirtualProtect FAILED %lu", tag, GetLastError());
				return 0;
			}
			*slot = (uintptr_t) hook;
			DWORD d = 0;
			VirtualProtect(slot, 8, old, &d);
			mod::Log("iat-hook %s: PATCHED slot @base+%x (orig=%p)", tag, (unsigned) va, (void*) orig);
			return orig;
		}
	}
	mod::Log("iat-hook %s: slot NOT FOUND in IAT 0x47e000..0x495a00", tag);
	return 0;
}

/* k32/kb variant: the game's slots may hold the kernel32 forwarder or the
   kernelbase target (match must use the runtime-resolved address of either). */
static uintptr_t HookIatImport(const char* name, void* hook)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	HMODULE kb = GetModuleHandleA("kernelbase.dll");
	uintptr_t exp[2] = {
		k32 ? (uintptr_t) GetProcAddress(k32, name) : 0,
		kb ? (uintptr_t) GetProcAddress(kb, name) : 0,
	};
	for (int i = 0; i < 2; i++)
	{
		if (!exp[i]) continue;
		uintptr_t r = HookIatSlot(exp[i], hook, name);
		if (r) return r;
	}
	return 0;
}

static uintptr_t HookIatImportDll(const char* dll, const char* name, void* hook)
{
	HMODULE h = GetModuleHandleA(dll);
	uintptr_t exp = h ? (uintptr_t) GetProcAddress(h, name) : 0;
	return HookIatSlot(exp, hook, name);
}

void hooks::file::InstallHooks()
{
	uintptr_t o1 = HookIatImport("FindFirstFileW", Ad2FindFirstFileW);
	uintptr_t o2 = HookIatImport("CreateDirectoryW", Ad2CreateDirectoryW);
	if (o1) findFirstFileOrig = (FnFindFirstFileW) o1;
	if (o2) createDirectoryOrig = (FnCreateDirectoryW) o2;
	/* activate only once the scan hooks are set, to avoid a torn state */
	if (o1 && o2)
		InterlockedExchange(&hookActive, 1);
	else mod::Log("fileapi: hooks incomplete (fff=%p cdw=%p) -- logging disabled", (void*) o1, (void*) o2);

	/* supplementary capture (each independent; a missing slot just logs) */
	uintptr_t o3 = HookIatImport("CreateFileW", Ad2CreateFileW);
	if (o3) createFileOrig = (FnCreateFileW) o3;
	uintptr_t o4 = HookIatImport("CreateFile2", Ad2CreateFile2);
	if (o4) createFile2Orig = (FnCreateFile2) o4;
	uintptr_t o5 = HookIatImport("GetFileAttributesW", Ad2GetFileAttributesW);
	if (o5) getFileAttributesOrig = (FnGetFileAttributesW) o5;
	uintptr_t o6 = HookIatImportDll("shell32.dll", "SHGetKnownFolderPath", Ad2ShellGetKnownFolder);
	if (o6) shellGetKnownFolderOrig = (FnShellGetKnownFolder) o6;
	/* CRT fopen: try ucrtbase first, then msvcrt -- whichever address the
	   loader actually bound into the game's IAT is the one that matches. */
	uintptr_t o7 = HookIatImportDll("ucrtbase.dll", "fopen", Ad2Fopen);
	if (!o7) o7 = HookIatImportDll("msvcrt.dll", "fopen", Ad2Fopen);
	if (o7) fopenOrig = (FnFopen) o7;
	mod::Log("fileapi: summary fff=%p cdw=%p cfw=%p cf2=%p gfa=%p shgkf=%p fopen=%p",
	         (void*) o1, (void*) o2, (void*) o3, (void*) o4, (void*) o5, (void*) o6, (void*) o7);
}

/*
 * hsmadvisor_integration_loader.h - finds and loads HSMAdvisorIntegration.dll at run time.
 *
 * Header-only; compile it into your integration. hsma_load() tries, in order:
 *   1. the copy installed with HSMAdvisor:  <HSMAdvisor folder>\integration\<x64|x86>\HSMAdvisorIntegration.dll
 *      (the HSMAdvisor folder is found like hsma_find_executable(): HSMADVISOR_EXE,
 *      Software\HSMAdvisor\ExePath, the uninstall entry, Program Files\HSMAdvisor)
 *   2. bundled_dll_path: a copy you ship with your integration (may be NULL)
 * The installed copy is preferred because it is updated together with HSMAdvisor; a bundled copy
 * lets the integration work with HSMAdvisor versions that do not install the DLL yet.
 *
 *     hsma_api api;
 *     if (hsma_load(&api, L"C:\\MyPlugin\\HSMAdvisorIntegration.dll") == HSMA_LOAD_OK) {
 *         hsma_client* client = api.client_create("My CAM", "1.0");
 *         char* payload = NULL; char* error = NULL;
 *         int status = api.request(client, "Hello", NULL, 0, NULL, &payload, &error);
 *         ...
 *         api.free_memory(payload); api.free_memory(error);
 *         api.client_destroy(client);
 *         hsma_unload(&api);
 *     }
 *
 * Define HSMA_LOADER_VERIFY_SIGNATURE before including this header to accept only DLLs with a
 * valid Authenticode signature (links wintrust.lib).
 *
 * (c) Eldar Gerfanov, HSMAdvisor Inc. MIT License: you may copy this header into your integration.
 */
#ifndef HSMADVISOR_INTEGRATION_LOADER_H
#define HSMADVISOR_INTEGRATION_LOADER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include <wchar.h>

#include "hsmadvisor_integration.h"

#ifdef HSMA_LOADER_VERIFY_SIGNATURE
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HSMA_LOADER_FN static __inline

#if defined(_M_X64) || defined(_M_AMD64)
#define HSMA_LOADER_ARCH L"x64"
#elif defined(_M_ARM64)
#define HSMA_LOADER_ARCH L"arm64"
#else
#define HSMA_LOADER_ARCH L"x86"
#endif

#define HSMA_LOADER_DLL_NAME L"HSMAdvisorIntegration.dll"
#define HSMA_LOADER_MAX_PATH 2048

typedef enum hsma_load_result
{
    HSMA_LOAD_OK = 0,
    HSMA_LOAD_NOT_FOUND = 1, /* no HSMAdvisorIntegration.dll in either location      */
    HSMA_LOAD_TOO_OLD = 2,   /* a DLL was found but its API is older than this header */
    HSMA_LOAD_UNTRUSTED = 3  /* a DLL was found but its signature is not valid        */
} hsma_load_result;

/* The loaded DLL's functions; see hsmadvisor_integration.h for what each does. */
typedef struct hsma_api
{
    HMODULE module;
    wchar_t path[HSMA_LOADER_MAX_PATH]; /* the DLL that was loaded */

    hsma_pfn_api_version api_version;
    hsma_pfn_client_create client_create;
    hsma_pfn_client_destroy client_destroy;
    hsma_pfn_client_set_action client_set_action;
    hsma_pfn_client_set_minimum_version client_set_minimum_version;
    hsma_pfn_client_set_executable_path client_set_executable_path;
    hsma_pfn_client_set_launch_timeout_ms client_set_launch_timeout_ms;
    hsma_pfn_client_set_launching_callback client_set_launching_callback;
    hsma_pfn_client_set_pipe_name client_set_pipe_name;
    hsma_pfn_client_server_version client_server_version;
    hsma_pfn_cancel_create cancel_create;
    hsma_pfn_cancel_signal cancel_signal;
    hsma_pfn_cancel_destroy cancel_destroy;
    hsma_pfn_request request;
    hsma_pfn_request request_no_launch;
    hsma_pfn_is_running is_running;
    hsma_pfn_find_executable find_executable;
    hsma_pfn_get_pipe_name get_pipe_name;
    hsma_pfn_free free_memory; /* hsma_free */
} hsma_api;

HSMA_LOADER_FN int hsma_loader_is_file(const wchar_t* path)
{
    DWORD attr;
    if (!path || !*path) return 0;
    attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

HSMA_LOADER_FN int hsma_loader_append(wchar_t* buf, size_t cch, const wchar_t* s)
{
    size_t len = wcslen(buf), add = wcslen(s);
    if (len + add + 1 > cch) return 0;
    memcpy(buf + len, s, (add + 1) * sizeof(wchar_t));
    return 1;
}

/* dir + "\" + name into out */
HSMA_LOADER_FN int hsma_loader_combine(wchar_t* out, size_t cch, const wchar_t* dir, const wchar_t* name)
{
    size_t len;
    out[0] = L'\0';
    if (!hsma_loader_append(out, cch, dir)) return 0;
    len = wcslen(out);
    if (len > 0 && out[len - 1] != L'\\' && out[len - 1] != L'/' && !hsma_loader_append(out, cch, L"\\")) return 0;
    return hsma_loader_append(out, cch, name);
}

HSMA_LOADER_FN int hsma_loader_read_reg(HKEY hive, REGSAM view, const wchar_t* key, const wchar_t* value,
                                        wchar_t* out, DWORD cch)
{
    HKEY k;
    DWORD bytes = cch * (DWORD)sizeof(wchar_t);
    LSTATUS r;
    out[0] = L'\0';
    if (RegOpenKeyExW(hive, key, 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) return 0;
    r = RegGetValueW(k, NULL, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, out, &bytes);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) out[0] = L'\0';
    return r == ERROR_SUCCESS && out[0] != L'\0';
}

/* Installed HSMAdvisor.exe into exe (same order as hsma_find_executable). */
HSMA_LOADER_FN int hsma_loader_find_executable(wchar_t* exe, DWORD cch)
{
    static const wchar_t* uninstallKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{E6C929E6-A8DB-4DDF-B2FF-72DC696AF154}_is1";
    HKEY hives[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    REGSAM views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    const wchar_t* programFiles[2] = { L"ProgramFiles", L"ProgramFiles(x86)" };
    wchar_t dir[HSMA_LOADER_MAX_PATH];
    int h, v, i;

    DWORD n = GetEnvironmentVariableW(L"HSMADVISOR_EXE", exe, cch);
    if (n > 0 && n < cch && hsma_loader_is_file(exe)) return 1;

    for (h = 0; h < 2; ++h)
    {
        for (v = 0; v < 2; ++v)
        {
            if (hsma_loader_read_reg(hives[h], views[v], L"Software\\HSMAdvisor", L"ExePath", exe, cch) && hsma_loader_is_file(exe))
                return 1;
            if (hsma_loader_read_reg(hives[h], views[v], uninstallKey, L"InstallLocation", dir, HSMA_LOADER_MAX_PATH) &&
                hsma_loader_combine(exe, cch, dir, L"HSMAdvisor.exe") && hsma_loader_is_file(exe))
                return 1;
        }
    }

    for (i = 0; i < 2; ++i)
    {
        n = GetEnvironmentVariableW(programFiles[i], dir, HSMA_LOADER_MAX_PATH);
        if (n > 0 && n < HSMA_LOADER_MAX_PATH && hsma_loader_combine(exe, cch, dir, L"HSMAdvisor\\HSMAdvisor.exe") &&
            hsma_loader_is_file(exe))
            return 1;
    }
    exe[0] = L'\0';
    return 0;
}

#ifdef HSMA_LOADER_VERIFY_SIGNATURE
HSMA_LOADER_FN int hsma_loader_is_signed(const wchar_t* path)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO file;
    WINTRUST_DATA data;
    LONG status;
    memset(&file, 0, sizeof(file));
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path;
    memset(&data, 0, sizeof(data));
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    status = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
    return status == ERROR_SUCCESS;
}
#endif

#define HSMA_LOADER_GET(field, name)                                                   \
    if ((*(FARPROC*)&api->field = GetProcAddress(api->module, name)) == NULL) complete = 0

HSMA_LOADER_FN hsma_load_result hsma_loader_try(hsma_api* api, const wchar_t* path)
{
    int complete = 1;
    memset(api, 0, sizeof(*api));
    if (!hsma_loader_is_file(path) || wcslen(path) >= HSMA_LOADER_MAX_PATH) return HSMA_LOAD_NOT_FOUND;
#ifdef HSMA_LOADER_VERIFY_SIGNATURE
    if (!hsma_loader_is_signed(path)) return HSMA_LOAD_UNTRUSTED;
#endif
    api->module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!api->module) return HSMA_LOAD_NOT_FOUND;

    HSMA_LOADER_GET(api_version, "hsma_api_version");
    HSMA_LOADER_GET(client_create, "hsma_client_create");
    HSMA_LOADER_GET(client_destroy, "hsma_client_destroy");
    HSMA_LOADER_GET(client_set_action, "hsma_client_set_action");
    HSMA_LOADER_GET(client_set_minimum_version, "hsma_client_set_minimum_version");
    HSMA_LOADER_GET(client_set_executable_path, "hsma_client_set_executable_path");
    HSMA_LOADER_GET(client_set_launch_timeout_ms, "hsma_client_set_launch_timeout_ms");
    HSMA_LOADER_GET(client_set_launching_callback, "hsma_client_set_launching_callback");
    HSMA_LOADER_GET(client_set_pipe_name, "hsma_client_set_pipe_name");
    HSMA_LOADER_GET(client_server_version, "hsma_client_server_version");
    HSMA_LOADER_GET(cancel_create, "hsma_cancel_create");
    HSMA_LOADER_GET(cancel_signal, "hsma_cancel_signal");
    HSMA_LOADER_GET(cancel_destroy, "hsma_cancel_destroy");
    HSMA_LOADER_GET(request, "hsma_request");
    HSMA_LOADER_GET(request_no_launch, "hsma_request_no_launch");
    HSMA_LOADER_GET(is_running, "hsma_is_running");
    HSMA_LOADER_GET(find_executable, "hsma_find_executable");
    HSMA_LOADER_GET(get_pipe_name, "hsma_get_pipe_name");
    HSMA_LOADER_GET(free_memory, "hsma_free");

    if (!complete || api->api_version() < HSMA_API_VERSION)
    {
        FreeLibrary(api->module);
        memset(api, 0, sizeof(*api));
        return HSMA_LOAD_TOO_OLD;
    }
    wcscpy_s(api->path, HSMA_LOADER_MAX_PATH, path);
    return HSMA_LOAD_OK;
}

#undef HSMA_LOADER_GET

/* Loads the installed DLL, else bundled_dll_path (may be NULL). On failure *api is zeroed and the
 * most specific reason is returned (TOO_OLD / UNTRUSTED before NOT_FOUND). */
HSMA_LOADER_FN hsma_load_result hsma_load(hsma_api* api, const wchar_t* bundled_dll_path)
{
    wchar_t exe[HSMA_LOADER_MAX_PATH];
    wchar_t candidate[HSMA_LOADER_MAX_PATH];
    hsma_load_result best = HSMA_LOAD_NOT_FOUND, r;

    memset(api, 0, sizeof(*api));
    if (hsma_loader_find_executable(exe, HSMA_LOADER_MAX_PATH))
    {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = L'\0';
        if (hsma_loader_combine(candidate, HSMA_LOADER_MAX_PATH, exe, L"integration\\" HSMA_LOADER_ARCH L"\\" HSMA_LOADER_DLL_NAME))
        {
            r = hsma_loader_try(api, candidate);
            if (r == HSMA_LOAD_OK) return r;
            if (r != HSMA_LOAD_NOT_FOUND) best = r;
        }
    }
    if (bundled_dll_path)
    {
        r = hsma_loader_try(api, bundled_dll_path);
        if (r == HSMA_LOAD_OK) return r;
        if (r != HSMA_LOAD_NOT_FOUND) best = r;
    }
    memset(api, 0, sizeof(*api));
    return best;
}

/* Path of the loaded DLL ("" when nothing is loaded), e.g. to log which copy is in use. */
HSMA_LOADER_FN const wchar_t* hsma_loaded_from(const hsma_api* api)
{
    return api->path;
}

HSMA_LOADER_FN void hsma_unload(hsma_api* api)
{
    if (api->module) FreeLibrary(api->module);
    memset(api, 0, sizeof(*api));
}

#ifdef __cplusplus
}
#endif

#endif /* HSMADVISOR_INTEGRATION_LOADER_H */

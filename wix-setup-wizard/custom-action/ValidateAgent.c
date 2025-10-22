// =============================================================================
//  ONEGOV PASSWORD AGENT - ValidateAgent.c
//
//  Purpose:
//    MSI/WiX Custom Action DLL that performs a simple REST validation step
//    during installer runtime. Designed to confirm OneGov connectivity
//    before proceeding to installation.
//
//  Behavior:
//    1. Reads HOSTNAME, IPADDRESS, and SECRETKEY from MSI properties.
//    2. Builds JSON payload and performs an HTTPS POST to demo endpoint.
//    3. Parses the HTTP response; if contains "token", marks success.
//    4. Sets REST_OK / REST_MSG properties for WiX navigation logic.
//
//  Notes:
//    • This version uses https://reqres.in as demo endpoint (no real API call).
//    • Always returns ERROR_SUCCESS so the UI never crashes.
//    • MSI logs show the detailed steps if logging is enabled.
//
//  Build Command (x64 Native Tools):
//    cl /LD ValidateAgent.c /Fe:ValidateAgent.dll msi.lib winhttp.lib ^
//       /DUNICODE /D_UNICODE /W4 /WX- /Zi /nologo /MD /link /DEF:ValidateAgent.def
//
//  WiX Integration (example):
//    <Binary Id="RestCaDll" SourceFile="bin\ValidateAgent.dll"/>
//    <CustomAction Id="ValidateAgent" BinaryRef="RestCaDll"
//                  DllEntry="ValidateAgent" Execute="immediate" Return="check"/>
//    <Publish Event="DoAction" Value="ValidateAgent"
//             Condition="HOSTNAME AND IPADDRESS AND SECRETKEY"/>
//    <Publish Event="NewDialog" Value="VerifyReadyDlg"
//             Condition="REST_OK = 1"/>
//    <Publish Event="SpawnDialog" Value="ConfigErrorDlg"
//             Condition="REST_OK <> 1"/>
// =============================================================================

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <winhttp.h>
#include <strsafe.h>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "winhttp.lib")

// ------------------------------------------------------------
// Constants
// ------------------------------------------------------------
static const WCHAR* kHost = L"reqres.in";                // Target host (no "https://")
static const WCHAR* kPath = L"/api/login";               // REST endpoint path
static const DWORD  kPort = INTERNET_DEFAULT_HTTPS_PORT; // 443 (HTTPS)
static const DWORD  kTimeoutMs = 7000;                   // Network timeouts
#define JSON_W_CAPACITY 1024                             // Buffer for payload

// ------------------------------------------------------------
// MSI LOGGING HELPERS
// ------------------------------------------------------------
// Write log entries into MSI log file (visible if logging enabled)
static void LogMessage(MSIHANDLE hInstall, LPCWSTR text)
{
    if (!text) return;
    PMSIHANDLE hRec = MsiCreateRecord(1);
    if (!hRec) return;
    MsiRecordSetStringW(hRec, 0, L"[1]");
    MsiRecordSetStringW(hRec, 1, text);
    MsiProcessMessage(hInstall, INSTALLMESSAGE_INFO, hRec);
}

static void LogFmt(MSIHANDLE hInstall, LPCWSTR fmt, LPCWSTR a, LPCWSTR b)
{
    WCHAR buf[512];
    if (SUCCEEDED(StringCchPrintfW(buf, _countof(buf), fmt,
                                   a ? a : L"", b ? b : L"")))
        LogMessage(hInstall, buf);
}

// ------------------------------------------------------------
// MSI PROPERTY HELPERS
// ------------------------------------------------------------
static wchar_t* GetMsiPropAlloc(MSIHANDLE h, LPCWSTR name)
{
    if (!name) return NULL;
    DWORD cch = 0;
    UINT rc = MsiGetPropertyW(h, name, L"", &cch);
    if (rc == ERROR_MORE_DATA) cch++;
    if (cch == 0) cch = 1;

    wchar_t* buf = (wchar_t*)LocalAlloc(LPTR, cch * sizeof(wchar_t));
    if (!buf) return NULL;

    rc = MsiGetPropertyW(h, name, buf, &cch);
    if (rc != ERROR_SUCCESS)
    {
        LocalFree(buf);
        return NULL;
    }
    return buf; // may be empty string
}

static void SetMsiPropBool(MSIHANDLE h, LPCWSTR name, BOOL val)
{
    MsiSetPropertyW(h, name, val ? L"1" : L"0");
}

static void SetMsiPropMsg(MSIHANDLE h, LPCWSTR name, LPCWSTR msg)
{
    MsiSetPropertyW(h, name, msg ? msg : L"");
}

// ------------------------------------------------------------
// STRING UTILITY
// ------------------------------------------------------------
// Converts UTF-16 wide string → UTF-8 allocated buffer (HeapAlloc).
static char* WideToUtf8Alloc(const wchar_t* ws)
{
    if (!ws) return NULL;
    int cb = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (cb <= 0) return NULL;
    char* s = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb);
    if (!s) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, cb, NULL, NULL))
    {
        HeapFree(GetProcessHeap(), 0, s);
        return NULL;
    }
    return s;
}

// ------------------------------------------------------------
// HTTP POST FUNCTION (DEMO IMPLEMENTATION)
// ------------------------------------------------------------
// Performs HTTPS POST to https://reqres.in/api/login using demo credentials.
// Returns TRUE if response status == 200 and body contains "token".
static BOOL HttpPostJsonEchoContainsSecret(void)
{
    BOOL ok = FALSE;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    // Hardcoded demo JSON body for ReqRes
    static const char* kReqResJson =
        "{\"email\":\"eve.holt@reqres.in\",\"password\":\"cityslicka\"}";
    const DWORD reqBodyLen = (DWORD)strlen(kReqResJson);

    // --- Session ---
    hSession = WinHttpOpen(L"OneGovSetup/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto done;

    // --- Connect to host ---
    hConnect = WinHttpConnect(hSession, kHost, kPort, 0);
    if (!hConnect) goto done;

    // --- Create POST request ---
    hRequest = WinHttpOpenRequest(hConnect, L"POST", kPath,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE);
    if (!hRequest) goto done;

    // Apply timeouts for UI responsiveness
    WinHttpSetTimeouts(hRequest, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    LPCWSTR hdrs = L"Content-Type: application/json; charset=utf-8";
    if (!WinHttpSendRequest(hRequest, hdrs, (DWORD)-1,
                            (LPVOID)kReqResJson, reqBodyLen,
                            reqBodyLen, 0))
        goto done;

    if (!WinHttpReceiveResponse(hRequest, NULL)) goto done;

    // --- Status code ---
    DWORD status = 0, slen = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &slen, WINHTTP_NO_HEADER_INDEX))
        goto done;

    // --- Read response body (accumulate) ---
    char* body = NULL;
    DWORD total = 0;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (!avail) break;

        char* nb = (char*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      body, total + avail + 1);
        if (!nb)
        {
            if (body) HeapFree(GetProcessHeap(), 0, body);
            body = NULL;
            break;
        }
        body = nb;

        DWORD read = 0;
        if (!WinHttpReadData(hRequest, body + total, avail, &read))
        {
            HeapFree(GetProcessHeap(), 0, body);
            body = NULL;
            break;
        }
        total += read;
        body[total] = '\0';
    }

    // Evaluate success
    if (status == 200 && body && strstr(body, "\"token\""))
        ok = TRUE;

    if (body) HeapFree(GetProcessHeap(), 0, body);

done:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return ok;
}

// ------------------------------------------------------------
// CUSTOM ACTION ENTRY POINT
// ------------------------------------------------------------
UINT __stdcall ValidateAgent(MSIHANDLE hInstall)
{
    // --- Retrieve MSI properties ---
    wchar_t* hostW = GetMsiPropAlloc(hInstall, L"HOSTNAME");
    wchar_t* ipW   = GetMsiPropAlloc(hInstall, L"IPADDRESS");
    wchar_t* secW  = GetMsiPropAlloc(hInstall, L"SECRETKEY");

    LogFmt(hInstall, L"[ValidateAgent] HOSTNAME='%s'", hostW, NULL);
    LogFmt(hInstall, L"[ValidateAgent] IPADDRESS='%s'", ipW,  NULL);
    LogFmt(hInstall, L"[ValidateAgent] SECRETKEY='%s'", secW, NULL);

    // --- Local validation ---
    if (!hostW || !*hostW || !ipW || !*ipW || !secW || !*secW)
    {
        LogMessage(hInstall, L"[ValidateAgent] Missing Host/IP/Secret; aborting.");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Missing Host/IP/Secret.");
        goto cleanup;
    }

    // --- Build placeholder JSON ---
    wchar_t jsonW[JSON_W_CAPACITY];
    if (FAILED(StringCchPrintfW(jsonW, _countof(jsonW),
                                L"{\"hostname\":\"%s\",\"ip\":\"%s\",\"secret\":\"%s\"}",
                                hostW, ipW, secW)))
    {
        LogMessage(hInstall, L"[ValidateAgent] Failed to format JSON payload.");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Failed to build JSON.");
        goto cleanup;
    }

    // --- Convert to UTF-8 (kept for real API version) ---
    char* jsonUtf8 = WideToUtf8Alloc(jsonW);
    char* secUtf8  = WideToUtf8Alloc(secW);
    if (!jsonUtf8 || !secUtf8)
    {
        LogMessage(hInstall, L"[ValidateAgent] UTF-8 conversion failed.");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
        goto cleanup;
    }

    // --- Log target URL ---
    WCHAR fullUrl[256];
    StringCchPrintfW(fullUrl, _countof(fullUrl), L"https://%s%s", kHost, kPath);
    LogFmt(hInstall, L"[ValidateAgent] POST %s", fullUrl, NULL);

    // --- Perform HTTP POST ---
    BOOL ok = HttpPostJsonEchoContainsSecret();

    // Free transient buffers
    HeapFree(GetProcessHeap(), 0, jsonUtf8);
    HeapFree(GetProcessHeap(), 0, secUtf8);

    // --- Set result properties for WiX UI logic ---
    if (ok)
    {
        LogMessage(hInstall, L"[ValidateAgent] Validation SUCCESS (ReqRes token received).");
        SetMsiPropBool(hInstall, L"REST_OK", TRUE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Validated via ReqRes.");
    }
    else
    {
        LogMessage(hInstall, L"[ValidateAgent] Validation FAILED (ReqRes).");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Server validation failed.");
    }

cleanup:
    if (hostW) LocalFree(hostW);
    if (ipW)   LocalFree(ipW);
    if (secW)  LocalFree(secW);
    return ERROR_SUCCESS; // Always return success to avoid MSI abort
}

// ------------------------------------------------------------
// DLL ENTRY POINT
// ------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(h); // Reduce thread overhead
    return TRUE;
}

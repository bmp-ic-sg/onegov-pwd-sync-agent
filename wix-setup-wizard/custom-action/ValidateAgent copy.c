/**
 * ValidateAgent.c
 * -----------------------------------------------------------------------------
 * WiX/MSI Custom Action (C) that performs a simple REST validation during the
 * installer UI. Invoke from your Next button via:
 *
 *   <Publish Event="DoAction" Value="ValidateAgent"
 *            Condition="HOSTNAME AND IPADDRESS AND SECRETKEY"/>
 *
 * Behavior
 * --------
 * 1) Reads MSI properties: HOSTNAME, IPADDRESS, SECRETKEY (Unicode).
 * 2) Builds JSON: {"hostname":"...","ip":"...","secret":"..."}.
 * 3) Sends HTTP POST to http://localhost:8080/validate using WinHTTP.
 * 4) If HTTP status == 200 AND the response body contains the provided SECRETKEY,
 *    sets MSI property REST_OK=1 and REST_MSG="Validated.".
 *    Otherwise sets REST_OK=0 and an error message in REST_MSG.
 * 5) Always returns ERROR_SUCCESS (never crash the UI). Gate navigation in WiX
 *    using the REST_OK property.
 *
 * Logging
 * -------
 * - Verbose logs are written to the MSI log via MsiProcessMessage (if logging is enabled).
 * - SECRETKEY is a public key in your scenario, so it is logged as-is (no masking).
 *
 * Build (x64 Native Tools for VS)
 * --------------------------------
 *   cl /LD ValidateAgent.c /Fe:ValidateAgent.dll msi.lib winhttp.lib ^
 *      /DUNICODE /D_UNICODE /W4 /WX- /Zi /nologo /MD /link /DEF:ValidateAgent.def
 *
 * WiX Wiring (example)
 * --------------------
 *   <Binary Id="RestCaDll" SourceFile="bin\ValidateAgent.dll"/>
 *   <CustomAction Id="ValidateAgent"
 *                 BinaryRef="RestCaDll"
 *                 DllEntry="ValidateAgent"
 *                 Execute="immediate"
 *                 Return="check"/>
 *   <Publish Event="DoAction"   Value="ValidateAgent"
 *            Condition="HOSTNAME AND IPADDRESS AND SECRETKEY"/>
 *   <Publish Event="NewDialog"  Value="VerifyReadyDlg"
 *            Condition="HOSTNAME AND IPADDRESS AND SECRETKEY AND REST_OK = 1"/>
 *   <Publish Event="SpawnDialog" Value="ConfigErrorDlg"
 *            Condition="HOSTNAME AND IPADDRESS AND SECRETKEY AND REST_OK <> 1"/>
 */

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <winhttp.h>
#include <strsafe.h>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "winhttp.lib")

// =========================== Constants & Macros ===============================

// ---- Endpoint constants (your local validator) ----
// NOTE: WinHttpConnect takes the host only (no "http://").
static const WCHAR* kHost = L"localhost";       // host only
static const WCHAR* kPath = L"/validate";       // leading slash required
static const DWORD  kPort = 8080;               // your custom port

// ---- Request flag constants ----
#define REQUEST_FLAGS_HTTP   0
#define REQUEST_FLAGS_HTTPS  WINHTTP_FLAG_SECURE

// You are using HTTP here:
static const DWORD kRequestFlags = REQUEST_FLAGS_HTTP;   // HTTP (no TLS)
// If you switch to HTTPS on a custom port, flip to:
// static const DWORD kRequestFlags = REQUEST_FLAGS_HTTPS;

// Networking timeouts (milliseconds) to keep UI responsive
static const DWORD  kTimeoutMs = 7000;

// Max JSON buffer (wide chars) when composing payload
#define JSON_W_CAPACITY 1024

// ============================== MSI Logging ==================================

/**
 * LogMessage
 * ----------
 * Writes a message to the MSI log (if logging is enabled).
 */
static void LogMessage(MSIHANDLE hInstall, LPCWSTR text)
{
    if (!text) return;
    PMSIHANDLE hRec = MsiCreateRecord(1);
    if (!hRec) return;
    MsiRecordSetStringW(hRec, 0, L"[1]");
    MsiRecordSetStringW(hRec, 1, text);
    MsiProcessMessage(hInstall, INSTALLMESSAGE_INFO, hRec);
}

/**
 * LogFmt
 * ------
 * Formats a message into a small stack buffer, then logs it.
 */
static void LogFmt(MSIHANDLE hInstall, LPCWSTR fmt, LPCWSTR a, LPCWSTR b)
{
    WCHAR buf[512] = {0};
    if (SUCCEEDED(StringCchPrintfW(buf, _countof(buf), fmt, a ? a : L"", b ? b : L"")))
        LogMessage(hInstall, buf);
}

// ============================ Property Helpers ================================

/**
 * GetMsiPropAlloc
 * ---------------
 * Reads an MSI property into a heap buffer (LocalAlloc). Caller must LocalFree().
 * Returns empty string if property exists but empty, or NULL on error.
 */
static wchar_t* GetMsiPropAlloc(MSIHANDLE h, LPCWSTR name)
{
    if (!name) return NULL;
    DWORD cch = 0;
    UINT rc = MsiGetPropertyW(h, name, L"", &cch);
    if (rc == ERROR_MORE_DATA) cch++; // include NUL
    if (cch == 0) cch = 1;
    wchar_t* buf = (wchar_t*)LocalAlloc(LPTR, cch * sizeof(wchar_t));
    if (!buf) return NULL;
    rc = MsiGetPropertyW(h, name, buf, &cch);
    if (rc != ERROR_SUCCESS) { LocalFree(buf); return NULL; }
    return buf; // may be L""
}

static void SetMsiPropBool(MSIHANDLE h, LPCWSTR name, BOOL val)
{
    MsiSetPropertyW(h, name, val ? L"1" : L"0");
}

static void SetMsiPropMsg(MSIHANDLE h, LPCWSTR name, LPCWSTR msg)
{
    MsiSetPropertyW(h, name, msg ? msg : L"");
}

// ============================ String Utilities ================================

/**
 * WideToUtf8Alloc
 * ---------------
 * Converts a UTF-16 wide string to UTF-8. Uses process heap. Caller must
 * HeapFree(GetProcessHeap(), 0, ptr).
 */
static char* WideToUtf8Alloc(const wchar_t* ws)
{
    if (!ws) return NULL;
    int cb = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (cb <= 0) return NULL;
    char* s = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb);
    if (!s) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, cb, NULL, NULL)) {
        HeapFree(GetProcessHeap(), 0, s);
        return NULL;
    }
    return s; // NUL-terminated
}

// ============================= HTTP Functions =================================

/**
 * HttpPostJsonEchoContainsSecret
 * ------------------------------
 * Sends an HTTP/HTTPS POST to kHost:kPort/kPath with UTF-8 JSON body, then checks:
 *   - HTTP 200
 *   - response body contains 'secretUtf8' substring (demo check)
 *
 * Note: This is a simple validation. For production, parse the JSON response properly.
 */
static BOOL HttpPostJsonEchoContainsSecret(
    const char* jsonUtf8,
    DWORD jsonLen,
    const char* secretUtf8)
{
    BOOL ok = FALSE;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    hSession = WinHttpOpen(L"OneGovSetup/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS,
                           0 /* synchronous */);
    if (!hSession) goto done;

    hConnect = WinHttpConnect(hSession, kHost, kPort, 0);
    if (!hConnect) goto done;

    hRequest = WinHttpOpenRequest(hConnect, L"POST", kPath,
                                  NULL /* HTTP version */,
                                  WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  kRequestFlags /* 0=HTTP, SECURE=HTTPS */);
    if (!hRequest) goto done;

    // Keep UI responsive
    WinHttpSetTimeouts(hRequest, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    // Headers + body
    LPCWSTR hdrs = L"Content-Type: application/json; charset=utf-8";
    BOOL b = WinHttpSendRequest(hRequest, hdrs, (DWORD)-1,
                                (LPVOID)jsonUtf8, jsonLen,
                                jsonLen, 0);
    if (!b) goto done;

    if (!WinHttpReceiveResponse(hRequest, NULL)) goto done;

    // Status code
    DWORD status = 0, slen = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &slen, WINHTTP_NO_HEADER_INDEX))
        goto done;

    // Read response body
    char* body = NULL;
    DWORD total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (!avail) break;

        char* nb = (char*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      body, total + avail + 1);
        if (!nb) { if (body) HeapFree(GetProcessHeap(), 0, body); body = NULL; break; }
        body = nb;

        DWORD read = 0;
        if (!WinHttpReadData(hRequest, body + total, avail, &read)) {
            HeapFree(GetProcessHeap(), 0, body);
            body = NULL;
            break;
        }
        total += read;
        body[total] = '\0';
    }

    if (status == 200 && body && secretUtf8 && *secretUtf8) {
        if (strstr(body, secretUtf8) != NULL) ok = TRUE;
    }

    if (body) HeapFree(GetProcessHeap(), 0, body);

done:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return ok;
}

// ============================ Custom Action Entry =============================

/**
 * ValidateAgent
 * -------------
 * MSI Custom Action entry point (exported).
 * Returns ERROR_SUCCESS; result communicated via REST_OK/REST_MSG properties.
 */
UINT __stdcall ValidateAgent(MSIHANDLE hInstall)
{
    // Read properties from MSI
    wchar_t* hostW = GetMsiPropAlloc(hInstall, L"HOSTNAME");
    wchar_t* ipW   = GetMsiPropAlloc(hInstall, L"IPADDRESS");
    wchar_t* secW  = GetMsiPropAlloc(hInstall, L"SECRETKEY");

    // Log inputs (SECRETKEY is public in your case, so log as-is)
    LogFmt(hInstall, L"[ValidateAgent] HOSTNAME='%s'", hostW, NULL);
    LogFmt(hInstall, L"[ValidateAgent] IPADDRESS='%s'", ipW,  NULL);
    LogFmt(hInstall, L"[ValidateAgent] SECRETKEY='%s'", secW, NULL);

    // Basic local validation
    if (!hostW || !*hostW || !ipW || !*ipW || !secW || !*secW) {
        LogMessage(hInstall, L"[ValidateAgent] Missing Host/IP/Secret; aborting.");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Missing Host/IP/Secret.");
        goto cleanup;
    }

    // Build JSON payload (wide)
    wchar_t jsonW[JSON_W_CAPACITY] = {0};
    HRESULT hr = StringCchPrintfW(jsonW, _countof(jsonW),
        L"{\"hostname\":\"%s\",\"ip\":\"%s\",\"secret\":\"%s\"}",
        hostW, ipW, secW);
    if (FAILED(hr)) {
        LogMessage(hInstall, L"[ValidateAgent] Failed to format JSON payload.");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Failed to build JSON.");
        goto cleanup;
    }

    // Convert to UTF-8 for request body
    char* jsonUtf8 = WideToUtf8Alloc(jsonW);
    char* secUtf8  = WideToUtf8Alloc(secW);
    if (!jsonUtf8 || !secUtf8) {
        if (jsonUtf8) HeapFree(GetProcessHeap(), 0, jsonUtf8);
        if (secUtf8)  HeapFree(GetProcessHeap(), 0, secUtf8);
        LogMessage(hInstall, L"[ValidateAgent] UTF-8 conversion failed.");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
        goto cleanup;
    }

    // Log exact URL
    WCHAR fullUrl[256] = {0};
    StringCchPrintfW(fullUrl, _countof(fullUrl), L"http%s://%s:%u%s",
        (kRequestFlags == REQUEST_FLAGS_HTTPS) ? L"s" : L"",
        kHost, (unsigned)kPort, kPath);
    LogFmt(hInstall, L"[ValidateAgent] POST %s", fullUrl, NULL);

    // Call
    {
        BOOL ok = HttpPostJsonEchoContainsSecret(jsonUtf8, (DWORD)strlen(jsonUtf8), secUtf8);

        HeapFree(GetProcessHeap(), 0, jsonUtf8);
        HeapFree(GetProcessHeap(), 0, secUtf8);

        if (ok) {
            LogMessage(hInstall, L"[ValidateAgent] Validation SUCCESS.");
            SetMsiPropBool(hInstall, L"REST_OK", TRUE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"Validated.");
        } else {
            LogMessage(hInstall, L"[ValidateAgent] Validation FAILED.");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"Server validation failed.");
        }
    }

cleanup:
    if (hostW) LocalFree(hostW);
    if (ipW)   LocalFree(ipW);
    if (secW)  LocalFree(secW);
    return ERROR_SUCCESS; // never hard-fail the UI
}

/**
 * DllMain
 * -------
 * Optional: disable thread notifications to reduce overhead.
 */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}

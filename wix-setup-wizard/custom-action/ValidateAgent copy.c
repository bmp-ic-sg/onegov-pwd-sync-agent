// =============================================================================
//  ONEGOV PASSWORD AGENT - ValidateAgent.c   (C / x64)  [VERBOSE LOGGING]
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

// ------------------------------ DEV SWITCHES ---------------------------------
// Set to 1 to log first bytes of the HTTP response body (utf-8 -> utf-16)
#define CA_LOG_BODY_SNIPPET      1
#define CA_BODY_SNIPPET_MAX_UTF8 256  // bytes

// Set to 1 ONLY in lab/test if you want to ignore bad TLS (self-signed etc.)
#define CA_ALLOW_INSECURE_TLS    0
// -----------------------------------------------------------------------------

// ------------------------------------------------------------
// Constants
// ------------------------------------------------------------
#define JSON_W_CAPACITY 1024
static const DWORD kTimeoutMs = 7000;

// ------------------------------------------------------------
// GLOBAL timing anchor (set at CA start) for [t=...ms] stamps
// ------------------------------------------------------------
static ULONGLONG g_t0 = 0;

static void NowStamp(WCHAR* out, size_t cch)
{
    if (!out || cch == 0) return;
    ULONGLONG t = GetTickCount64() - g_t0;
    StringCchPrintfW(out, cch, L"[t=%llu ms] ", (unsigned long long)t);
}

// ---- single-file dev logger: %TEMP%\OneGovCA.log ----
static HANDLE g_LogMutex = NULL;

static void FileLogOne_Init(void)
{
    if (!g_LogMutex)
        g_LogMutex = CreateMutexW(NULL, FALSE, L"Global\\OneGovCA-Log"); // cross-process mutex
}

static void FileLogOne(LPCWSTR text)
{
    if (!text || !*text) return;

    FileLogOne_Init();
    if (g_LogMutex) WaitForSingleObject(g_LogMutex, 5000);

    WCHAR dir[MAX_PATH], file[MAX_PATH];
    if (!GetTempPathW(_countof(dir), dir)) goto done;
    StringCchPrintfW(file, _countof(file), L"%sOneGovCA.log", dir);

    HANDLE h = CreateFileW(file,
                           FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) goto done;

    // Simple rotation: truncate if > 1 MiB
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > (1LL << 20)) {
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        SetEndOfFile(h);
    }

    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR line[1600];
    StringCchPrintfW(line, _countof(line),
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear, st.wMonth, st.wDay,  // <-- fixed here
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        text);

    DWORD n;
    WriteFile(h, line, (DWORD)(lstrlenW(line) * sizeof(WCHAR)), &n, NULL);
    CloseHandle(h);

done:
    if (g_LogMutex) ReleaseMutex(g_LogMutex);
}


// ------------------------------------------------------------
// MSI LOGGING HELPERS  (WARNING so they always appear in /l*vx)
// ------------------------------------------------------------
static void LogMessage(MSIHANDLE hInstall, LPCWSTR text)
{
    if (!text) return;
    FileLogOne(text); // <— always write to %TEMP%\OneGovCA-<pid>.log

    MSIHANDLE hRec = MsiCreateRecord(1);
    if (!hRec) return;
    MsiRecordSetStringW(hRec, 0, L"[1]");
    MsiRecordSetStringW(hRec, 1, text);
    MsiProcessMessage(hInstall, INSTALLMESSAGE_WARNING, hRec);
    MsiCloseHandle(hRec);
}


// printf-style helpers that prepend the time stamp
static void LogFmt1(MSIHANDLE hInstall, LPCWSTR fmt, LPCWSTR a)
{
    WCHAR buf[768];
    WCHAR stamp[64]; NowStamp(stamp, _countof(stamp));
    if (SUCCEEDED(StringCchPrintfW(buf, _countof(buf), L"%s" L"%s",
                                   stamp, L"")))
    {
        WCHAR body[512];
        if (SUCCEEDED(StringCchPrintfW(body, _countof(body), fmt, a ? a : L"")))
        {
            StringCchCatW(buf, _countof(buf), body);
            LogMessage(hInstall, buf + lstrlenW(stamp)); // stamp added again inside LogMessage, so just send body
        }
    }
}

static void LogFmt2(MSIHANDLE hInstall, LPCWSTR fmt, LPCWSTR a, LPCWSTR b)
{
    WCHAR body[768];
    if (SUCCEEDED(StringCchPrintfW(body, _countof(body), fmt,
                                   a ? a : L"", b ? b : L"")))
        LogMessage(hInstall, body);
}

// Rich WinHTTP error logging
static void LogLastError(MSIHANDLE hInstall, LPCWSTR where)
{
    DWORD err = GetLastError();
    WCHAR sys[512] = L"";
    DWORD got = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, err, 0, sys, _countof(sys), NULL);
    if (!got) StringCchCopyW(sys, _countof(sys), L"(no message)");
    WCHAR msg[768];
    StringCchPrintfW(msg, _countof(msg), L"[ERR] %s (GetLastError=%lu) %s",
                     where, (unsigned long)err, sys);
    LogMessage(hInstall, msg);
}

// ------------------------------------------------------------
// UTIL: mask secret for logs (keep first/last 2 chars)
// ------------------------------------------------------------
static void MaskSecret(const wchar_t* src, wchar_t* dst, size_t cchDst)
{
    if (!dst || cchDst == 0) return;
    if (!src || !*src) { StringCchCopyW(dst, cchDst, L"(empty)"); return; }

    size_t n = 0; StringCchLengthW(src, STRSAFE_MAX_CCH, &n);
    if (n <= 4) { StringCchCopyW(dst, cchDst, L"****"); return; }

    WCHAR tmp[256];
    size_t keep = 2;
    size_t mask = n - 2 * keep;
    if (mask > 200) mask = 200; // cap mask length for log
    StringCchCopyNW(tmp, _countof(tmp), src, keep);
    StringCchCatW(tmp, _countof(tmp), L"****");
    StringCchCatW(tmp, _countof(tmp), src + (n - keep));
    StringCchCopyW(dst, cchDst, tmp);
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
    UINT rc = MsiSetPropertyW(h, name, val ? L"1" : L"0");
    if (rc != ERROR_SUCCESS)
    {
        WCHAR m[256]; StringCchPrintfW(m, _countof(m),
            L"[WARN] MsiSetProperty(%s) failed rc=%u", name, rc);
        LogMessage(h, m);
    }
}

static void SetMsiPropMsg(MSIHANDLE h, LPCWSTR name, LPCWSTR msg)
{
    UINT rc = MsiSetPropertyW(h, name, msg ? msg : L"");
    if (rc != ERROR_SUCCESS)
    {
        WCHAR m[256]; StringCchPrintfW(m, _countof(m),
            L"[WARN] MsiSetProperty(%s,msg) failed rc=%u", name, rc);
        LogMessage(h, m);
    }
}

// ------------------------------------------------------------
// STRING UTILITY
// ------------------------------------------------------------
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

// Small helper to allocate a UTF-8 copy (used for reqres demo payload)
static char* DupUtf8(const char* s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (DWORD)n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

// ------------------------------------------------------------
// HTTP POST (generic): POST UTF-8 JSON to parsed URL  [S5 flow]
// ------------------------------------------------------------
static BOOL HttpPostJson(MSIHANDLE hInstall,
                         LPCWSTR host, INTERNET_PORT port, BOOL secure,
                         LPCWSTR path, const char* jsonUtf8,
                         LPCWSTR apiKeyOpt)  // <— new param
{
    BOOL ok = FALSE;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    if (!host || !*host || !path || !jsonUtf8)
    {
        LogMessage(hInstall, L"[S5.0] HttpPostJson: invalid inputs");
        return FALSE;
    }

    LogMessage(hInstall, L"[S5.1] WinHttpOpen session");
    hSession = WinHttpOpen(L"OneGovSetup/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { LogLastError(hInstall, L"WinHttpOpen"); goto done; }

    LogFmt1(hInstall, L"[S5.2] WinHttpConnect host=%s", host);
    hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) { LogLastError(hInstall, L"WinHttpConnect"); goto done; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    LogFmt1(hInstall, L"[S5.3] WinHttpOpenRequest path=%s", path);
    hRequest = WinHttpOpenRequest(hConnect, L"POST", path,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { LogLastError(hInstall, L"WinHttpOpenRequest"); goto done; }

#if CA_ALLOW_INSECURE_TLS
    if (secure)
    {
        DWORD secFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                              &secFlags, sizeof(secFlags)))
        {
            LogLastError(hInstall, L"WinHttpSetOption(INSECURE_TLS)");
        }
        else
        {
            LogMessage(hInstall, L"[S5.3a] INSECURE TLS allowed (lab only)");
        }
    }
#endif

    LogMessage(hInstall, L"[S5.4] Set timeouts");
    WinHttpSetTimeouts(hRequest, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    // Add our base headers
    LPCWSTR hdrs =
        L"Content-Type: application/json; charset=utf-8\r\n"
        L"Accept: application/json";
    DWORD bodyLen = (DWORD)strlen(jsonUtf8);

    // If API key provided, add x-api-key: <SECRETKEY>
    if (apiKeyOpt && *apiKeyOpt)
    {
        WCHAR hdrLine[512];
        if (SUCCEEDED(StringCchPrintfW(hdrLine, _countof(hdrLine),
                                       L"x-api-key: %s", apiKeyOpt)))
        {
            if (!WinHttpAddRequestHeaders(hRequest, hdrLine, (DWORD)-1,
                                          WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
            {
                LogLastError(hInstall, L"WinHttpAddRequestHeaders(x-api-key)");
            }
            else
            {
                // mask for log
                WCHAR masked[256];
                size_t n = 0; StringCchLengthW(apiKeyOpt, STRSAFE_MAX_CCH, &n);
                if (n <= 4) StringCchCopyW(masked, _countof(masked), L"****");
                else {
                    StringCchCopyNW(masked, _countof(masked), apiKeyOpt, 2);
                    StringCchCatW(masked, _countof(masked), L"****");
                    StringCchCatW(masked, _countof(masked), apiKeyOpt + (n - 2));
                }
                LogFmt1(hInstall, L"[S5.h] Added x-api-key=%s", masked);
            }
        }
    }

    WCHAR m[128];
    StringCchPrintfW(m, _countof(m), L"[S5.5] SendRequest bodyLen=%lu", (unsigned long)bodyLen);
    LogMessage(hInstall, m);

    if (!WinHttpSendRequest(hRequest, hdrs, (DWORD)-1,
                            (LPVOID)jsonUtf8, bodyLen,
                            bodyLen, 0))
    {
        LogLastError(hInstall, L"WinHttpSendRequest");
        goto done;
    }

    LogMessage(hInstall, L"[S5.6] ReceiveResponse");
    if (!WinHttpReceiveResponse(hRequest, NULL))
    {
        LogLastError(hInstall, L"WinHttpReceiveResponse");
        goto done;
    }

    // --- Status code ---
    DWORD status = 0, slen = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &slen, WINHTTP_NO_HEADER_INDEX))
    {
        LogLastError(hInstall, L"WinHttpQueryHeaders(status)");
        goto done;
    }
    WCHAR statusMsg[64];
    StringCchPrintfW(statusMsg, _countof(statusMsg), L"[S5.7] HTTP status=%lu", (unsigned long)status);
    LogMessage(hInstall, statusMsg);

    // Helpful header logging
    {
        WCHAR ct[256]; DWORD ctsz = sizeof(ct);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_TYPE,
                                WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctsz, WINHTTP_NO_HEADER_INDEX))
            LogFmt1(hInstall, L"[S5.h] Content-Type=%s", ct);
    }

#if CA_LOG_BODY_SNIPPET
    // Read a small snippet of body for debugging
    DWORD total = 0;
    DWORD toReadTotal = CA_BODY_SNIPPET_MAX_UTF8;
    char  snippet[CA_BODY_SNIPPET_MAX_UTF8 + 1];
    ZeroMemory(snippet, sizeof(snippet));

    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { LogLastError(hInstall, L"WinHttpQueryDataAvailable"); break; }
        if (!avail) break;

        DWORD chunk = avail;
        if (chunk > toReadTotal - total) chunk = toReadTotal - total;
        if (chunk == 0) break;

        DWORD read = 0;
        if (!WinHttpReadData(hRequest, snippet + total, chunk, &read))
        {
            LogLastError(hInstall, L"WinHttpReadData");
            break;
        }
        total += read;
        if (total >= toReadTotal) break;
    }
    snippet[total] = '\0';

    WCHAR bodyW[512];
    int need = MultiByteToWideChar(CP_UTF8, 0, snippet, -1, NULL, 0);
    if (need > 0 && need < (int)_countof(bodyW))
    {
        MultiByteToWideChar(CP_UTF8, 0, snippet, -1, bodyW, _countof(bodyW));
        LogFmt1(hInstall, L"[S5.8] Body(snippet)=%s", bodyW);
    }
    else
    {
        LogMessage(hInstall, L"[S5.8] Body(snippet)=(unavailable or too large)");
    }
#endif

    if (status == 200) ok = TRUE;

done:
    if (hRequest) { LogMessage(hInstall, L"[S5.x] Close handle: request"); WinHttpCloseHandle(hRequest); }
    if (hConnect) { LogMessage(hInstall, L"[S5.x] Close handle: connect"); WinHttpCloseHandle(hConnect); }
    if (hSession) { LogMessage(hInstall, L"[S5.x] Close handle: session"); WinHttpCloseHandle(hSession); }
    return ok;
}

// ------------------------------------------------------------
// URL PARSER: use WinHttpCrackUrl on URLHOST  [S3 flow]
// ------------------------------------------------------------
static BOOL ParseUrl(LPCWSTR url,
                     WCHAR hostOut[256],
                     WCHAR pathOut[512],
                     INTERNET_PORT* portOut,
                     BOOL* secureOut)
{
    if (!url || !*url) return FALSE;

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);

    WCHAR host[256] = {0};
    WCHAR path[512] = {0};

    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(url, 0, 0, &uc))
        return FALSE;

    if (!host[0]) return FALSE;

    INTERNET_PORT port = uc.nPort;
    BOOL secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    if (!path[0]) StringCchCopyW(path, _countof(path), L"/");

    StringCchCopyW(hostOut, 256, host);
    StringCchCopyW(pathOut, 512, path);
    if (portOut) *portOut = port ? port : (secure ? 443 : 80);
    if (secureOut) *secureOut = secure;

    return TRUE;
}

// ------------------------------------------------------------
// CUSTOM ACTION ENTRY POINT  [S1..S7 flow]
// ------------------------------------------------------------
UINT __stdcall ValidateAgent(MSIHANDLE hInstall)
{
    //g_t0 = GetTickCount64();
    LogMessage(hInstall, L"[S1.0] ValidateAgent() begin");

    // S1.1 — Read MSI properties (inputs)
    wchar_t* urlW  = GetMsiPropAlloc(hInstall, L"URLHOST");
    wchar_t* secW  = GetMsiPropAlloc(hInstall, L"SECRETKEY");

    WCHAR masked[256]; MaskSecret(secW, masked, _countof(masked));
    LogFmt1(hInstall, L"[S1.1] URLHOST='%s'",  urlW);
    LogFmt1(hInstall, L"[S1.1] SECRETKEY(masked)='%s'",  masked);

    // S2.0 — Validate required inputs
    if (!urlW || !*urlW || !hostW || !*hostW || !ipW || !*ipW || !secW || !*secW)
    {
        LogMessage(hInstall, L"[S2.0] Missing URL/Host/IP/Secret");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Missing URL/Host/IP/Secret.");
        goto cleanup;
    }
    LogMessage(hInstall, L"[S2.1] Input validation passed");

    // S3.0 — Parse URLHOST
    WCHAR srvHost[256] = {0};
    WCHAR srvPath[512] = {0};
    INTERNET_PORT srvPort = 0;
    BOOL srvSecure = FALSE;

    LogMessage(hInstall, L"[S3.0] Parse URLHOST");
    if (!ParseUrl(urlW, srvHost, srvPath, &srvPort, &srvSecure))
    {
        LogMessage(hInstall, L"[S3.1] Invalid URLHOST (WinHttpCrackUrl failed)");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Invalid URLHOST.");
        goto cleanup;
    }

    WCHAR fullUrl[256 + 512];
    StringCchPrintfW(fullUrl, _countof(fullUrl),
                     L"%s://%s:%u%s",
                     srvSecure ? L"https" : L"http",
                     srvHost, (unsigned)srvPort, srvPath);
    LogFmt2(hInstall, L"[S3.2] Target host=%s path=%s", srvHost, srvPath);
    LogFmt1(hInstall, L"[S3.3] POST %s", fullUrl);

    // S4.0 — Build JSON body
/*     wchar_t jsonW[JSON_W_CAPACITY];
    LogMessage(hInstall, L"[S4.0] Build JSON");
    if (FAILED(StringCchPrintfW(jsonW, _countof(jsonW),
                                L"{\"hostname\":\"%s\",\"ip\":\"%s\",\"secret\":\"%s\"}",
                                hostW, ipW, secW)))
    {
        LogMessage(hInstall, L"[S4.1] Failed to format JSON");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Failed to build JSON.");
        goto cleanup;
    }

    LogMessage(hInstall, L"[S4.2] Convert JSON to UTF-8");
    char* jsonUtf8 = WideToUtf8Alloc(jsonW);
    if (!jsonUtf8)
    {
        LogMessage(hInstall, L"[S4.3] UTF-8 conversion failed");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
        goto cleanup;
    } */

    BOOL  isReqresDemo = (_wcsicmp(srvHost, L"reqres.in") == 0) && (wcsstr(srvPath, L"/api/login") != NULL);
    char* jsonUtf8     = NULL;

    if (isReqresDemo)
    {
        // DEMO mode
            LogMessage(hInstall, L"[S4.0] DEMO: reqres.in payload (email=eve.holt@reqres.in, password=cityslicka)");

            wchar_t jsonW[JSON_W_CAPACITY];
            if (FAILED(StringCchPrintfW(jsonW, _countof(jsonW),
                L"{\"email\":\"eve.holt@reqres.in",\"password\":\"cityslicka\"}",
                (hostW && *hostW) ? hostW : L"",
                (ipW   && *ipW)   ? ipW   : L"")))
            {
                LogMessage(hInstall, L"[S4.1] Failed to format demo JSON");
                SetMsiPropBool(hInstall, L"REST_OK", FALSE);
                SetMsiPropMsg(hInstall, L"REST_MSG", L"Failed to build demo JSON.");
                goto cleanup;
            }

            jsonUtf8 = WideToUtf8Alloc(jsonW);
            if (!jsonUtf8)
            {
                LogMessage(hInstall, L"[S4.3] UTF-8 conversion failed (demo JSON)");
                SetMsiPropBool(hInstall, L"REST_OK", FALSE);
                SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
                goto cleanup;
            }
    }
    else
    {
        // NORMAL: build JSON from inputs
        wchar_t jsonW[1024];
        LogMessage(hInstall, L"[S4.0] Build JSON");
        if (FAILED(StringCchPrintfW(jsonW, _countof(jsonW),
                                    L"{\"hostname\":\"%s\",\"ip\":\"%s\",\"secret\":\"%s\"}",
                                    hostW, ipW, secW)))
        {
            LogMessage(hInstall, L"[S4.1] Failed to format JSON");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"Failed to build JSON.");
            goto cleanup;
        }

        LogMessage(hInstall, L"[S4.2] Convert JSON to UTF-8");
        jsonUtf8 = WideToUtf8Alloc(jsonW);
        if (!jsonUtf8)
        {
            LogMessage(hInstall, L"[S4.3] UTF-8 conversion failed");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
            goto cleanup;
        }
    }

    // S5.x — HTTP POST
    LogMessage(hInstall, L"[S5.0] HTTP POST begin");
    {
        BOOL ok = HttpPostJson(hInstall, srvHost, (INTERNET_PORT)srvPort, srvSecure, srvPath, jsonUtf8, secW);
        HeapFree(GetProcessHeap(), 0, jsonUtf8);

        // S6.0 — Set MSI properties from result
        if (ok)
        {
            LogMessage(hInstall, L"[S6.0] Validation SUCCESS");
            SetMsiPropBool(hInstall, L"REST_OK", TRUE);
            //SetMsiPropMsg(hInstall, L"REST_MSG", L"Validated.");
            SetMsiPropMsg(hInstall, L"REST_MSG", isReqresDemo ? L"Validated via reqres.in demo." : L"Validated.");
        }
        else
        {
            LogMessage(hInstall, L"[S6.1] Validation FAILED");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            //SetMsiPropMsg(hInstall, L"REST_MSG", L"Server validation failed.");
            SetMsiPropMsg(hInstall, L"REST_MSG", isReqresDemo ? L"reqres.in demo call failed." : L"Server validation failed.");
        }
    }

cleanup:
    // S7.x — Cleanup and exit
    if (urlW)  { LocalFree(urlW);  LogMessage(hInstall, L"[S7.1] Free URLHOST"); }
    if (secW)  { LocalFree(secW);  LogMessage(hInstall, L"[S7.4] Free SECRETKEY"); }

    LogMessage(hInstall, L"[S7.9] ValidateAgent() end");
    return ERROR_SUCCESS; // never abort MSI
} 
 

// ------------------------------------------------------------
// DLL ENTRY POINT
// ------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID lp)
{
    UNREFERENCED_PARAMETER(lp);
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(h);
    return TRUE;
}

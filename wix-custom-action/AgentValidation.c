// =============================================================================
//  ONEGOV PASSWORD AGENT — AgentValidation.c (x64, MSI Custom Action)
// =============================================================================
//
// Purpose
//   Validate OneGov connection settings at install-time. Posts a small JSON
//   payload to the configured OneGov IDM URL and reports success/failure via
//   MSI properties (REST_OK, REST_MSG).
//
// Key Features:
//   • Reads MSI properties: IDMURL (server URL), APIKEY (secret)
//   • Parses the URL with WinHttpCrackUrl (host/port/path/https)
//   • Sends HTTP POST (WinHTTP) with a JSON body
//   • On success → REST_OK=1, REST_MSG="Validated."
//     On failure → REST_OK=0 with a friendly guidance message
//   • Logs to both the MSI log (as WARNING for visibility) and
//     %TEMP%\OneGovCA.log with simple rotation
//
// MSI properties
//   Input : IDMURL, APIKEY
//   Output: REST_OK (1/0), REST_MSG (string)
//
// Dependencies
//   WinHTTP (winhttp.lib), MSI (msi.lib), StrSafe (strsafe.h)
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

#define CA_LOG_BODY_SNIPPET      1    // 1 = log first bytes of response body
#define CA_BODY_SNIPPET_MAX_UTF8 256  // max UTF-8 bytes to capture
#define CA_ALLOW_INSECURE_TLS    0    // 1 = ignore invalid TLS (LAB ONLY)

// -----------------------------------------------------------------------------
// Constants / globals
// -----------------------------------------------------------------------------
#define JSON_W_CAPACITY 1024
static const DWORD kTimeoutMs = 7000;

static ULONGLONG g_t0 = 0;
static HANDLE g_LogMutex = NULL;

// Optional time-stamp helper
static void NowStamp(WCHAR* out, size_t cch)
{
    if (!out || cch == 0) return;
    ULONGLONG t = GetTickCount64() - g_t0;
    StringCchPrintfW(out, cch, L"[t=%llu ms] ", (unsigned long long)t);
}

// -----------------------------------------------------------------------------
// Logging helpers — file: %TEMP%\OneGovCA.log + MSI log (WARNING)
// -----------------------------------------------------------------------------
static void FileLog_Init(void)
{
    if (!g_LogMutex)
        g_LogMutex = CreateMutexW(NULL, FALSE, L"Global\\OneGovCA-Log");
}

static void WriteLog(LPCWSTR text)
{
    if (!text || !*text) return;

    FileLog_Init();
    if (g_LogMutex) WaitForSingleObject(g_LogMutex, 5000);

    WCHAR dir[MAX_PATH], file[MAX_PATH];
    if (!GetTempPathW(_countof(dir), dir)) return;
    StringCchPrintfW(file, _countof(file), L"%sOneGovCA.log", dir);

    HANDLE h = CreateFileW(file, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { ReleaseMutex(g_LogMutex); return; }

    // Truncate if file grows beyond 1 MiB
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > (1LL << 20)) {
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        SetEndOfFile(h);
    }

    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR line[1600];
    StringCchPrintfW(line, _countof(line),
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        text);

    DWORD n;
    WriteFile(h, line, (DWORD)(lstrlenW(line) * sizeof(WCHAR)), &n, NULL);
    CloseHandle(h);

    ReleaseMutex(g_LogMutex);
}

// Strip leading step-tags like "[S7.9] " from messages
static LPCWSTR StripStepTag(LPCWSTR s)
{
    if (!s || s[0] != L'[') return s;
    if (s[1] != L'S' && s[1] != L's') return s;
    const wchar_t* p = wcschr(s, L']');
    if (!p) return s;
    if (p[1] == L' ') return p + 2; // skip "] "
    return p + 1;                    // skip "]"
}

static void LogMessage(MSIHANDLE hInstall, LPCWSTR text)
{
    if (!text) return;

    // clean any step-tag if present
    LPCWSTR cleaned = StripStepTag(text);

    WriteLog(cleaned);

    MSIHANDLE hRec = MsiCreateRecord(1);
    if (!hRec) return;
    MsiRecordSetStringW(hRec, 0, L"[1]");
    MsiRecordSetStringW(hRec, 1, cleaned);
    MsiProcessMessage(hInstall, INSTALLMESSAGE_WARNING, hRec);
    MsiCloseHandle(hRec);
}

static void LogFormat1(MSIHANDLE hInstall, LPCWSTR fmt, LPCWSTR a)
{
    WCHAR body[512];
    if (SUCCEEDED(StringCchPrintfW(body, _countof(body), fmt, a ? a : L"")))
        LogMessage(hInstall, body);
}

static void LogFormat2(MSIHANDLE hInstall, LPCWSTR fmt, LPCWSTR a, LPCWSTR b)
{
    WCHAR body[768];
    if (SUCCEEDED(StringCchPrintfW(body, _countof(body), fmt,
                                   a ? a : L"", b ? b : L"")))
        LogMessage(hInstall, body);
}

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

// -----------------------------------------------------------------------------
// Secret masking utility (show first/last 2 chars only)
// -----------------------------------------------------------------------------
static void MaskSecret(const wchar_t* src, wchar_t* dst, size_t cchDst)
{
    if (!dst || cchDst == 0) return;
    if (!src || !*src) { StringCchCopyW(dst, cchDst, L"(empty)"); return; }

    size_t n = 0; StringCchLengthW(src, STRSAFE_MAX_CCH, &n);
    if (n <= 4) { StringCchCopyW(dst, cchDst, L"****"); return; }

    WCHAR tmp[256];
    const size_t keep = 2;
    size_t mask = n - 2 * keep;
    if (mask > 200) mask = 200;

    StringCchCopyNW(tmp, _countof(tmp), src, keep);
    StringCchCatW(tmp, _countof(tmp), L"****");
    StringCchCatW(tmp, _countof(tmp), src + (n - keep));
    StringCchCopyW(dst, cchDst, tmp);
}

// -----------------------------------------------------------------------------
// MSI property helpers
// -----------------------------------------------------------------------------
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
    if (rc != ERROR_SUCCESS) { LocalFree(buf); return NULL; }
    return buf; // may be empty
}

static void SetMsiPropBool(MSIHANDLE h, LPCWSTR name, BOOL val)
{
    UINT rc = MsiSetPropertyW(h, name, val ? L"1" : L"0");
    if (rc != ERROR_SUCCESS)
    {
        WCHAR m[256];
        StringCchPrintfW(m, _countof(m),
                         L"[WARN] MsiSetProperty(%s) failed rc=%u", name, rc);
        LogMessage(h, m);
    }
}

static void SetMsiPropMsg(MSIHANDLE h, LPCWSTR name, LPCWSTR msg)
{
    UINT rc = MsiSetPropertyW(h, name, msg ? msg : L"");
    if (rc != ERROR_SUCCESS)
    {
        WCHAR m[256];
        StringCchPrintfW(m, _countof(m),
                         L"[WARN] MsiSetProperty(%s,msg) failed rc=%u", name, rc);
        LogMessage(h, m);
    }
}

// -----------------------------------------------------------------------------
// String utilities
// -----------------------------------------------------------------------------
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

static char* DupUtf8(const char* s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (DWORD)n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

// -----------------------------------------------------------------------------
// HTTP POST (generic): POST UTF-8 JSON to target URL
// -----------------------------------------------------------------------------
static BOOL HttpPostJson(MSIHANDLE hInstall,
                         LPCWSTR host, INTERNET_PORT port, BOOL secure,
                         LPCWSTR path, const char* jsonUtf8,
                         LPCWSTR apiKeyOpt)
{
    BOOL ok = FALSE;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    if (!host || !*host || !path || !jsonUtf8)
    {
        LogMessage(hInstall, L"HttpPostJson: invalid inputs");
        return FALSE;
    }

    LogMessage(hInstall, L"Open WinHTTP session");
    hSession = WinHttpOpen(L"OneGovSetup/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { LogLastError(hInstall, L"WinHttpOpen"); goto done; }

    LogFormat1(hInstall, L"Connect host=%s", host);
    hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) { LogLastError(hInstall, L"WinHttpConnect"); goto done; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    LogFormat1(hInstall, L"OpenRequest path=%s", path);
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
            LogMessage(hInstall, L"INSECURE TLS allowed (LAB ONLY)");
        }
    }
#endif

    LogMessage(hInstall, L"Set timeouts");
    WinHttpSetTimeouts(hRequest, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    // Default headers
    LPCWSTR hdrs =
        L"Content-Type: application/json; charset=utf-8\r\n"
        L"Accept: application/json";
    DWORD bodyLen = (DWORD)strlen(jsonUtf8);

    // Optional x-api-key
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
                WCHAR masked[256];
                MaskSecret(apiKeyOpt, masked, _countof(masked));
                LogFormat1(hInstall, L"Added x-api-key=%s", masked);
            }
        }
    }

    WCHAR m[128];
    StringCchPrintfW(m, _countof(m), L"SendRequest bodyLen=%lu", (unsigned long)bodyLen);
    LogMessage(hInstall, m);

    if (!WinHttpSendRequest(hRequest, hdrs, (DWORD)-1,
                            (LPVOID)jsonUtf8, bodyLen,
                            bodyLen, 0))
    {
        LogLastError(hInstall, L"WinHttpSendRequest");
        goto done;
    }

    LogMessage(hInstall, L"ReceiveResponse");
    if (!WinHttpReceiveResponse(hRequest, NULL))
    {
        LogLastError(hInstall, L"WinHttpReceiveResponse");
        goto done;
    }

    // Status code
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
    StringCchPrintfW(statusMsg, _countof(statusMsg), L"HTTP status=%lu", (unsigned long)status);
    LogMessage(hInstall, statusMsg);

    // Helpful header logging
    {
        WCHAR ct[256]; DWORD ctsz = sizeof(ct);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_TYPE,
                                WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctsz, WINHTTP_NO_HEADER_INDEX))
            LogFormat1(hInstall, L"Content-Type=%s", ct);
    }

#if CA_LOG_BODY_SNIPPET
    // Read a small snippet of the body for debugging
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
        LogFormat1(hInstall, L"Body(snippet)=%s", bodyW);
    }
    else
    {
        LogMessage(hInstall, L"Body(snippet) unavailable or too large");
    }
#endif

    if (status == 200) ok = TRUE;

done:
    if (hRequest) { LogMessage(hInstall, L"Close handle: request"); WinHttpCloseHandle(hRequest); }
    if (hConnect) { LogMessage(hInstall, L"Close handle: connect"); WinHttpCloseHandle(hConnect); }
    if (hSession) { LogMessage(hInstall, L"Close handle: session"); WinHttpCloseHandle(hSession); }
    return ok;
}

// -----------------------------------------------------------------------------
// URL parsing via WinHttpCrackUrl
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Custom Action entry point
// -----------------------------------------------------------------------------
UINT __stdcall AgentValidation(MSIHANDLE hInstall)
{
    // g_t0 = GetTickCount64(); // enable if you want [t=...ms] stamps
    LogMessage(hInstall, L"AgentValidation begin");

    // Read MSI properties (inputs)
    wchar_t* urlW  = GetMsiPropAlloc(hInstall, L"IDMURL");
    wchar_t* secW  = GetMsiPropAlloc(hInstall, L"APIKEY");

    WCHAR masked[256]; MaskSecret(secW, masked, _countof(masked));
    LogFormat1(hInstall, L"IDMURL='%s'", urlW);
    LogFormat1(hInstall, L"APIKEY(masked)='%s'", masked);

    // Parse IDMURL
    WCHAR srvHost[256] = {0};
    WCHAR srvPath[512] = {0};
    INTERNET_PORT srvPort = 0;
    BOOL srvSecure = FALSE;

    LogMessage(hInstall, L"Parse IDMURL");
    if (!ParseUrl(urlW, srvHost, srvPath, &srvPort, &srvSecure))
    {
        LogMessage(hInstall, L"Invalid IDMURL (WinHttpCrackUrl failed)");
        SetMsiPropBool(hInstall, L"REST_OK", FALSE);
        SetMsiPropMsg(hInstall, L"REST_MSG", L"Invalid OneGov IDM Server URL (e.g. https://onegov.azlabs.sg).");
        goto cleanup;
    }

    WCHAR fullUrl[256 + 512];
    StringCchPrintfW(fullUrl, _countof(fullUrl),
                     L"%s://%s:%u%s",
                     srvSecure ? L"https" : L"http",
                     srvHost, (unsigned)srvPort, srvPath);
    LogFormat2(hInstall, L"Target host=%s path=%s", srvHost, srvPath);
    LogFormat1(hInstall, L"POST %s", fullUrl);

    
    // Build JSON body
    // TODO: to remove demo code after OneGov servers start development
    BOOL  isReqresDemo = (_wcsicmp(srvHost, L"reqres.in") == 0) && (wcsstr(srvPath, L"/api/login") != NULL);
    char* jsonUtf8     = NULL;

    if (!isReqresDemo) {
        // Normalize path for OneGov servers → ensure /api/validate is present
        if (srvPath[0] == L'\0' || wcscmp(srvPath, L"/") == 0) {
            StringCchCopyW(srvPath, _countof(srvPath), L"/api/validate");
        } else if (wcsstr(srvPath, L"/api/validate") == NULL) {
            size_t n = wcslen(srvPath);
            if (n > 0 && srvPath[n - 1] == L'/') {
                StringCchCatW(srvPath, _countof(srvPath), L"api/validate");
            } else {
                StringCchCatW(srvPath, _countof(srvPath), L"/api/validate");
            }
        }
        LogFormat1(hInstall, L"Using endpoint path: %s", srvPath);
    }

    if (isReqresDemo)
    {
        LogMessage(hInstall, L"DEMO: reqres.in payload (email=eve.holt@reqres.in, password=cityslicka)");
        static const wchar_t* kReqResJsonW = L"{\"email\":\"eve.holt@reqres.in\",\"password\":\"cityslicka\"}";
        jsonUtf8 = WideToUtf8Alloc(kReqResJsonW);

        if (!jsonUtf8)
        {
            LogMessage(hInstall, L"UTF-8 conversion failed (demo JSON)");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
            goto cleanup;
        }
    }
    else
    {
        wchar_t jsonW[1024];
        LogMessage(hInstall, L"Build JSON");
        if (FAILED(StringCchPrintfW(jsonW, _countof(jsonW),
                                    L"{\"secret\":\"%s\"}", secW)))
        {
            LogMessage(hInstall, L"Failed to format JSON");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"Failed to build JSON.");
            goto cleanup;
        }

        LogMessage(hInstall, L"Convert JSON to UTF-8");
        jsonUtf8 = WideToUtf8Alloc(jsonW);
        if (!jsonUtf8)
        {
            LogMessage(hInstall, L"UTF-8 conversion failed");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"UTF-8 conversion failed.");
            goto cleanup;
        }
    }

    // HTTP POST
    LogMessage(hInstall, L"HTTP POST begin");
    {
        BOOL ok = HttpPostJson(hInstall, srvHost, (INTERNET_PORT)srvPort, srvSecure, srvPath, jsonUtf8, secW);
        HeapFree(GetProcessHeap(), 0, jsonUtf8);

        // Set result properties
        if (ok)
        {
            LogMessage(hInstall, L"Validation success");
            SetMsiPropBool(hInstall, L"REST_OK", TRUE);
            SetMsiPropMsg(hInstall, L"REST_MSG", L"Validated.");
        }
        else
        {
            LogMessage(hInstall, L"Validation failed");
            SetMsiPropBool(hInstall, L"REST_OK", FALSE);
            SetMsiPropMsg(hInstall, L"REST_MSG",
                L"Unable to validate your connection settings. Check the OneGov IDM Server URL, API key, and network, then try again. If the issue persists, contact your administrator.");
        }
    }

cleanup:
    // Cleanup and exit
    if (urlW) { LocalFree(urlW); LogMessage(hInstall, L"Cleanup IDMURL"); }
    if (secW) { LocalFree(secW); LogMessage(hInstall, L"Cleanup SECRETKEY"); }

    LogMessage(hInstall, L"AgentValidation end");
    return ERROR_SUCCESS; // never abort MSI
}

// -----------------------------------------------------------------------------
// DLL entry point
// -----------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID lp)
{
    UNREFERENCED_PARAMETER(lp);
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(h);
    return TRUE;
}

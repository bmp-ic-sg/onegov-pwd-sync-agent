// =============================================================================
//  ONEGOV PASSWORD AGENT CORE
//  Windows Password Watcher (OneGovPasswordAgent-core.dll)
//
//  Purpose:
//    Listens for Windows password changes and forwards details to the
//    OneGov Password Sync Agent to keep systems in sync.
//
//  Safety notes:
//    • Runs inside LSASS (a critical Windows security process).
//      Any stall or crash can impact the whole machine.
//    • Keep logic minimal, fast, and non-blocking; avoid waiting.
//    • Do not log plaintext passwords in production. Demo logging must be disabled.
// =============================================================================

#include <windows.h>
#include <strsafe.h>

#pragma comment(lib, "advapi32.lib")

// ------------------------------------------------------------
// UNICODE_STRING
// Descriptor used by LSASS to pass UserName and NewPassword as UTF-16.
// ------------------------------------------------------------
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((LONG)0x00000000L)
#endif

// ============================================================
// HELPER: LogEvent
//
// Writes a message to the Windows Event Log.
//
// View logs in: Event Viewer → Windows Logs → Application,
// Source: "OneGovPasswordAgent"
// ============================================================
static void LogEvent(LPCWSTR message, LPCWSTR detail){
    HANDLE hEvent = RegisterEventSourceW(NULL, L"OneGovPasswordAgent");

    if (hEvent){
        LPCWSTR msgs[2] = { message, detail };

        ReportEventW(
            hEvent,
            EVENTLOG_INFORMATION_TYPE,  // Information
            0,                          // Category
            0,                          // Event ID (no message file)
            NULL,                       // User SID
            detail ? 2 : 1,             // Line count
            0,                          // Binary data size
            msgs,                       // Strings
            NULL                        // Binary data
        );

        DeregisterEventSource(hEvent);
    }
}

// ============================================================
// HELPER: SendToPipe
//
// Sends a password-change payload to OneGovPwdAgent-service.exe
// via a local named pipe.
//
// Notes:
//  • Minimal work by design—this runs in LSASS.
//  • Converts input from UTF-16 to UTF-8 before writing.
// ============================================================
static void SendToPipe(LPCWSTR message){
    
    if (!message){
        LogEvent(L"SendToPipe: Empty message, skipping", NULL);
        return;
    }

    int need = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        LogEvent(L"SendToPipe: Invalid message, skipping", NULL);
        return;
    }

    CHAR* buf = (CHAR*)LocalAlloc(LPTR, need);
    if (!buf) {
        LogEvent(L"SendToPipe: Out of memory, skipping", NULL);
        return;
    }

    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, need, NULL, NULL);

    HANDLE h = CreateFileW(
        L"\\\\.\\pipe\\OneGovPasswordAgentPipe",
        GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
    );

    if (h != INVALID_HANDLE_VALUE){
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &written, NULL);
        WriteFile(h, "\n", 1, &written, NULL);
        CloseHandle(h);
    }

    LocalFree(buf);
}

// ------------------------------------------------------------
// InitializeChangeNotify
// Called by LSASS when this DLL is loaded. Do only quick setup.
// ------------------------------------------------------------
__declspec(dllexport) BOOLEAN __stdcall InitializeChangeNotify(void) {
    LogEvent(L"OneGovPasswordAgent initialized", NULL);
    SendToPipe(L"OneGovPasswordAgent initialized");
    return TRUE;
}

// ------------------------------------------------------------
// PasswordChangeNotify
// Called by LSASS after a successful password change.
//
// Params:
//   UserName    - Unicode username
//   RelativeId  - User RID (numeric portion of SID)
//   NewPassword - New password (UNICODE_STRING)
//
// WARNING:
//   Heavy work here blocks LSASS. Keep it minimal and non-blocking.
// ------------------------------------------------------------
__declspec(dllexport) LONG __stdcall PasswordChangeNotify( PUNICODE_STRING UserName, 
                                                           ULONG RelativeId, 
                                                           PUNICODE_STRING NewPassword ) {
    // Username
    WCHAR userBuf[256] = L"(unknown)";
    if (UserName && UserName->Buffer && UserName->Length > 0)
    {
        size_t chars = UserName->Length / sizeof(WCHAR);
        if (chars >= _countof(userBuf)) chars = _countof(userBuf) - 1;
        wmemcpy_s(userBuf, _countof(userBuf), UserName->Buffer, chars);
        userBuf[chars] = L'\0';
    }

    // RID string
    WCHAR ridBuf[64];
    StringCchPrintfW(ridBuf, _countof(ridBuf), L"RID=%lu", RelativeId);

    // Password length
    WCHAR lenBuf[64] = L"(no password buffer)";
    if (NewPassword && NewPassword->Buffer)
    {
        size_t pwdChars = NewPassword->Length / sizeof(WCHAR);
        StringCchPrintfW(lenBuf, _countof(lenBuf),
                         L"Password length (chars) = %zu", pwdChars);
    }

    // DEMO ONLY: capture plaintext password (remove for production)
    WCHAR pwdBuff[256] = L"(unknown)";
    if (NewPassword && NewPassword->Buffer && NewPassword->Length > 0)
    {
        size_t chars = NewPassword->Length / sizeof(WCHAR);
        if (chars >= _countof(pwdBuff)) chars = _countof(pwdBuff) - 1;
        wmemcpy_s(pwdBuff, _countof(pwdBuff), NewPassword->Buffer, chars);
        pwdBuff[chars] = L'\0';
    }

    // Log username + RID (and demo password line)
    WCHAR msgWithRid[320];
    StringCchPrintfW(msgWithRid, _countof(msgWithRid), L"%s (%s)", userBuf, ridBuf);
    LogEvent(msgWithRid, pwdBuff);

    // DEMO ONLY: explicit password message
    WCHAR changePwdMsg[256];
    StringCchPrintfW(changePwdMsg, _countof(changePwdMsg),
                     L"User (%s) changed password to '%s'", userBuf, pwdBuff);
    LogEvent(changePwdMsg, NULL);

    // Send JSON payload over the pipe
    // Example: {"User":"Alice","RID":"RID=1001","Length":"Password length (chars) = 12","Password":"MyNewPass"}
    WCHAR pipeMsg[512];
    StringCchPrintfW(
        pipeMsg, _countof(pipeMsg),
        L"{\"User\":\"%s\",\"RID\":\"%s\",\"Length\":\"%s\",\"Password\":\"%s\"}",
        userBuf, ridBuf, lenBuf, pwdBuff
    );
    SendToPipe(pipeMsg);

    // Security cleanup: wipe plaintext from LSASS memory
    if (NewPassword && NewPassword->Buffer && NewPassword->MaximumLength > 0)
    {
        SecureZeroMemory(NewPassword->Buffer, NewPassword->MaximumLength);
    }

    return STATUS_SUCCESS;
}

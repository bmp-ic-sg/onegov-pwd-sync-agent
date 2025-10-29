// =============================================================================
//  ONEGOV PASSWORD AGENT
//  Windows Password Filter (OneGovPwdAgent-core.dll)
//
//  Purpose:
//    This add-on listens for Windows password changes and passes the details
//    to the OneGov Password Sync Agent (the helper app) so other systems can
//    stay in sync.
//
//  What it does:
//    • Writes a simple note to the Windows Event Log (for auditing).
//    • Sends a small text message to our helper app through a private
//      named pipe.
//    • (Optional/demo) Can clear any copies of the password from memory
//      after sending, to reduce the chance of leftovers.
//
//  Important safety notes:
//    • This runs inside LSASS — a core Windows security process.
//      If it stalls or crashes, the whole machine can be affected.
//      That’s why the code is kept short, fast, and avoids waiting.
//    • We avoid detailed error messages here to keep LSASS stable and quiet.
//    • Never log real passwords in production. Demo logging must be disabled.
//
//  TL;DR:
//    Listen for password changes → send password change details → keep moving.
// =============================================================================


#include <windows.h>
#include <strsafe.h>

#pragma comment(lib, "advapi32.lib")


// ------------------------------------------------------------
// UNICODE_STRING (type)
// Simple Windows/NT string descriptor used by LSASS to hand us
// the UserName and NewPassword as UTF-16 text.
// - Length        : bytes currently used (no trailing NUL)
// - MaximumLength : total bytes available in Buffer
// - Buffer        : pointer to the UTF-16 characters
//   (This is just a wrapper; it does NOT own/allocate memory.)
// ------------------------------------------------------------
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

// ------------------------------------------------------------
// STATUS_SUCCESS
// NTSTATUS code meaning “success” (zero).
// ------------------------------------------------------------
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((LONG)0x00000000L)
#endif


// ============================================================
// HELPER: LogEvent
//
// Purpose:
//   - Writes a log message to the Windows Event Log.
//
// How it’s used:
//   - Open the Event Viewer → Windows Logs → Application, 
//     and find the log under source name "OneGovPasswordAgent".
//   - message : the main line of text
//   - detail  : an extra line (can be empty)
//
// Safety:
//   - If Windows’ log isn’t available, it just skips and moves on.
//   - Kept very quick and quiet because this runs inside a critical
//     Windows process (LSASS).
// ============================================================
static void LogEvent(LPCWSTR message, LPCWSTR detail){
    HANDLE hEvent = RegisterEventSourceW(NULL, L"OneGovPasswordAgent");

    // If cannot open the Windows’ log, skip
    if (hEvent){
        LPCWSTR msgs[2] = { message, detail };

        ReportEventW(
            hEvent,
            EVENTLOG_INFORMATION_TYPE,  // Information entry
            0,                          // No category
            0,                          // Generic ID (no message file)
            NULL,                       // No user SID
            detail ? 2 : 1,             // One or two lines
            0,                          // No binary data
            msgs,                       // Text to record
            NULL                        // No raw data
        );

        // Close handle to avoid leaks
        DeregisterEventSource(hEvent);  
    }
}


// ============================================================
// HELPER: SendToPipe
//
// Purpose:
//   To sends the password change request to OneGovPwdAgent-service.exe.
//   The password change details send with named pipe(local transport).
//
// Safety:
//   This code runs inside a very important Windows process. If it slows
//   down or crashes, the whole computer could be affected. So we keep it
//   short, safe, and quiet.
// ============================================================
static void SendToPipe(LPCWSTR message){
    // If message is empty, skip
    if (!message){
        LogEvent(L"SendToPipe: Empty message, skipping", NULL);
        return;
    }

    // Validate the message can be converted to UTF-8 from UTF-16
    int need = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
    if (need <= 0) { 
        LogEvent(L"SendToPipe: Invalid message, skipping", NULL); 
        return; 
    }

    // Calculate the message size, if empty, skip
    CHAR* buf = (CHAR*)LocalAlloc(LPTR, need);
    if (!buf) { 
        LogEvent(L"SendToPipe: Out of memory, skipping", NULL); 
        return;
    }

    // Convert message to UTF-8 
    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, need, NULL, NULL);

    HANDLE h = CreateFileW(
        L"\\\\.\\pipe\\OneGovPasswordPipe",
        GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
    );

    if (h != INVALID_HANDLE_VALUE){
        DWORD written = 0;
        // Send the message
        WriteFile(h, buf, (DWORD)strlen(buf), &written, NULL);
        WriteFile(h, "\n", 1, &written, NULL);
        // Close the pipe connection
        CloseHandle(h);
    }

    // Clean up memory
    LocalFree(buf);
}


// ------------------------------------------------------------
// InitializeChangeNotify
// Purpose:
//   - Called by LSASS whenever it loads this DLL.
//   - Do only quick, safe setup here.
// ------------------------------------------------------------
__declspec(dllexport) BOOLEAN __stdcall InitializeChangeNotify(void) {
    // Log that DLL has been successfully loaded by LSASS
    LogEvent(L"OneGovPasswordAgent initialized", NULL);

    // Notify user-mode service through pipe (if available)
    SendToPipe(L"OneGovPasswordAgent initialized");

    // Return TRUE → indicates DLL successfully initialized
    return TRUE;
}


// ------------------------------------------------------------
// PasswordChangeNotify
// Called by LSASS whenever a password is successfully changed.
// Parameters:
//   UserName    → Unicode username of the account
//   RelativeId  → User’s RID (unique numeric part of SID)
//   NewPassword → The new password (in UNICODE_STRING form)
//
// WARNING:
//   Executing heavy logic here blocks LSASS thread context.
//   Keep all processing minimal and non-blocking.
// ------------------------------------------------------------
__declspec(dllexport) LONG __stdcall PasswordChangeNotify( PUNICODE_STRING UserName, 
                                                           ULONG RelativeId, 
                                                           PUNICODE_STRING NewPassword ) {
    // -------------------------------
    // Capture username
    // -------------------------------
    WCHAR userBuf[256] = L"(unknown)";
    if (UserName && UserName->Buffer && UserName->Length > 0)
    {
        // Convert from counted UNICODE_STRING → null-terminated WCHAR
        size_t chars = UserName->Length / sizeof(WCHAR);
        if (chars >= _countof(userBuf)) chars = _countof(userBuf) - 1;
        wmemcpy_s(userBuf, _countof(userBuf), UserName->Buffer, chars);
        userBuf[chars] = L'\0'; // Ensure proper null termination
    }

    // -------------------------------
    // Convert RID (ULONG) to string
    // -------------------------------
    WCHAR ridBuf[64];
    StringCchPrintfW(ridBuf, _countof(ridBuf), L"RID=%lu", RelativeId);

    // -------------------------------
    // Determine password length
    // -------------------------------
    WCHAR lenBuf[64] = L"(no password buffer)";
    if (NewPassword && NewPassword->Buffer)
    {
        size_t pwdChars = NewPassword->Length / sizeof(WCHAR);
        StringCchPrintfW(lenBuf, _countof(lenBuf),
                         L"Password length (chars) = %zu", pwdChars);
    }

    // -------------------------------
    // Capture plaintext password (demo)
    // NOTE: For demonstration only. Remove in production builds.
    // -------------------------------
    WCHAR pwdBuff[256] = L"(unknown)";
    if (NewPassword && NewPassword->Buffer && NewPassword->Length > 0)
    {
        size_t chars = NewPassword->Length / sizeof(WCHAR);
        if (chars >= _countof(pwdBuff)) chars = _countof(pwdBuff) - 1;
        wmemcpy_s(pwdBuff, _countof(pwdBuff), NewPassword->Buffer, chars);
        pwdBuff[chars] = L'\0';
    }

    // -------------------------------
    // Compose log entry: user + RID
    // -------------------------------
    WCHAR msgWithRid[320];
    StringCchPrintfW(msgWithRid, _countof(msgWithRid),
                     L"%s (%s)", userBuf, ridBuf);

    // Log username + RID (optionally with password — demo only)
    LogEvent(msgWithRid, pwdBuff);

    // ------------------------------------------------------------
    // DEMO ONLY: Log explicit password message to Event Log
    // ------------------------------------------------------------
    WCHAR changePwdMsg[256];
    StringCchPrintfW(
        changePwdMsg, _countof(changePwdMsg),
        L"User (%s) changed password to '%s'",
        userBuf, pwdBuff
    );
    LogEvent(changePwdMsg, NULL);
    // ------------------------------------------------------------

    // -------------------------------
    // Send structured JSON to pipe
    // -------------------------------
    // e.g. {"User":"Alice","RID":"RID=1001","Length":"Password length (chars) = 12","Password":"MyNewPass"}
    WCHAR pipeMsg[512];
    StringCchPrintfW(
        pipeMsg, _countof(pipeMsg),
        L"{\"User\":\"%s\",\"RID\":\"%s\",\"Length\":\"%s\",\"Password\":\"%s\"}",
        userBuf, ridBuf, lenBuf, pwdBuff
    );
    SendToPipe(pipeMsg);

    // -------------------------------
    // Security cleanup
    // -------------------------------
    // Explicitly zero out the NewPassword buffer to remove
    // plaintext from LSASS memory once we're done.
    if (NewPassword && NewPassword->Buffer && NewPassword->MaximumLength > 0)
    {
        SecureZeroMemory(NewPassword->Buffer, NewPassword->MaximumLength);
    }

    // Return NTSTATUS success → signals LSASS the operation succeeded
    return STATUS_SUCCESS;
}

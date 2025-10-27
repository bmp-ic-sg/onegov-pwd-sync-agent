// =============================================================================
//  ONEGOV PASSWORD AGENT
//  Windows Password Filter DLL (OneGovPasswordAgent.dll)
//
//  Purpose:
//    Hooks into LSASS password change notifications and forwards events
//    to both Windows Event Log and a named pipe for user-mode processing.
//    Hooks into LSASS password change notifications to read the user password
//    and send it to OneGov Password Sync Agent Service
//
//  Key Features:
//    • Logs password changes via Event Log for audit
//    • Sends formatted data through named pipe for OneGov Password Sync Agent Service
//    • Supports secure zeroing of password memory(clear the memmory after send to
//      OneGov Password Sync Agent Service)
//
//  NOTE:
//    This module runs inside LSASS (Local Security Authority Subsystem Service).
//    LSASS is a critical Windows process; a crash here forces a system restart.
//    Keep code minimal, non-blocking, and dependency-free.
// =============================================================================

#include <windows.h>
#include <strsafe.h>

// For Event Log and security APIs
#pragma comment(lib, "advapi32.lib")


// ------------------------------------------------------------
// Basic NT-style Unicode string type definition
// ------------------------------------------------------------
// Avoids pulling full NT headers from WDK to stay lightweight.
typedef struct _UNICODE_STRING {
    USHORT Length;         // Length (in bytes) of current string
    USHORT MaximumLength;  // Allocated buffer length
    PWSTR  Buffer;         // Pointer to Unicode characters
} UNICODE_STRING, *PUNICODE_STRING;

// Define STATUS_SUCCESS if not already present (for NTSTATUS return)
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((LONG)0x00000000L)
#endif


// ============================================================
// HELPER: Log to Windows Event Log
// ============================================================
// Called internally to write informational entries under
// the "OneGovPasswordAgent" source name in Event Viewer.
//
// Arguments:
//   message – short description (e.g., “User changed password”)
//   detail  – optional secondary message (can be NULL)
// ============================================================
static void LogEvent(LPCWSTR message, LPCWSTR detail)
{
    // Obtain handle to the “OneGovPasswordAgent” event source
    HANDLE hEvent = RegisterEventSourceW(NULL, L"OneGovPasswordAgent");

    if (hEvent)
    {
        // Create an array of message pointers.
        // If detail exists → use both strings, otherwise only message.
        LPCWSTR msgs[2] = { message, detail };

        // Write entry to Windows Event Log
        ReportEventW(
            hEvent,
            EVENTLOG_INFORMATION_TYPE,  // Log type: Information
            0,                          // Category (unused)
            0,                          // Event ID → use 0 to prevent lookup errors
            NULL,                       // No user SID
            detail ? 2 : 1,             // Number of strings to log
            0,                          // No binary data
            msgs,                       // Pointer array of message strings
            NULL                        // No raw data
        );

        // Always deregister the handle to prevent LSASS leaks
        DeregisterEventSource(hEvent);
    }
    // If hEvent == NULL → Event Log unavailable, silently ignore.
}


// ============================================================
// HELPER: Send message to Named Pipe
// ============================================================
// Used to forward real-time password change data to the user-mode
// service (OneGovPwdAgent-service.exe) for further processing.
//
// Pipe endpoint name: \\.\pipe\OneGovPasswordPipe
// Messages encoded in UTF-8 and newline-delimited.
// ============================================================
static void SendToPipe(LPCWSTR text)
{
    if (!text) return;

    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return; // Conversion not possible, skip silently

    CHAR* buf = (CHAR*)LocalAlloc(LPTR, need);
    if (!buf) return; // Low-memory condition → skip

    // Perform actual conversion UTF-16 → UTF-8
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, need, NULL, NULL);

    // Attempt to connect to existing pipe listener
    HANDLE h = CreateFileW(
        L"\\\\.\\pipe\\OneGovPasswordPipe", // Named pipe path
        GENERIC_WRITE,                     // Write-only access
        0,                                 // No sharing
        NULL,                              // Default security
        OPEN_EXISTING,                     // Must already exist (service listening)
        FILE_ATTRIBUTE_NORMAL,             // Normal file mode
        NULL                               // No template
    );

    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;

        // Write main message body (JSON or text)
        WriteFile(h, buf, (DWORD)strlen(buf), &written, NULL);

        // Append newline to mark message boundary
        WriteFile(h, "\n", 1, &written, NULL);

        // Always close handle to avoid LSASS handle leaks
        CloseHandle(h);
    }

    // Free temporary UTF-8 buffer
    LocalFree(buf);
}


// ============================================================
// REQUIRED EXPORTS
// ============================================================
// The functions below are required by Windows Password Filter API.
// They must be exported with exact names for LSASS to load correctly.
// ============================================================


// ------------------------------------------------------------
// InitializeChangeNotify()
// Invoked once when LSASS loads this DLL.
// Use it to perform lightweight startup initialization.
// ------------------------------------------------------------
__declspec(dllexport) BOOLEAN __stdcall InitializeChangeNotify(void)
{
    // Log that DLL has been successfully loaded by LSASS
    LogEvent(L"OneGovPasswordAgent initialized", NULL);

    // Notify user-mode service through pipe (if available)
    SendToPipe(L"OneGovPasswordAgent initialized");

    // Return TRUE → indicates DLL successfully initialized
    return TRUE;
}


// ------------------------------------------------------------
// PasswordChangeNotify()
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
__declspec(dllexport) LONG __stdcall PasswordChangeNotify(
    PUNICODE_STRING UserName,
    ULONG RelativeId,
    PUNICODE_STRING NewPassword
)
{
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

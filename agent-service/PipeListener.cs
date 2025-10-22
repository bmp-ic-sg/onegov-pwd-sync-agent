// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC - PIPE LISTENER
//
//  Purpose:
//    Listens for password change events sent from the LSASS-side DLL
//    (OneGovPasswordAgent.dll) through a local Named Pipe channel.
//
//  Behavior:
//    • Creates a NamedPipeServerStream ("\\.\pipe\OneGovPasswordPipe")
//    • Waits for connections from the LSASS module
//    • Reads incoming JSON payloads (password change data)
//    • Passes them for processing or forwarding to IDM endpoint
//
//  Design Notes:
//    • Runs asynchronously in a background Task
//    • Safe cancellation using CancellationToken
//    • Tolerates transient errors and auto-restarts after delay
//    • Uses Newtonsoft.Json for parsing (legacy compatible)
// =============================================================================

using System;
using System.IO;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace OneGovPwdAgentSync
{
    /// <summary>
    /// Handles incoming messages from the OneGovPasswordAgent DLL
    /// via Windows Named Pipe communication channel.
    /// </summary>
    public class PipeListener
    {
        // ============================================================
        // PRIVATE FIELDS
        // ============================================================

        // Fixed pipe name (must match the DLL's pipe target)
        private readonly string _pipeName;

        // ============================================================
        // CONSTRUCTOR
        // ============================================================
        public PipeListener()
        {
            // Use a predefined pipe name consistent with native agent DLL
            _pipeName = "OneGovPasswordPipe";
        }

        // ============================================================
        // START()
        // Launches asynchronous listener loop with cancellation token.
        // ============================================================
        public void Start(CancellationToken token)
        {
            LoggerManager.LogFileInfo($"PipeListener started on pipe: {_pipeName}");

            // Run listening loop on a background thread
            Task.Run(() => ListenLoop(token), token);
        }

        // ============================================================
        // LISTEN LOOP
        // Continuously waits for client connections and reads messages.
        // ============================================================
        private async Task ListenLoop(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    // Create a new named pipe instance (single client per instance)
                    using (var server = new NamedPipeServerStream(_pipeName, PipeDirection.In))
                    {
                        // Wait synchronously for LSASS DLL connection
                        server.WaitForConnection();
                        LoggerManager.LogInfo("OneGovPasswordAgent connected to pipe.");

                        // Once connected, wrap the stream in a text reader
                        using (var reader = new StreamReader(server))
                        {
                            // Read the entire content sent by the client
                            string message = await reader.ReadToEndAsync();

                            // Ignore empty or whitespace-only messages
                            if (!string.IsNullOrWhiteSpace(message))
                                ProcessMessage(message);
                        }
                    }
                }
                catch (OperationCanceledException)
                {
                    // Graceful stop requested via CancellationToken
                    LoggerManager.LogError("PipeListener cancellation requested, stopping...");
                    break;
                }
                catch (Exception ex)
                {
                    // Generic catch to ensure listener survives transient failures
                    LoggerManager.LogError($"PipeListener error: {ex.Message}");

                    // Delay to prevent high CPU if continuous errors occur
                    Thread.Sleep(2000);
                }
            }

            LoggerManager.LogFileInfo("PipeListener stopped gracefully.");
        }

        // ============================================================
        // PROCESS MESSAGE
        // Parses and handles a single JSON message from pipe.
        // ============================================================
        private static void ProcessMessage(string json)
        {
            try
            {
                // Attempt to deserialize JSON into strongly-typed object
                var data = JsonConvert.DeserializeObject<PipeMessage>(json);

                if (data == null)
                {
                    LoggerManager.LogWarn($"Received invalid JSON: {json}");
                    return;
                }

                // Compose descriptive log entry for diagnostics
                string logMsg = $"Received password change event: " +
                                $"User={data.User}, RID={data.RID}, " +
                                $"Length={data.Length} chars, Password={data.Password}";

                // Verbose debug log (file + event viewer)
                // ⚠️ Keep disabled in production for security reasons
                LoggerManager.LogDebug(logMsg);

                // -----------------------------------------------------------------
                // TODO (Future Enhancement):
                // 1. Encrypt password before transmission or persistence.
                // 2. Implement API integration with OneGov IDM for password sync.
                // -----------------------------------------------------------------
            }
            catch (JsonException jex)
            {
                // Handles malformed JSON (invalid format)
                LoggerManager.LogError($"JSON parse error: {jex.Message} - Raw: {json}");
            }
            catch (Exception ex)
            {
                // Catches unexpected runtime errors (e.g. IO or threading)
                LoggerManager.LogError($"Pipe message error: {ex.Message}");
            }
        }

        // ============================================================
        // STOP()
        // Signals graceful shutdown of the pipe listener.
        // ============================================================
        public void Stop()
        {
            LoggerManager.LogFileInfo("PipeListener stop requested.");
        }

        // ============================================================
        // INTERNAL MESSAGE STRUCTURE
        // Matches JSON payload format from OneGovPasswordAgent.dll.
        // ============================================================
        private class PipeMessage
        {
            public string User { get; set; }     // Username of account changed
            public string RID { get; set; }      // RID string ("RID=xxxx")
            public string Length { get; set; }   // Password length summary
            public string Password { get; set; } // Plaintext (demo only)
        }
    }
}

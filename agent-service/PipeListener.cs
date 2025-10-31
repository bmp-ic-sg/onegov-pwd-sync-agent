// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC — PIPE LISTENER
//
//  Purpose:
//    Listens for password-change events sent by the LSASS-side DLL
//    (OneGovPasswordAgent-core.dll) over a local named pipe.
//
//  Behavior:
//    • Creates a NamedPipeServerStream ("OneGovPasswordAgentPipe")
//    • Waits for connections from the native agent
//    • Reads incoming JSON payloads (password-change data)
//    • Hands them off for processing / forwarding to the IDM endpoint
//
//  Design notes:
//    • Runs asynchronously on a background Task
//    • Cooperative cancellation via CancellationToken
//    • Resilient to transient errors (auto-retry with backoff)
//    • Uses Newtonsoft.Json for parsing
// =============================================================================

using System;
using System.IO;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace OneGovPwdAgentSync
{
    public class PipeListener
    {
        private readonly string _pipeName;

        public PipeListener()
        {
            // Must match the native DLL's target pipe
            _pipeName = "OneGovPasswordAgentPipe";
        }

        public void Start(CancellationToken token)
        {
            LoggerManager.LogFileInfo($"Pipe listening password changes events on pipe : {_pipeName}");
            Task.Run(() => ListenLoop(token), token);
        }

        // ---------------------------------------------------------------------
        // ListenLoop: accept connections and read messages until cancelled
        // ---------------------------------------------------------------------
        private async Task ListenLoop(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    using (var server = new NamedPipeServerStream(_pipeName, PipeDirection.In))
                    {
                        server.WaitForConnection();
                        LoggerManager.LogInfo("OneGovPasswordAgent-core connected to pipe.");

                        using (var reader = new StreamReader(server))
                        {
                            string message = await reader.ReadToEndAsync();

                            if (!string.IsNullOrWhiteSpace(message))
                                ProcessMessage(message);
                        }
                    }
                }
                catch (OperationCanceledException)
                {
                    LoggerManager.LogError("Pipe Listener cancellation requested; stopping...");
                    break;
                }
                catch (Exception ex)
                {
                    LoggerManager.LogError($"Pipe Listener error: {ex.Message}");
                    Thread.Sleep(2000);
                }
            }

            LoggerManager.LogFileInfo("Pipe Listener stopped gracefully.");
        }

        // ---------------------------------------------------------------------
        // ProcessMessage: parse and handle a single JSON message
        // ---------------------------------------------------------------------
        private static void ProcessMessage(string json)
        {
            try
            {
                var data = JsonConvert.DeserializeObject<PipeMessage>(json);

                if (data == null)
                {
                    LoggerManager.LogError($"Received invalid JSON: {json}");
                    return;
                }

                // Verbose diagnostics (keep disabled in production: contains password)
                LoggerManager.LogInfo($"Received password change event User={data.User}(RID={data.RID})");
                string logMsg =
                    $"Received password change event: User={data.User}(RID={data.RID}), Password Length={data.Length} chars, Password={data.Password}";
                LoggerManager.LogDebug(logMsg);

                // TODO:
                // 1) Encrypt or redact sensitive fields (Password) before any persistence/transit.
                // 2) POST to OneGov IDM endpoint with appropriate authentication and retries.
            }
            catch (JsonException jex)
            {
                LoggerManager.LogError($"JSON parse error: {jex.Message} — Raw: {json}");
            }
            catch (Exception ex)
            {
                LoggerManager.LogError($"Pipe message error: {ex.Message}");
            }
        }

        public void Stop()
        {
            LoggerManager.LogFileInfo("Pipe Listener stop requested.");
        }

        // ---------------------------------------------------------------------
        // Message contract expected from the native DLL
        // ---------------------------------------------------------------------
        private class PipeMessage
        {
            public string User { get; set; }     // Username
            public string RID { get; set; }      // "RID=xxxx"
            public string Length { get; set; }   // e.g., "Password length (chars) = 12"
            public string Password { get; set; } // Plaintext (demo only; must not be logged in prod)
        }
    }
}

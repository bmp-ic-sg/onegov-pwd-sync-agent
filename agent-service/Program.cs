// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC SERVICE
//
//  Purpose:
//    Windows service that hosts the PipeListener to receive password-change
//    notifications from the LSASS-side agent DLL.
//
//  Features:
//    • Runs as a background Windows service ("OneGovPasswordAgentService")
//    • Initializes configuration and logging
//    • Starts and supervises a named-pipe server (PipeListener)
//    • Supports both service and console (debug) modes
//
//  Design notes:
//    • Uses CancellationTokenSource for controlled shutdown
//    • Avoids blocking the Service Control Manager (SCM)
//    • Ensures graceful cleanup and logging on stop
// =============================================================================

using System;
using System.ServiceProcess;
using System.Threading;
using System.Threading.Tasks;

namespace OneGovPwdAgentSync
{
    public class OneGovPwdAgentSyncService : ServiceBase
    {
        private PipeListener _listener;
        private CancellationTokenSource _cts;

        // Event Log source name
        public OneGovPwdAgentSyncService()
        {
            ServiceName = "OneGovPasswordAgentService";
        }

        protected override void OnStart(string[] args)
        {
            try
            {
                // Load configuration and initialize logging
                ConfigurationManager.Load();
                LoggerManager.Initialize();

                LoggerManager.LogFileInfo("=== Starting OneGov Password Agent Service ===");

                _cts = new CancellationTokenSource();
                _listener = new PipeListener();

                // Run listener on a background task
                Task.Run(() => _listener.Start(_cts.Token));
                LoggerManager.LogInfo("OneGov Password Agent Service PipeListener started successfully.");
            }
            catch (Exception ex)
            {
                LoggerManager.LogError($"Service start failed: {ex}");
                throw;
            }
        }

        protected override void OnStop()
        {
            try
            {
                LoggerManager.LogFileInfo("Stopping OneGov Password Agent Service...");
                _cts?.Cancel();
                _listener?.Stop();
                LoggerManager.LogInfo("OneGov Password Agent Service stopped successfully.");
            }
            catch (Exception ex)
            {
                LoggerManager.LogError($"Error during service stop: {ex}");
            }
        }

        // ------------------------------------------------------------
        // DebugRun
        // Runs the service in console mode for local debugging.
        // ------------------------------------------------------------
        internal void DebugRun()
        {
            OnStart(null);
            Console.WriteLine("Running in console mode. Press ENTER to stop...");
            Console.ReadLine();
            OnStop();
        }
    }

    // ------------------------------------------------------------
    // Program entry point
    // Runs in:
    //   • Console mode when interactive (debug)
    //   • Service mode under the SCM otherwise
    // ------------------------------------------------------------
    static class Program
    {
        static void Main(string[] args)
        {
            if (Environment.UserInteractive)
            {
                var service = new OneGovPwdAgentSyncService();
                service.DebugRun();
            }
            else
            {
                ServiceBase.Run(new OneGovPwdAgentSyncService());
            }
        }
    }
}

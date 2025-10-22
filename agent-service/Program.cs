// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC SERVICE
//
//  Purpose:
//    Windows Service entry point that hosts the PipeListener responsible for
//    receiving password change notifications from the LSASS-side agent DLL.
//
//  Features:
//    • Runs as background Windows service ("OneGov PwdAgentSync service")
//    • Initializes configuration and logging subsystems
//    • Starts and supervises the PipeListener (Named Pipe server)
//    • Supports both service and console (debug) modes
//
//  Design Notes:
//    • Uses CancellationTokenSource for controlled async shutdown
//    • Avoids blocking the Service Control Manager (SCM)
//    • Ensures graceful cleanup and logging during stop
// =============================================================================

using System;
using System.ServiceProcess;
using System.Threading;
using System.Threading.Tasks;

namespace OneGovPwdAgentSync
{
    /// <summary>
    /// Main service class for OneGov Password Agent Sync.
    /// </summary>
    public class OneGovPwdAgentSyncService : ServiceBase
    {
        // ============================================================
        // PRIVATE FIELDS
        // ============================================================

        // Background pipe listener for LSASS messages
        private PipeListener _listener;

        // Token source for cooperative cancellation
        private CancellationTokenSource _cts;

        // ============================================================
        // CONSTRUCTOR
        // Sets service display name and default state.
        // ============================================================
        public OneGovPwdAgentSyncService()
        {
            ServiceName = "OneGov PwdAgentSync service";
        }

        // ============================================================
        // ONSTART()
        // Triggered when the Windows Service Control Manager starts service.
        // ============================================================
        protected override void OnStart(string[] args)
        {
            try
            {
                // -------------------------------------------
                // Load configuration and initialize logger
                // -------------------------------------------
                ConfigurationManager.Load();
                LoggerManager.Initialize();

                LoggerManager.LogFileInfo("=== Starting OneGov PwdAgentSync service ===");

                // -------------------------------------------
                // Prepare cooperative cancellation
                // -------------------------------------------
                _cts = new CancellationTokenSource();

                // Instantiate the Named Pipe listener
                _listener = new PipeListener();

                // -------------------------------------------
                // Launch background task to run listener
                // -------------------------------------------
                // Service thread must return quickly, so we offload listener
                // to a background worker Task.
                Task.Run(() => _listener.Start(_cts.Token));

                LoggerManager.LogInfo("PipeListener started successfully.");
            }
            catch (Exception ex)
            {
                // Critical startup failure (log + rethrow to notify SCM)
                LoggerManager.LogError($"Service start failed: {ex}");
                throw;
            }
        }

        // ============================================================
        // ONSTOP()
        // Triggered when SCM or user stops the service.
        // ============================================================
        protected override void OnStop()
        {
            try
            {
                LoggerManager.LogFileInfo("Stopping OneGov PwdAgentSync service...");

                // Request cancellation for background worker
                _cts?.Cancel();

                // Allow listener to shut down gracefully
                _listener?.Stop();

                LoggerManager.LogInfo("Service stopped successfully.");
            }
            catch (Exception ex)
            {
                // Log errors that occur during stop sequence
                LoggerManager.LogError($"Error during service stop: {ex}");
            }
        }

        // ============================================================
        // DEBUGRUN()
        // Allows service to be executed interactively (console mode).
        // Useful for local debugging and non-service environments.
        // ============================================================
        internal void DebugRun()
        {
            OnStart(null);
            Console.WriteLine("Running in console mode. Press ENTER to stop...");
            Console.ReadLine();
            OnStop();
        }
    }

    // ============================================================
    // PROGRAM ENTRY POINT
    // Detects execution context and runs either in:
    //   1. Interactive mode (console)
    //   2. Windows Service mode (via SCM)
    // ============================================================
    static class Program
    {
        static void Main(string[] args)
        {
            // Check if service is running interactively (e.g., debugging)
            if (Environment.UserInteractive)
            {
                // Console/debug run for development testing
                var service = new OneGovPwdAgentSyncService();
                service.DebugRun();
            }
            else
            {
                // Run as background service under SCM
                ServiceBase.Run(new OneGovPwdAgentSyncService());
            }
        }
    }
}

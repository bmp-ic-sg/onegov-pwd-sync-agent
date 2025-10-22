// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC - LOGGER MANAGER
//
//  Purpose:
//    Provides unified logging for the OneGov Password Agent Sync service.
//    Handles both file-based and Windows Event Log outputs with adjustable
//    verbosity via configuration.
//
//  Features:
//    • Supports multiple log levels (DEBUG, INFO, WARN, ERROR)
//    • Automatically rotates log files daily (YYYY-MM-DD naming)
//    • Logs to both Event Viewer and log file simultaneously
//    • Thread-safe file writes using lock()
//    • Reads log directory and level from OneGovPwdAgent.ini
//
//  Note:
//    Event Log registration requires administrative privileges. If the
//    event source is missing, the logger falls back gracefully to console.
// =============================================================================

using System;
using System.Diagnostics;
using System.IO;

namespace OneGovPwdAgentSync
{
    /// <summary>
    /// Defines supported log levels for the agent.
    /// </summary>
    public enum LogLevel
    {
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4
    }

    /// <summary>
    /// Provides centralized logging for both Event Log and log files.
    /// </summary>
    public static class LoggerManager
    {
        // ============================================================
        // INTERNAL FIELDS
        // ============================================================

        // Thread synchronization object (ensures safe concurrent writes)
        private static readonly object _lock = new();

        // Directory path for log files
        private static string _logDir;

        // Full path to current daily log file
        private static string _currentLogFile;

        // Minimum log level to record (controlled by LogLevel in .ini)
        private static LogLevel _minLevel = LogLevel.INFO;

        // The Windows Event Log source name
        public static string ServiceName { get; } = "OneGovPasswordAgentSync";


        // ============================================================
        // STATIC CONSTRUCTOR
        // Automatically runs once when LoggerManager is first used.
        // Loads configuration and ensures log directory exists.
        // ============================================================
        static LoggerManager()
        {
            try
            {
                // Read configuration entries (optional)
                _logDir = ConfigurationManager.Get("LogDir");
                string logLevel = ConfigurationManager.Get("LogLevel");

                // Fallback to default path if LogDir not set
                if (string.IsNullOrWhiteSpace(_logDir))
                {
                    _logDir = Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                        "OneGovPasswordAgent", "logs"
                    );
                    Console.WriteLine("[Logger] Using default log directory: " + _logDir);
                }

                // Ensure directory exists or create it
                Directory.CreateDirectory(_logDir);

                // Parse LogLevel string to enum (case-insensitive)
                if (Enum.TryParse(logLevel, true, out LogLevel parsed))
                    _minLevel = parsed;

                // Set log file for current day
                UpdateLogFile();

                // Confirm initialization
                Console.WriteLine($"[Logger] Minimum level: {_minLevel}");
            }
            catch (Exception ex)
            {
                // Fallback if something goes wrong during setup
                Console.WriteLine($"[Logger Init Error] {ex.Message}");
            }
        }


        // ============================================================
        // INITIALIZE()
        // Optional re-initialization (used if configuration changes at runtime)
        // ============================================================
        public static void Initialize()
        {
            try
            {
                // Re-read log directory setting
                _logDir = ConfigurationManager.Get("LogDir");
                if (string.IsNullOrWhiteSpace(_logDir))
                {
                    // Use same fallback path as constructor
                    _logDir = Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                        "OneGovPasswordAgent", "logs"
                    );
                }

                // Ensure log folder exists
                Directory.CreateDirectory(_logDir);

                // Refresh current log file name
                UpdateLogFile();

                Console.WriteLine("[Logger] Using directory: " + _logDir);
            }
            catch (Exception ex)
            {
                Console.WriteLine("[Logger Init] " + ex.Message);
            }
        }


        // ============================================================
        // UPDATELOGFILE()
        // Determines daily log file name based on system date.
        // ============================================================
        private static void UpdateLogFile()
        {
            try
            {
                // Create one log file per day (e.g., OneGovPwdAgentSync_2025-10-21.log)
                string date = DateTime.Now.ToString("yyyy-MM-dd");
                _currentLogFile = Path.Combine(_logDir, $"OneGovPwdAgentSync_{date}.log");
            }
            catch (Exception ex)
            {
                // Any error creating path just outputs to console
                Console.WriteLine($"[Logger Path Error] {ex.Message}");
            }
        }


        // ============================================================
        // PUBLIC LOGGING METHODS (COMBINED OUTPUT)
        // These log both to file and to Windows Event Log
        // ============================================================
        public static void LogInfo(string message)
        {
            // Always log INFO messages
            LogFileInfo(message);
            LogEventInfo(message);
        }

        public static void LogDebug(string message)
        {
            // Skip debug logs if current level > DEBUG
            if (_minLevel > LogLevel.DEBUG)
                return;

            LogFileDebug(message);
            LogEventInfo(message);
        }

        public static void LogWarn(string message)
        {
            // Always record warnings
            LogFileWarn(message);
            LogEventWarn(message);
        }

        public static void LogError(string message)
        {
            // Always record errors
            LogFileError(message);
            LogEventError(message);
        }


        // ============================================================
        // FILE LOGGING METHODS (WRITE TO LOG FILE)
        // Each writes a formatted timestamp + level + message
        // ============================================================
        private static void LogToFile(LogLevel level, string message)
        {
            try
            {
                // Refresh current log filename (daily rotation)
                UpdateLogFile();

                // Format timestamped log line
                string line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] [{level}] {message}";

                // Ensure only one thread writes at a time
                lock (_lock)
                {
                    File.AppendAllText(_currentLogFile, line + Environment.NewLine);
                }

                // Also mirror to console (useful for debugging)
                Console.WriteLine(line);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Logger File Error] {ex.Message}");
            }
        }

        // Individual wrappers for clarity
        public static void LogFileInfo(string message) => LogToFile(LogLevel.INFO, message);
        public static void LogFileDebug(string message) => LogToFile(LogLevel.DEBUG, message);
        public static void LogFileWarn(string message) => LogToFile(LogLevel.WARN, message);
        public static void LogFileError(string message) => LogToFile(LogLevel.ERROR, message);


        // ============================================================
        // EVENT LOG METHODS (WRITE TO WINDOWS EVENT LOG)
        // Automatically maps LogLevel → EventLogEntryType
        // ============================================================
        public static void LogEventInfo(string message) => LogToEvent(message, LogLevel.INFO);
        public static void LogEventWarn(string message) => LogToEvent(message, LogLevel.WARN);
        public static void LogEventError(string message) => LogToEvent(message, LogLevel.ERROR);

        private static void LogToEvent(string message, LogLevel level)
        {
            try
            {
                // Ensure the Event Source exists
                if (!EventLog.SourceExists(ServiceName))
                {
                    // Creating a new source requires admin rights, so skip gracefully
                    Console.WriteLine($"[Logger] Event log source '{ServiceName}' not found.");
                    return;
                }

                // Write log message to Windows Event Viewer
                EventLog.WriteEntry(ServiceName, message, MapEventType(level));
            }
            catch (Exception ex)
            {
                // Capture and print any event log write errors
                Console.WriteLine($"[Logger EventLog Error] {ex.Message}");
            }
        }


        // ============================================================
        // MAP EVENT TYPE
        // Converts LogLevel → Windows EventLogEntryType enum
        // ============================================================
        private static EventLogEntryType MapEventType(LogLevel level)
        {
            return level switch
            {
                LogLevel.ERROR => EventLogEntryType.Error,     // High severity
                LogLevel.WARN => EventLogEntryType.Warning,   // Mid severity
                _ => EventLogEntryType.Information // Default for INFO/DEBUG
            };
        }
    }
}

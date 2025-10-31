// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC — LOGGER MANAGER
//
//  Purpose:
//    Centralized logging for the OneGov Password Agent Sync service, writing to
//    both file and Windows Event Log with configurable verbosity.
//
//  Features:
//    • Log levels: DEBUG, INFO, WARN, ERROR
//    • Daily file rotation (YYYY-MM-DD)
//    • Dual output: file + Event Viewer
//    • Thread-safe file writes via lock()
//    • Reads log directory/level from OneGovPasswordAgent.ini
//
//  Note:
//    Creating a Windows Event Log source requires administrative rights.
//    If the source is missing, logging to Event Viewer is skipped gracefully.
// =============================================================================

using System;
using System.Diagnostics;
using System.IO;

namespace OneGovPwdAgentSync
{
    public enum LogLevel
    {
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4
    }

    public static class LoggerManager
    {
        private static readonly object _lock = new();
        private static string _logDir;
        private static string _currentLogFile;
        private static LogLevel _minLevel = LogLevel.INFO;

        // Event Log source name
        public static string ServiceName { get; } = "OneGovPasswordAgentService";

        static LoggerManager()
        {
            try
            {
                _logDir = ConfigurationManager.Get("LogDir");
                string logLevel = ConfigurationManager.Get("LogLevel");

                if (string.IsNullOrWhiteSpace(_logDir))
                {
                    _logDir = Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                        "OneGovPasswordAgent", "logs"
                    );
                    Console.WriteLine("[Logger] Using default log directory: " + _logDir);
                }

                Directory.CreateDirectory(_logDir);

                if (Enum.TryParse(logLevel, true, out LogLevel parsed))
                    _minLevel = parsed;

                UpdateLogFile();
                Console.WriteLine($"[Logger] Minimum level: {_minLevel}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Logger Init Error] {ex.Message}");
            }
        }

        public static void Initialize()
        {
            try
            {
                _logDir = ConfigurationManager.Get("LogDir");
                if (string.IsNullOrWhiteSpace(_logDir))
                {
                    _logDir = Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                        "OneGovPasswordAgent", "logs"
                    );
                }

                Directory.CreateDirectory(_logDir);
                UpdateLogFile();

                Console.WriteLine("[Logger] Using directory: " + _logDir);
            }
            catch (Exception ex)
            {
                Console.WriteLine("[Logger Init] " + ex.Message);
            }
        }

        // ---------------------------------------------------------------------
        // Determine today's log file name
        // ---------------------------------------------------------------------
        private static void UpdateLogFile()
        {
            try
            {
                string date = DateTime.Now.ToString("yyyy-MM-dd");
                _currentLogFile = Path.Combine(_logDir, $"OneGovPasswordAgentService_{date}.log");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Logger Path Error] {ex.Message}");
            }
        }

        // ---------------------------------------------------------------------
        // Public logging (mirrors to file + Event Log)
        // ---------------------------------------------------------------------
        public static void LogInfo(string message) { LogFileInfo(message); LogEventInfo(message); }
        public static void LogDebug(string message)
        {
            if (_minLevel <= LogLevel.DEBUG)
            {
                LogFileDebug(message);
                LogEventInfo(message);
            }
        }
        public static void LogWarn(string message) { LogFileWarn(message); LogEventWarn(message); }
        public static void LogError(string message) { LogFileError(message); LogEventError(message); }

        // ---------------------------------------------------------------------
        // File logging
        // ---------------------------------------------------------------------
        private static void LogToFile(LogLevel level, string message)
        {
            try
            {
                UpdateLogFile();
                string line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] [{level}] {message}";

                lock (_lock)
                {
                    File.AppendAllText(_currentLogFile, line + Environment.NewLine);
                }

                Console.WriteLine(line);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Logger File Error] {ex.Message}");
            }
        }

        public static void LogFileInfo(string message) => LogToFile(LogLevel.INFO, message);
        public static void LogFileDebug(string message) => LogToFile(LogLevel.DEBUG, message);
        public static void LogFileWarn(string message) => LogToFile(LogLevel.WARN, message);
        public static void LogFileError(string message) => LogToFile(LogLevel.ERROR, message);

        // ---------------------------------------------------------------------
        // Windows Event Log
        // ---------------------------------------------------------------------
        public static void LogEventInfo(string message) => LogToEvent(message, LogLevel.INFO);
        public static void LogEventWarn(string message) => LogToEvent(message, LogLevel.WARN);
        public static void LogEventError(string message) => LogToEvent(message, LogLevel.ERROR);

        private static void LogToEvent(string message, LogLevel level)
        {
            try
            {
                if (!EventLog.SourceExists(ServiceName))
                {
                    // Creating a source requires admin rights; skip if unavailable.
                    Console.WriteLine($"[Logger] Event Log source '{ServiceName}' not found.");
                    return;
                }

                EventLog.WriteEntry(ServiceName, message, MapEventType(level));
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Logger EventLog Error] {ex.Message}");
            }
        }

        // ---------------------------------------------------------------------
        // Map LogLevel → EventLogEntryType
        // ---------------------------------------------------------------------
        private static EventLogEntryType MapEventType(LogLevel level) =>
            level switch
            {
                LogLevel.ERROR => EventLogEntryType.Error,
                LogLevel.WARN => EventLogEntryType.Warning,
                _ => EventLogEntryType.Information
            };
    }
}

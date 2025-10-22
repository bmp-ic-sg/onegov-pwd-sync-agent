// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC - CONFIGURATION MANAGER
//
//  Purpose:
//    Provides lightweight INI-style configuration management for the
//    OneGovPwdAgentSync service. Automatically loads key-value pairs
//    from `OneGovPwdAgent.ini` located in the application directory.
//
//  Features:
//    • Reads and parses key=value configuration entries
//    • Supports inline comments (# or ;) and ignores blank lines
//    • Provides typed accessors (string, int, bool)
//    • Optional debug dump for runtime inspection
//
//  Note:
//    This class is intentionally simple and dependency-free to allow
//    usage in service mode environments without extra libraries.
// =============================================================================

using System;
using System.Collections.Generic;
using System.IO;

namespace OneGovPwdAgentSync
{
    /// <summary>
    /// Provides runtime configuration management for the agent service.
    /// </summary>
    public static class ConfigurationManager
    {
        // ------------------------------------------------------------
        // CONFIG FILE PATH
        // Located alongside the service executable:
        //   e.g. C:\Program Files\OneGov Password Sync\OneGovPwdAgent.ini
        // ------------------------------------------------------------
        private static readonly string ConfigPath =
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "OneGovPwdAgent.ini");

        // ------------------------------------------------------------
        // INTERNAL STATE
        // Stores parsed configuration key-value pairs (case-insensitive)
        // ------------------------------------------------------------
        private static readonly Dictionary<string, string> _settings =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        private static bool _loaded;

        // ============================================================
        // LOAD()
        // Reads and parses configuration file lines into dictionary.
        // ============================================================
        public static void Load()
        {
            try
            {
                _settings.Clear();

                if (!File.Exists(ConfigPath))
                {
                    Console.WriteLine($"[Config] File not found: {ConfigPath}");
                    return;
                }

                foreach (string rawLine in File.ReadAllLines(ConfigPath))
                {
                    string line = rawLine.Trim();

                    // Skip empty lines or comment lines
                    if (string.IsNullOrEmpty(line) || line.StartsWith("#") || line.StartsWith(";"))
                        continue;

                    // Parse as key=value
                    string[] parts = line.Split(new[] { '=' }, 2);
                    if (parts.Length != 2)
                        continue;

                    string key = parts[0].Trim();
                    string value = parts[1].Trim();

                    // Add or update key
                    if (!_settings.ContainsKey(key))
                        _settings.Add(key, value);
                    else
                        _settings[key] = value;
                }

                _loaded = true;
                Console.WriteLine($"[Config] Loaded configuration from {ConfigPath}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Config Error] {ex.Message}");
            }
        }

        // ============================================================
        // GET() - Retrieve string
        // ============================================================
        public static string Get(string key, string defaultValue = "")
        {
            return _settings.TryGetValue(key, out string value) ? value : defaultValue;
        }

        // ============================================================
        // GETINT() - Retrieve integer
        // ============================================================
        public static int GetInt(string key, int defaultValue = 0)
        {
            string val = Get(key, defaultValue.ToString());
            return int.TryParse(val, out int result) ? result : defaultValue;
        }

        // ============================================================
        // GETBOOL() - Retrieve boolean
        // Supports values: "true"/"1"
        // ============================================================
        public static bool GetBool(string key, bool defaultValue = false)
        {
            string val = Get(key, defaultValue ? "true" : "false");
            return val.Equals("true", StringComparison.OrdinalIgnoreCase) ||
                   val.Equals("1");
        }

        // ============================================================
        // ISLOADED PROPERTY
        // Indicates whether configuration has been loaded successfully.
        // ============================================================
        public static bool IsLoaded => _loaded;

        // ============================================================
        // DUMP() - Debug Utility
        // Prints all loaded configuration pairs to console output.
        // ============================================================
        public static void Dump()
        {
            Console.WriteLine("[Config] Current settings:");
            foreach (var kv in _settings)
                Console.WriteLine($"  {kv.Key} = {kv.Value}");
        }
    }
}

// =============================================================================
//  ONEGOV PASSWORD AGENT SYNC — CONFIGURATION MANAGER
//
//  Purpose:
//    Lightweight, dependency-free INI reader for the OneGovPasswordAgent service.
//    Loads key/value pairs from OneGovPasswordAgent.ini in the app directory.
//
//  Features:
//    • Parses key=value pairs
//    • Ignores blank lines and lines starting with # or ;
//    • Typed accessors: string, int, bool
//    • Optional Dump() for quick inspection
// =============================================================================

using System;
using System.Collections.Generic;
using System.IO;

namespace OneGovPwdAgentSync
{
    public static class ConfigurationManager
    {
        // ---------------------------------------------------------------------
        // Config file path (beside the service executable), e.g.:
        //   C:\Program Files\OneGov Password Sync\OneGovPasswordAgent.ini
        // ---------------------------------------------------------------------
        private static readonly string ConfigPath =
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "OneGovPasswordAgent.ini");

        private static readonly Dictionary<string, string> _settings =
            new(StringComparer.OrdinalIgnoreCase);

        private static bool _loaded;

        // ---------------------------------------------------------------------
        // Load: read and parse the INI file into the dictionary
        // ---------------------------------------------------------------------
        public static void Load()
        {
            try
            {
                _settings.Clear();

                if (!File.Exists(ConfigPath))
                {
                    Console.WriteLine($"[Configuration] File not found: {ConfigPath}");
                    _loaded = false;
                    return;
                }

                foreach (var raw in File.ReadAllLines(ConfigPath))
                {
                    var line = raw.Trim();

                    // Skip blanks, comments, and INI section headers like [General Settings], [connection], etc.
                    if (line.Length == 0 || line.StartsWith("#") || line.StartsWith(";") ||
                        (line.StartsWith("[") && line.EndsWith("]")))
                    {
                        continue;
                    }

                    // key=value (first '=' only)
                    var parts = line.Split(new[] { '=' }, 2);
                    if (parts.Length != 2) continue;

                    var key = parts[0].Trim();
                    var value = parts[1].Trim();

                    if (key.Length == 0) continue;

                    _settings[key] = value;
                }

                _loaded = true;
                Console.WriteLine($"[Configuration] Loaded: {ConfigPath}");
            }
            catch (Exception ex)
            {
                _loaded = false;
                Console.WriteLine($"[Configuration Error] {ex.Message}");
            }
        }

        public static string Get(string key, string defaultValue = "") =>
            _settings.TryGetValue(key, out var value) ? value : defaultValue;

        public static int GetInt(string key, int defaultValue = 0)
        {
            var val = Get(key, defaultValue.ToString());
            return int.TryParse(val, out var result) ? result : defaultValue;
        }

        public static bool GetBool(string key, bool defaultValue = false)
        {
            var val = Get(key, defaultValue ? "true" : "false");
            return val.Equals("true", StringComparison.OrdinalIgnoreCase) || val.Equals("1");
        }

        public static bool IsLoaded => _loaded;

        // ---------------------------------------------------------------------
        // Dump: print all key/value pairs (for debugging)
        // ---------------------------------------------------------------------
        public static void Dump()
        {
            Console.WriteLine("[Configuration] Current settings:");
            foreach (var kv in _settings)
                Console.WriteLine($"  {kv.Key} = {kv.Value}");
        }
    }
}

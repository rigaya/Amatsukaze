using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Intrinsics.X86;
using System.Text;

namespace Amatsukaze.Server.Rest
{
    public static class FilterPreviewScripts
    {
        public static void WriteScript(string path, string text) =>
            File.WriteAllBytes(path, Util.AmatsukazeDefaultEncoding.GetBytes(text));

        private static string Quote(string path) => "\"" + path.Replace('\\', '/').Replace("\"", "\" + Chr(34) + \"") + "\"";

        public static IReadOnlyList<string> SelectPlugins(string directory, bool avx, bool avx2)
        {
            if (!Directory.Exists(directory)) return Array.Empty<string>();
            var suffixes = new[] { "_avx2.dll", "_avx.dll", ".dll" };
            int support = avx2 ? 0 : avx ? 1 : 2;
            var selected = new SortedDictionary<string, string>(StringComparer.Ordinal);
            var files = Directory.GetFiles(directory).OrderBy(x => x, StringComparer.Ordinal).ToArray();
            for (int i = suffixes.Length - 1; i >= support; i--)
            {
                foreach (var file in files)
                {
                    var name = Path.GetFileName(file).ToLowerInvariant();
                    int category = Array.FindIndex(suffixes, suffix => name.EndsWith(suffix, StringComparison.Ordinal));
                    if (category == i) selected[name.Substring(0, name.Length - suffixes[i].Length)] = file;
                }
            }
            return selected.Values.ToArray();
        }

        public static string Source(string moduleDirectory, string datPath)
        {
            var library = Path.Combine(moduleDirectory, OperatingSystem.IsWindows() ? "Amatsukaze.dll" : "libAmatsukaze.so");
            return "ClearAutoloadDirs()\nLoadPlugin(" + Quote(library) + ")\nAMTSource(" + Quote(datPath) + ")\n";
        }

        public static string Preview(string moduleDirectory, string datPath, string workDirectory, FilterSetting filter, Setting setting, int device)
        {
            var sb = new StringBuilder();
            var plugins = Path.Combine(moduleDirectory, "plugins64");
            sb.AppendLine("ClearAutoloadDirs()");
            sb.AppendLine("AddAutoloadDir(" + Quote(plugins) + ")");
            foreach (var path in SelectPlugins(Path.Combine(plugins, "AutoSelected"), Avx.IsSupported, Avx2.IsSupported))
                sb.AppendLine("LoadPlugin(" + Quote(path) + ")");
            sb.AppendLine("SetCacheMode(CACHE_OPTIMAL_SIZE)");
            sb.AppendLine("LoadPlugin(" + Quote(Path.Combine(moduleDirectory, OperatingSystem.IsWindows() ? "Amatsukaze.dll" : "libAmatsukaze.so")) + ")");
            sb.AppendLine("AMT_SOURCE = AMTSource(" + Quote(datPath) + ")");
            sb.AppendLine("AMT_TMP = " + Quote(Path.Combine(workDirectory, "preview")));
            sb.AppendLine("AMT_PASS = 0");
            sb.AppendLine("AMT_DEV = " + device);
            sb.Append(AvsScriptCreator.FilterToString(filter, setting, false, true));
            if (filter.EnableCUDA) sb.AppendLine("OnCPU(2)");
            return sb.ToString();
        }
    }
}

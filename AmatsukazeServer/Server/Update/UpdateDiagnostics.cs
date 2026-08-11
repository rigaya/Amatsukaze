using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Security;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal static class UpdateDiagnostics
    {
        private static readonly string[] DiagnosticHosts =
        {
            "api.github.com",
            "objects.githubusercontent.com",
        };

        public static async Task LogEnvironmentAsync(UpdateLog log, string installRoot)
        {
            var appRoot = Safe(() => Path.GetFullPath(AppContext.BaseDirectory)).ToString();
            var values = new List<(string Key, object Value)>
            {
                ("os", Safe(() => RuntimeInformation.OSDescription)),
                ("os_version", Safe(() => Environment.OSVersion.VersionString)),
                ("arch", Safe(() => RuntimeInformation.OSArchitecture.ToString())),
                ("process_arch", Safe(() => RuntimeInformation.ProcessArchitecture.ToString())),
                ("dotnet", Safe(() => RuntimeInformation.FrameworkDescription)),
                ("app_root", appRoot),
                ("app_writable", Safe(() => CheckDirectoryWritable(appRoot) ? "yes" : "no")),
                ("app_free_bytes", Safe(() => GetAvailableFreeSpace(appRoot))),
                ("install_root", installRoot),
                ("install_writable", Safe(() => CheckDirectoryWritable(installRoot) ? "yes" : "no")),
                ("install_free_bytes", Safe(() => GetAvailableFreeSpace(installRoot))),
                ("temp_free_bytes", Safe(() => GetAvailableFreeSpace(Path.GetTempPath()))),
                ("http_proxy", Safe(() => SanitizeProxy(Environment.GetEnvironmentVariable("HTTP_PROXY")))),
                ("https_proxy", Safe(() => SanitizeProxy(Environment.GetEnvironmentVariable("HTTPS_PROXY")))),
                ("no_proxy", Safe(() => Environment.GetEnvironmentVariable("NO_PROXY") ?? "(none)")),
                ("default_proxy", Safe(GetDefaultProxy)),
                ("docker", Safe(() => IsDockerEnvironment() ? "yes" : "no")),
                ("exe_files_volume", Safe(GetAppExeFilesMountState)),
                ("execution_context", Safe(GetSafeExecutionContext)),
            };

            if (OperatingSystem.IsWindows())
            {
                values.Add(("winhttp_proxy", await SafeCommandAsync("netsh.exe",
                    new[] { "winhttp", "show", "proxy" }).ConfigureAwait(false)));
                values.Add(("antivirus", await SafeCommandAsync("powershell.exe", new[]
                {
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct | Select-Object -ExpandProperty displayName",
                }).ConfigureAwait(false)));
            }
            else
            {
                values.Add(("winhttp_proxy", "n/a"));
                values.Add(("antivirus", "n/a"));
            }
            log.Write("-", "S00_ENV", "OK", values.ToArray());
        }

        public static string GetLinuxDistributionVersion()
        {
            if (!OperatingSystem.IsLinux())
            {
                return null;
            }
            try
            {
                foreach (var line in File.ReadLines("/etc/os-release"))
                {
                    if (line.StartsWith("VERSION_ID=", StringComparison.Ordinal))
                    {
                        return line.Substring("VERSION_ID=".Length).Trim().Trim('"');
                    }
                }
            }
            catch
            {
                // ディストリビューション情報を取得できない場合は未対応判定に回す
            }
            return null;
        }

        public static bool IsDockerEnvironment()
        {
            try
            {
                if (File.Exists("/.dockerenv"))
                {
                    return true;
                }
            }
            catch
            {
            }
            try
            {
                if (File.Exists("/proc/1/cgroup"))
                {
                    var cgroup = File.ReadAllText("/proc/1/cgroup");
                    return cgroup.Contains("docker", StringComparison.OrdinalIgnoreCase) ||
                        cgroup.Contains("containerd", StringComparison.OrdinalIgnoreCase);
                }
            }
            catch
            {
            }
            return false;
        }

        public static async Task LogDnsFailureAsync(UpdateLog log, string target, Exception exception)
        {
            var values = new List<(string Key, object Value)>
            {
                ("code", "DNS_FAILED"),
                ("type", exception?.GetType().Name),
            };
            foreach (var host in DiagnosticHosts)
            {
                values.Add((host == "api.github.com" ? "api_dns" : "objects_dns",
                    await ResolveHostAsync(host).ConfigureAwait(false)));
                values.Add((host == "api.github.com" ? "api_tcp443" : "objects_tcp443",
                    await ProbeTcpAsync(host).ConfigureAwait(false)));
            }
            var tls = await ProbeTlsAsync("api.github.com").ConfigureAwait(false);
            values.Add(("tls", tls.Protocol));
            values.Add(("cert_issuer", tls.Issuer));
            values.Add(("tls_probe", tls.Result));
            values.Add(("system_time", DateTime.UtcNow));
            values.Add(("server_time", "?"));
            values.Add(("clockskew", "?"));
            log.WriteDiagnostic(target, "S03_CONNECT", values.ToArray());
        }

        public static async Task LogConnectionFailureAsync(UpdateLog log, string target, Exception exception)
        {
            var values = new List<(string Key, object Value)>
            {
                ("code", exception is OperationCanceledException ? "CONNECT_TIMEOUT" : "CONNECT_FAILED"),
                ("type", exception?.GetType().Name),
            };
            foreach (var host in DiagnosticHosts)
            {
                values.Add((host == "api.github.com" ? "api_dns" : "objects_dns",
                    await ResolveHostAsync(host).ConfigureAwait(false)));
                values.Add((host == "api.github.com" ? "api_tcp443" : "objects_tcp443",
                    await ProbeTcpAsync(host).ConfigureAwait(false)));
            }
            var tls = await ProbeTlsAsync("api.github.com").ConfigureAwait(false);
            values.Add(("tls", tls.Protocol));
            values.Add(("cert_issuer", tls.Issuer));
            values.Add(("tls_probe", tls.Result));
            values.Add(("system_time", DateTime.UtcNow));
            values.Add(("server_time", "?"));
            values.Add(("clockskew", "?"));
            log.WriteDiagnostic(target, "S03_CONNECT", values.ToArray());
        }

        public static async Task LogTlsFailureAsync(UpdateLog log, string target, Exception exception)
        {
            var probe = await ProbeTlsAsync("api.github.com").ConfigureAwait(false);
            log.WriteDiagnostic(target, "S03_CONNECT",
                ("code", "TLS_FAILED"),
                ("type", exception?.GetType().Name),
                ("tls", probe.Protocol),
                ("cert_issuer", probe.Issuer),
                ("system_time", DateTime.UtcNow),
                ("server_time", "?"),
                ("clockskew", "?"),
                ("probe", probe.Result));
        }

        public static void LogHttpFailure(UpdateLog log, string target, HttpStatusCode status,
            IReadOnlyDictionary<string, string> headers, string responseBody, string repository)
        {
            var values = new List<(string Key, object Value)>
            {
                ("code", "HTTP_" + ((int)status).ToString(CultureInfo.InvariantCulture)),
                ("repo", repository),
                ("status", (int)status),
            };
            if (status == HttpStatusCode.Forbidden)
            {
                foreach (var header in headers.Where(pair =>
                    pair.Key.StartsWith("X-RateLimit-", StringComparison.OrdinalIgnoreCase)))
                {
                    values.Add((header.Key.ToLowerInvariant().Replace('-', '_'), header.Value));
                }
                values.Add(("body", Limit(responseBody, 200)));
            }
            else if (status == HttpStatusCode.NotFound)
            {
                values.Add(("assets", "?"));
                values.Add(("body", Limit(responseBody, 200)));
            }
            log.WriteDiagnostic(target, "S03_CONNECT", values.ToArray());
        }

        public static void LogAssetNotFound(UpdateLog log, string target, string rule,
            IEnumerable<string> assetNames)
        {
            log.WriteDiagnostic(target, "S05_SELECT_ASSET",
                ("code", "ASSET_NOT_FOUND"),
                ("selected", "(none)"),
                ("rule", rule),
                ("assets", string.Join(",", assetNames)));
        }

        private static object Safe(Func<object> getter)
        {
            try
            {
                return getter() ?? "?";
            }
            catch
            {
                return "?";
            }
        }

        private static bool CheckDirectoryWritable(string directory)
        {
            var testPath = Path.Combine(directory, ".amatsukaze-update-write-test-" + Guid.NewGuid().ToString("N"));
            try
            {
                using (File.Create(testPath, 1, FileOptions.DeleteOnClose))
                {
                }
                return true;
            }
            catch
            {
                return false;
            }
            finally
            {
                try
                {
                    File.Delete(testPath);
                }
                catch
                {
                }
            }
        }

        private static long GetAvailableFreeSpace(string path)
        {
            var fullPath = Path.GetFullPath(path);
            var root = Path.GetPathRoot(fullPath);
            return string.IsNullOrEmpty(root) ? -1 : new DriveInfo(root).AvailableFreeSpace;
        }

        private static string GetDefaultProxy()
        {
            var proxy = WebRequest.DefaultWebProxy;
            if (proxy == null)
            {
                return "(none)";
            }
            return SanitizeProxy(proxy.GetProxy(new Uri("https://api.github.com/"))?.ToString());
        }

        private static string SanitizeProxy(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
            {
                return "(none)";
            }
            try
            {
                if (Uri.TryCreate(value, UriKind.Absolute, out var uri) && !string.IsNullOrEmpty(uri.UserInfo))
                {
                    var builder = new UriBuilder(uri)
                    {
                        UserName = string.Empty,
                        Password = string.Empty,
                    };
                    return builder.Uri.ToString();
                }
            }
            catch
            {
            }
            return value;
        }

        internal static string GetAppExeFilesMountState()
        {
            if (!OperatingSystem.IsLinux())
            {
                return "n/a";
            }
            try
            {
                foreach (var line in File.ReadLines("/proc/self/mountinfo"))
                {
                    var fields = line.Split(' ');
                    if (fields.Length > 4 && DecodeMountPath(fields[4]) == "/app/exe_files")
                    {
                        return "yes";
                    }
                }
                return "no";
            }
            catch
            {
                return "?";
            }
        }

        private static string DecodeMountPath(string path)
        {
            return path.Replace("\\040", " ", StringComparison.Ordinal)
                .Replace("\\011", "\t", StringComparison.Ordinal)
                .Replace("\\012", "\n", StringComparison.Ordinal)
                .Replace("\\134", "\\", StringComparison.Ordinal);
        }

        private static string GetSafeExecutionContext()
        {
            var values = new List<string>
            {
                "interactive=" + (Environment.UserInteractive ? "yes" : "no"),
                "privileged=" + (Environment.IsPrivilegedProcess ? "yes" : "no"),
            };
            if (OperatingSystem.IsWindows())
            {
                try
                {
                    values.Add("session=" + Process.GetCurrentProcess().SessionId.ToString(CultureInfo.InvariantCulture));
                }
                catch
                {
                    values.Add("session=?");
                }
            }
            return string.Join(",", values);
        }

        private static async Task<string> SafeCommandAsync(string executable, IReadOnlyList<string> arguments)
        {
            try
            {
                using var process = new Process();
                process.StartInfo = new ProcessStartInfo
                {
                    FileName = executable,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };
                foreach (var argument in arguments)
                {
                    process.StartInfo.ArgumentList.Add(argument);
                }
                if (!process.Start())
                {
                    return "?";
                }
                var stdout = process.StandardOutput.ReadToEndAsync();
                var stderr = process.StandardError.ReadToEndAsync();
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                try
                {
                    await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    try
                    {
                        process.Kill(true);
                    }
                    catch
                    {
                    }
                    return "?";
                }
                var output = (await stdout.ConfigureAwait(false)).Trim();
                var error = (await stderr.ConfigureAwait(false)).Trim();
                return Limit(string.IsNullOrEmpty(output) ? error : output, 1000);
            }
            catch
            {
                return "?";
            }
        }

        private static async Task<string> ResolveHostAsync(string host)
        {
            try
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                var addresses = await Dns.GetHostAddressesAsync(host, timeout.Token).ConfigureAwait(false);
                return addresses.Length == 0 ? "(none)" : string.Join(",", addresses.Select(address => address.ToString()));
            }
            catch (Exception ex)
            {
                return "ng:" + ex.GetType().Name;
            }
        }

        private static async Task<string> ProbeTcpAsync(string host)
        {
            try
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                using var client = new TcpClient();
                await client.ConnectAsync(host, 443, timeout.Token).ConfigureAwait(false);
                return "ok";
            }
            catch (Exception ex)
            {
                return "ng:" + ex.GetType().Name;
            }
        }

        private static async Task<(string Protocol, string Issuer, string Result)> ProbeTlsAsync(string host)
        {
            X509Certificate certificate = null;
            try
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                using var client = new TcpClient();
                await client.ConnectAsync(host, 443, timeout.Token).ConfigureAwait(false);
                using var stream = new SslStream(client.GetStream(), false,
                    (_, remoteCertificate, _, _) =>
                    {
                        certificate = remoteCertificate;
                        return true;
                    });
                await stream.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
                {
                    TargetHost = host,
                    EnabledSslProtocols = SslProtocols.None,
                    CertificateRevocationCheckMode = X509RevocationMode.NoCheck,
                }, timeout.Token).ConfigureAwait(false);
                return (stream.SslProtocol.ToString(), GetIssuer(certificate), "ok");
            }
            catch (Exception ex)
            {
                return ("?", GetIssuer(certificate), "ng:" + ex.GetType().Name);
            }
        }

        private static string GetIssuer(X509Certificate certificate)
        {
            if (certificate == null)
            {
                return "?";
            }
            try
            {
                using var certificate2 = new X509Certificate2(certificate);
                return certificate2.GetNameInfo(X509NameType.SimpleName, true);
            }
            catch
            {
                return certificate.Issuer ?? "?";
            }
        }

        private static string Limit(string value, int maxLength)
        {
            if (string.IsNullOrEmpty(value) || value.Length <= maxLength)
            {
                return value ?? string.Empty;
            }
            return value.Substring(0, maxLength);
        }
    }
}

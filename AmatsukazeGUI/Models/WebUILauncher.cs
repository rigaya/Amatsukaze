using System;
using System.Diagnostics;
using System.Net.Http;

namespace Amatsukaze.Models
{
    // WPF から WebUI のページを開くための補助。
    // standalone/リモートでの接続先の決め方を 1 箇所にまとめている。
    internal static class WebUILauncher
    {
        private static readonly TimeSpan ConnectionTimeout = TimeSpan.FromSeconds(3);

        // REST/WebUI の基準 URL を組み立てる。ポートが不明なら null。
        internal static string TryGetBaseUrl(ClientModel model)
        {
            var port = model.RestApiPort;
            if (port <= 0)
            {
                return null;
            }
            // standaloneモードではサーバーとクライアントが同一プロセスのため、接続先設定によらず127.0.0.1を使用
            // また、IPv6/IPv4不一致による接続失敗を避けるため、localhostではなく127.0.0.1を使用
            var host = (model.IsStandalone || string.IsNullOrWhiteSpace(model.ServerIP))
                ? "127.0.0.1" : model.ServerIP;
            return $"http://{host}:{port}";
        }

        // 相対パスのページをブラウザで開く。サーバーが応答しなければ false。
        internal static bool TryOpenPage(ClientModel model, string relativePath)
        {
            var baseUrl = TryGetBaseUrl(model);
            if (baseUrl == null)
            {
                return false;
            }
            try
            {
                var normalizedPath = relativePath ?? "";
                if (!normalizedPath.StartsWith("/", StringComparison.Ordinal))
                {
                    normalizedPath = "/" + normalizedPath;
                }
                var url = $"{baseUrl}{normalizedPath}";
                // HTTPリクエストでサーバーの応答を確認
                using var client = new HttpClient { Timeout = ConnectionTimeout };
                using var response = client.GetAsync(baseUrl).GetAwaiter().GetResult();
                Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}

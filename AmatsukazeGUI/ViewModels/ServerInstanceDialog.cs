using Amatsukaze.Server;
using Livet;
using Livet.Messaging;
using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;

namespace Amatsukaze.ViewModels
{
    // サーバー版とスタンドアロン版で、競合時の案内と接続先を共通にする。
    internal static class ServerInstanceDialog
    {
        public static async Task Show(ViewModel viewModel)
        {
            var root = Directory.GetCurrentDirectory();
            var port = App.Option.ServerPort;
            string text;
            if (ServerSupport.TryGetRunningServerInfo(root, out var info))
            {
                if (info.Kind == ServerInstanceKind.Standalone)
                {
                    await viewModel.Messenger.RaiseAsync(new InformationMessage(
                        "スタンドアロン版のAmatsukazeが起動しています。", "Amatsukaze", "Message"));
                    return;
                }
                port = info.Port;
                var isService = info.AccountType == "LocalSystem" || info.AccountType == "LocalService" || info.AccountType == "NetworkService";
                text = isService
                    ? "AmatsukazeServerがすでにサービス用アカウントで起動しています。"
                    : "AmatsukazeServerはすでに起動しています。";
                text += $"\r\nアカウント: {info.Account} / ポート: {port} / PID: {info.Pid}\r\n\r\nこのサーバーにクライアントとして接続しますか？";
                if (isService)
                    text += "\r\n\r\nEDCBなどのサービスから起動された可能性があります。終了するには、接続後に「その他」→「Amatsukazeサーバ操作」から停止してください。";
            }
            else
            {
                text = $"Amatsukazeが使用中ですが、種類と接続先を確認できませんでした。\r\nサーバーとして起動している場合は、127.0.0.1:{port}へ接続を試せます。\r\nスタンドアロン版の場合は接続できません。接続を試しますか？";
            }
            var message = new ConfirmationMessage(text, "Amatsukaze", MessageBoxImage.Information, MessageBoxButton.YesNo, "Confirm");
            await viewModel.Messenger.RaiseAsync(message);
            if (message.Response != true)
                return;
            try
            {
                var start = new ProcessStartInfo(Environment.ProcessPath) { WorkingDirectory = root };
                foreach (var arg in new[] { "-l", "client", "--ip", "127.0.0.1", "-p", port.ToString(System.Globalization.CultureInfo.InvariantCulture), "-r", root })
                    start.ArgumentList.Add(arg);
                using var process = Process.Start(start);
                if (process == null)
                    throw new InvalidOperationException("クライアントのプロセスを作成できませんでした。");
            }
            catch (Exception e)
            {
                await viewModel.Messenger.RaiseAsync(new InformationMessage(
                    "クライアントを起動できませんでした。\r\n" + e.Message, "Amatsukaze", "Message"));
            }
        }
    }
}

using System;
using System.Collections.Generic;
using System.Configuration;
using System.Data;
using System.Linq;
using System.Windows;

using Livet;
using System.Threading;
using System.Collections.Concurrent;
using Amatsukaze.Server;
using System.Runtime.InteropServices;
using System.IO;
using System.Threading.Tasks;
using System.Diagnostics;

namespace Amatsukaze
{
    /// <summary>
    /// App.xaml の相互作用ロジック
    /// </summary>
    public partial class App : Application
    {
        public static GUIOPtion Option;
        private static string CrashLogPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AmatsukazeGUI", "crash.log");

        private void Application_Startup(object sender, StartupEventArgs e)
        {
            // 例外ログとダイアログを有効化
            AppDomain.CurrentDomain.UnhandledException += CurrentDomain_UnhandledException;
            DispatcherUnhandledException += App_DispatcherUnhandledException;
            TaskScheduler.UnobservedTaskException += TaskScheduler_UnobservedTaskException;

            // オプションの初期化
            string[] args = e.Args;
            Option = new GUIOPtion(args);

            // RootDirが定義されていればカレントディレクトリを設定
            if(string.IsNullOrEmpty(Option.RootDir) == false &&
                Directory.Exists(Option.RootDir))
            {
                Directory.SetCurrentDirectory(Option.RootDir);
            }

            var logoCharSet = Util.AmatsukazeDefaultEncoding; // 文字コード(CP932)の登録のため、呼んでおく必要がある

            // ログパスを設定
            log4net.GlobalContext.Properties["Root"] = Directory.GetCurrentDirectory();
            log4net.Config.XmlConfigurator.Configure(new FileInfo(
                Path.Combine(System.AppContext.BaseDirectory, "Log4net.Config.xml")));

            DispatcherHelper.UIDispatcher = Dispatcher;
            //AppDomain.CurrentDomain.UnhandledException += new UnhandledExceptionEventHandler(CurrentDomain_UnhandledException);

            // StartupUriの設定
            if (Option.LaunchType == LaunchType.Server || Option.LaunchType == LaunchType.Debug)
            {
                this.StartupUri = new Uri(Path.Combine("Views", "ServerWindow.xaml"), UriKind.Relative);
            }
            else if(Option.LaunchType == LaunchType.Logo)
            {
                this.StartupUri = new Uri(Path.Combine("Views", "LogoAnalyzeWindow.xaml"), UriKind.Relative);
            }
            else 
            {
                this.StartupUri = new Uri(Path.Combine("Views", "MainWindow.xaml"), UriKind.Relative);
            }

            // 既定フォントサイズキーの初期化（XAMLをリテラルで置き、ここで定数へ）
            try
            {
                Current.Resources["AMT.FontSize"] = Amatsukaze.Models.UIConstants.DefaultFontSize;
            }
            catch { }
        }

        // 集約エラーハンドラ
        private void CurrentDomain_UnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            LogException(e.ExceptionObject as Exception, "CurrentDomain.UnhandledException");
        }

        private void App_DispatcherUnhandledException(object sender, System.Windows.Threading.DispatcherUnhandledExceptionEventArgs e)
        {
            LogException(e.Exception, "DispatcherUnhandledException");
            try
            {
                MessageBox.Show(
                    e.Exception?.ToString() ?? "不明なエラーが発生しました。",
                    "エラー",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
            catch { }
            e.Handled = false; // 既定のクラッシュ動作に任せる
        }

        private void TaskScheduler_UnobservedTaskException(object sender, UnobservedTaskExceptionEventArgs e)
        {
            LogException(e.Exception, "TaskScheduler.UnobservedTaskException");
        }

        private static void LogException(Exception ex, string source)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(CrashLogPath));
                using (var sw = new StreamWriter(CrashLogPath, true))
                {
                    sw.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {source}");
                    if (ex != null)
                    {
                        sw.WriteLine(ex.ToString());
                    }
                    sw.WriteLine("----");
                }
            }
            catch { }
        }

        public static void SetClipboardText(string str)
        {
            // RealVNCクライアント使ってると失敗する？ので
            try
            {
                Clipboard.SetText(str);
            }
            catch { }
        }
    }
}

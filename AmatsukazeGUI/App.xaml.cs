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

namespace Amatsukaze
{
    /// <summary>
    /// App.xaml の相互作用ロジック
    /// </summary>
    public partial class App : Application
    {
        public static GUIOPtion Option;

        private void Application_Startup(object sender, StartupEventArgs e)
        {
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
        }

        //集約エラーハンドラ
        //private void CurrentDomain_UnhandledException(object sender, UnhandledExceptionEventArgs e)
        //{
        //    //TODO:ロギング処理など
        //    MessageBox.Show(
        //        "不明なエラーが発生しました。アプリケーションを終了します。",
        //        "エラー",
        //        MessageBoxButton.OK,
        //        MessageBoxImage.Error);
        //
        //    Environment.Exit(1);
        //}

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

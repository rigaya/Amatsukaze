using System;
using System.IO;
using Amatsukaze.Lib;
using log4net;
using log4net.Appender;
using log4net.Layout;

namespace Amatsukaze.Server
{
    class ServerCLI
    {
        static void Main(string[] args)
        {
            try
            {
                var logoCharSet = Util.AmatsukazeDefaultEncoding; // 文字コード(CP932)の登録のため、呼んでおく

                // ログパスを設定
                log4net.GlobalContext.Properties["Root"] = Directory.GetCurrentDirectory();
                log4net.Config.XmlConfigurator.Configure(new FileInfo(
                    Path.Combine(System.AppContext.BaseDirectory, "Log4net.Config.xml")));

                // ConsoleAppender（標準エラー出力）を作成
                var consoleAppender = new ConsoleAppender
                {
                    Target = "Console.Error",
                    Layout = new PatternLayout("%date [%logger] %message%newline"),
                    Name = "ConsoleError"
                };
                consoleAppender.ActivateOptions();

                var loggerUserScript = (log4net.Repository.Hierarchy.Logger)LogManager.GetLogger("UserScript").Logger;
                loggerUserScript.AddAppender(consoleAppender);
                var loggerServer = (log4net.Repository.Hierarchy.Logger)LogManager.GetLogger("Server").Logger;
                loggerServer.AddAppender(consoleAppender);

                TaskSupport.SetSynchronizationContext();
                GUIOPtion option = new GUIOPtion(args);
                using (var lockFile = ServerSupport.GetLock())
                {
                    log4net.ILog LOG = log4net.LogManager.GetLogger("Server");
                    Util.LogHandlers.Add(text => LOG.Info(text));
                    using (var server = new EncodeServer(option.ServerPort, null, () =>
                     {
                         TaskSupport.Finish();
                     }))
                    {
                        var task = server.Init();

                        // この時点でtaskが完了していなくてもEnterMessageLoop()で続きが処理される

                        TaskSupport.EnterMessageLoop();

                        // この時点では"継続"を処理する人がいないので、
                        // task.Wait()はデッドロックするので呼べないことに注意
                        // あとはプログラムが終了するだけなのでWait()しても意味がない
                    }
                }
            }
            catch(MultipleInstanceException)
            {
                Console.WriteLine("多重起動を検知しました");
                return;
            }
            catch (Exception e)
            {
                Console.WriteLine(e.Message);
                return;
            }
        }
    }
}

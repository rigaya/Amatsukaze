using System;
using System.Diagnostics;
using System.IO;
using Amatsukaze.Lib;
using log4net;
using log4net.Appender;
using log4net.Layout;
using System.Runtime.InteropServices;
using System.Threading;

namespace Amatsukaze.Server
{
    class ServerCLI
    {
        static void Main(string[] args)
        {
            try
            {
                Util.EnsureEncodingProviderRegistered();

                // Debug.Printの出力を標準エラー出力に表示するためのリスナーを追加
                Trace.Listeners.Add(new TextWriterTraceListener(Console.Error));
                Trace.AutoFlush = true;

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

                        // Ctrl+C や SIGTERM でグレースフルシャットダウンする。
                        // .NET 10 では SIGTERM を送っても AppDomain.ProcessExit も
                        // AssemblyLoadContext.Unloading も発生せず即座にプロセスが落ちるため、
                        // PosixSignalRegistration で自前に受ける必要がある。
                        // (Windows でも Ctrl+C / Ctrl+Break / コンソールの終了に対応する)
                        int signalCount = 0;
                        Action<PosixSignalContext> signalHandler = ctx =>
                        {
                            if (Interlocked.Increment(ref signalCount) > 1)
                            {
                                // 2回目以降は既定動作 (即時終了) に任せる
                                return;
                            }
                            ctx.Cancel = true; // 自前で終了処理を行う
                            Console.WriteLine(ctx.Signal + " を受信しました。終了処理を行います。");
                            server.EndServer();
                        };

                        using (PosixSignalRegistration.Create(PosixSignal.SIGTERM, signalHandler))
                        using (PosixSignalRegistration.Create(PosixSignal.SIGINT, signalHandler))
                        using (PosixSignalRegistration.Create(PosixSignal.SIGQUIT, signalHandler))
                        {
                            TaskSupport.EnterMessageLoop();
                        }

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

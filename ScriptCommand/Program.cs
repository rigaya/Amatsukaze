﻿using Amatsukaze.Server;
using Microsoft.Win32.SafeHandles;
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Amatsukaze.Command
{
    class ScriptCommand
    {
        static void DoCommand(RPCMethodId id, string[] args)
        {
            var inPipe = new AnonymousPipeClientStream(PipeDirection.In,
                Environment.GetEnvironmentVariable("IN_PIPE_HANDLE"));
            var outPipe = new AnonymousPipeClientStream(PipeDirection.Out,
                Environment.GetEnvironmentVariable("OUT_PIPE_HANDLE"));

            var tag = (args.Length >= 1) ? args[0] : "";
            var bytes = RPCTypes.Serialize(id, tag);
            outPipe.Write(bytes, 0, bytes.Length);
            var ret = RPCTypes.Deserialize(inPipe).Result;
            Console.WriteLine((string)ret.arg);
        }

        static void CommandMain(string[] args)
        {
            if (Environment.GetEnvironmentVariable("IN_PIPE_HANDLE") == null)
            {
                // バッチファイルテスト用動作
                if (args.Length >= 2)
                {
                    Console.WriteLine(args[1]);
                }
                else
                {
                    Console.WriteLine("テスト実行です");
                }
                return;
            }

            // 自分のexe名がコマンドになる
            // コマンドライン引数はdll名が入ってしまっているので、
            // コマンド名を取得するためにProcess.GetCurrentProcess().ProcessNameを使用
            var exeName = System.Diagnostics.Process.GetCurrentProcess().ProcessName;
            if (exeName == "AddTag")
            {
                DoCommand(RPCMethodId.AddTag, args);
            }
            else if (exeName == "SetOutDir")
            {
                DoCommand(RPCMethodId.SetOutDir, args);
            }
            else if (exeName == "SetPriority")
            {
                DoCommand(RPCMethodId.SetPriority, args);
            }
            else if (exeName == "GetOutFiles")
            {
                DoCommand(RPCMethodId.GetOutFiles, args);
            }
            else if (exeName == "CancelItem")
            {
                DoCommand(RPCMethodId.CancelItem, args);
            }
            else
            {
                Console.WriteLine("不明なコマンドです");
            }
        }

        static void Main(string[] args)
        {
            // 親ディレクトリのDLLを参照できるようにする
            //（CLRはDLLをロードするときに環境変数PATHやカレントディレクトリは見ないことに注意）
            AppDomain.CurrentDomain.AssemblyResolve += (_, e) =>
            {
                var dir = System.AppContext.BaseDirectory; //Path.GetDirectoryName(typeof(ScriptCommand).Assembly.Location);
                // dirの終わりが区切り文字なら削除
                if (dir.EndsWith(Path.DirectorySeparatorChar.ToString()))
                {
                    dir = dir.Substring(0, dir.Length - 1);
                }
                // cmdディレクトリの親ディレクトリ（exe_files）を取得
                var parentDir = Path.GetDirectoryName(dir);
                var dllPath = Path.Combine(parentDir,
                    new System.Reflection.AssemblyName(e.Name).Name + ".dll");
                
                if (File.Exists(dllPath))
                {
                    return System.Reflection.Assembly.LoadFrom(dllPath);
                }
                
                return null;
            };

            CommandMain(args);
        }
    }
}

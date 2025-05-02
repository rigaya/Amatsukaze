﻿using Amatsukaze.Lib;
using log4net;
using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Amatsukaze.Server
{
    public enum ScriptPhase
    {
        OnAdd, PreEncode, PostEncode
    }

    public class UserScriptExecuter : IProcessExecuter
    {
        // 必須
        public EncodeServer Server;
        public ScriptPhase Phase;
        public string ScriptPath;
        public QueueItem Item;
        public Func<byte[], int, int, Task> OnOutput;

        // 追加時用
        public Program Prog;

        // 実行後用
        public LogItem Log;
        public List<string> RelatedFiles; // コピーしたEDCB関連ファイル
        public string MovedSrcPath; // 移動したソースファイルのパス

        private PipeCommunicator pipes = new PipeCommunicator();
        private NormalProcess process;

        private string PhaseString {
            get {
                switch (Phase)
                {
                    case ScriptPhase.OnAdd:
                        return "追加時";
                    case ScriptPhase.PreEncode:
                        return "実行前";
                    case ScriptPhase.PostEncode:
                        return "実行後";
                }
                return "不明フェーズ";
            }
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                if (disposing)
                {
                    // TODO: マネージ状態を破棄します (マネージ オブジェクト)。
                }

                // TODO: アンマネージ リソース (アンマネージ オブジェクト) を解放し、下のファイナライザーをオーバーライドします。
                process?.Dispose();
                // TODO: 大きなフィールドを null に設定します。
                process = null;

                disposedValue = true;
            }
        }

        // TODO: 上の Dispose(bool disposing) にアンマネージ リソースを解放するコードが含まれる場合にのみ、ファイナライザーをオーバーライドします。
        // ~UserScriptExecuter() {
        //   // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
        //   Dispose(false);
        // }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            // TODO: 上のファイナライザーがオーバーライドされる場合は、次の行のコメントを解除してください。
            // GC.SuppressFinalize(this);
        }
        #endregion

        private string GetOutFiles(string fmt)
        {
            int MAIN = 1;        // v
            int MAIN_CM = 2;     // c
            int MAIN_SUBS = 4;   // s
            int OTHER = 8;       // w
            int OTHER_CM = 16;   // d
            int OTHER_SUBS = 32; // t
            int RELATED = 64;    // r
            int LOG = 128;        // l

            int mask = 255;

            if(string.IsNullOrEmpty(fmt) == false && fmt != "all") {
                mask = 0;
                foreach(char c in fmt)
                {
                    switch(c)
                    {
                        case 'v':
                            mask |= MAIN;
                            break;
                        case 'c':
                            mask |= MAIN_CM;
                            break;
                        case 's':
                            mask |= MAIN_SUBS;
                            break;
                        case 'w':
                            mask |= OTHER;
                            break;
                        case 'd':
                            mask |= OTHER_CM;
                            break;
                        case 't':
                            mask |= OTHER_SUBS;
                            break;
                        case 'r':
                            mask |= RELATED;
                            break;
                        case 'l':
                            mask |= LOG;
                            break;
                    }
                }
            }

            var tailParser = new Regex("(-(\\d+))?(_div(\\d+))?(-cm)?(-nicojk-\\d+[ST])?(\\..+)");
            Func<string, int> fileType = tail =>
            {
                var m = tailParser.Match(tail);
                if (m.Success == false)
                {
                    throw new FormatException("出力ファイルのフォーマットがパースできません: " + tail);
                }
                var numStr = m.Groups[2].Value;
                var cmStr = m.Groups[5].Value;
                var ext = m.Groups[7].Value;
                bool isSubs = (ext == ".ass") || (ext == ".srt");
                int number = (numStr.Length == 0) ? 0 : int.Parse(numStr);
                bool isCM = (cmStr.Length > 0);
                if (number == 0)
                {
                    if (isSubs)
                    {
                        return MAIN_SUBS;
                    }
                    else if (isCM)
                    {
                        return MAIN_CM;
                    }
                    else
                    {
                        return MAIN;
                    }
                }
                else
                {
                    if (isSubs)
                    {
                        return OTHER_SUBS;
                    }
                    else if (isCM)
                    {
                        return OTHER_CM;
                    }
                    else
                    {
                        return OTHER;
                    }
                }
            };

            if (Log?.OutPath == null || Log?.DstPath == null)
            {
                // 失敗 or キャンセル
                return "";
            }

            try
            {
                var mainNoExt = Path.GetFileName(Log.DstPath);
                var list = Log.OutPath.Select(s => s.Replace('/', '\\')).Where(path =>
                {
                    var type = fileType(Path.GetFileName(path).Substring(mainNoExt.Length));
                    return (mask & type) != 0;
                }).ToList();

                if ((mask & RELATED) != 0)
                {
                    if(RelatedFiles != null)
                    {
                        list.AddRange(RelatedFiles);
                    }
                }

                if ((mask & LOG) != 0)
                {
                    if (Item.Profile.DisableLogFile == false)
                    {
                        list.Add(Log.DstPath + "-enc.log");
                    }
                }

                return string.Join(";", list);
            }
            catch(FormatException e)
            {
                return e.Message;
            }
        }

        private async Task RunCommandHost()
        {
            try
            {
                // 子プロセスが終了するまでループ
                while (true)
                {
                    var rpc = await RPCTypes.Deserialize(pipes.ReadPipe);
                    string ret = "";
                    try
                    {
                        switch (rpc.id)
                        {
                            case RPCMethodId.AddTag:
                                var tag = (string)rpc.arg;
                                if (!string.IsNullOrEmpty(tag) && !Item.Tags.Contains(tag))
                                {
                                    Item.Tags.Add(tag);
                                }
                                ret = string.Join(";", Item.Tags);
                                break;
                            case RPCMethodId.SetOutDir:
                                if (Phase == ScriptPhase.PostEncode)
                                {
                                    ret = "エンコードが完了したアイテムの出力先は変更できません";
                                }
                                else if(string.IsNullOrEmpty(rpc.arg as string))
                                {
                                    ret = "出力先を指定してください";
                                }
                                else
                                {
                                    var outdir = (rpc.arg as string).TrimEnd(Path.DirectorySeparatorChar);
                                    Item.DstPath = outdir + "\\" + Path.GetFileName(Item.DstPath);
                                    ret = "成功";
                                }
                                break;
                            case RPCMethodId.SetPriority:
                                if (Phase != ScriptPhase.OnAdd)
                                {
                                    ret = "追加時以外の優先度操作はできません";
                                }
                                else
                                {
                                    int priority;
                                    if (!int.TryParse(rpc.arg as string, out priority) ||
                                        priority < 1 || priority > 5)
                                    {
                                        ret = "優先度が範囲外です";
                                    }
                                    else
                                    {
                                        Item.Priority = priority;
                                        ret = "成功";
                                    }
                                }
                                break;
                            case RPCMethodId.GetOutFiles:
                                if (Phase != ScriptPhase.PostEncode)
                                {
                                    ret = "実行後ではないため、出力ファイルを取得できません";
                                }
                                else
                                {
                                    ret = GetOutFiles((string)rpc.arg);
                                }
                                break;
                            case RPCMethodId.CancelItem:
                                if (Phase == ScriptPhase.PostEncode)
                                {
                                    ret = "エンコードが完了したアイテムはキャンセルできません";
                                }
                                else
                                {
                                    Item.State = QueueState.Canceled;
                                    ret = "成功";
                                }
                                break;
                        }
                    }
                    catch(Exception e)
                    {
                        ret = e.Message;
                    }
                    var bytes = RPCTypes.Serialize(rpc.id, ret);
                    await pipes.WritePipe.WriteAsync(bytes, 0, bytes.Length);
                }
            }
            catch (Exception)
            {
                // 子プロセスが終了すると例外を吐く
            }
        }

        private String ConvertStrZtoH(String str)
        {
            var strZenkaku1 = @"０１２３４５６７８９ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ";
            var strZenkaku2 = @"　！”＃＄％＆’（）＊＋，－．／：；＜＝＞？＠［￥］＾＿‘｛｜｝￣";
            var strHankaku1 = @"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            var strHankaku2 = @" !”#$%&'()＊+,-.／：;＜=＞？@[￥]^_`{｜}~"; // パスに使用できない文字は全角のままにする

            var strZenkaku = strZenkaku1 + strZenkaku2;
            var strHankaku = strHankaku1 + strHankaku2;

            var sb = new StringBuilder();
            var itstr = StringInfo.GetTextElementEnumerator(str);
            while (itstr.MoveNext())
            {
                var targetChar = itstr.GetTextElement();
                // targetCharがstrZenkakuに含まれる文字列なら、同じ位置のstrHankakuの文字列に置換する
                var index = strZenkaku.IndexOf(targetChar);
                if (index >= 0)
                {
                    sb.Append(strHankaku[index]);
                }
                else
                {
                    sb.Append(targetChar);
                }
            }
            return sb.ToString();
        }

        public string RemoveBracketedChars(string str)
        {
            // 正規表現パターンを作成し、カッコで囲まれた1文字を検索する。
            var regex = new Regex(@"\[\w\]");
            while (regex.IsMatch(str))
            {
                str = regex.Replace(str, "");
            }
            return str;
        }

        private void SetupEnv(StringDictionary env)
        {
            var genre = Item.Genre.FirstOrDefault();
            var displayGenre = (genre != null) ? SubGenre.GetDisplayGenre(genre) : null;

            var inPath = MovedSrcPath ?? Item.SrcPath;
            var inDir = Path.GetDirectoryName(inPath);
            var inExt = Path.GetExtension(inPath);
            var inPath2 = Path.Combine(inDir, RemoveBracketedChars(Path.GetFileName(inPath)));
            var inPathZtoH = Path.Combine(inDir, ConvertStrZtoH(Path.GetFileName(inPath)));
            var inPath2ZtoH = Path.Combine(inDir, ConvertStrZtoH(Path.GetFileName(inPath2)));
            var inFilename = Path.GetFileNameWithoutExtension(inPath);
            var inFilenameZtoH = ConvertStrZtoH(inFilename);
            var inFilename2 = RemoveBracketedChars(inFilename);
            var inFilename2ZtoH = ConvertStrZtoH(inFilename2);

            var outDir = Path.GetDirectoryName(Item.DstPath); // DstPathは拡張子がついていないらしい
            var outPath = Item.DstPath;
            var outPath2 = Path.Combine(outDir, RemoveBracketedChars(Path.GetFileName(outPath)));
            var outPathZtoH = Path.Combine(outDir, ConvertStrZtoH(Path.GetFileName(outPath)));
            var outPath2ZtoH = Path.Combine(outDir, ConvertStrZtoH(Path.GetFileName(outPath2)));
            var outFilename = Path.GetFileName(outPath);
            var outFilenameZtoH = ConvertStrZtoH(outFilename);
            var outFilename2 = RemoveBracketedChars(outFilename);
            var outFilename2ZtoH = ConvertStrZtoH(outFilename2);

            var eventName = Item.EventName;
            var eventName2 = RemoveBracketedChars(eventName);
            var eventNameZtoH = ConvertStrZtoH(eventName);
            var eventName2ZtoH = ConvertStrZtoH(eventName2);

            env.Add("ITEM_ID", Item.Id.ToString());
            env.Add("IN_PATH", inPath);
            env.Add("IN_PATH2", inPath2);
            env.Add("IN_PATH_ZTOH", inPathZtoH);
            env.Add("IN_PATH2_ZTOH", inPath2ZtoH);
            env.Add("IN_FILENAME", inFilename);
            env.Add("IN_FILENAME_ZTOH", inFilenameZtoH);
            env.Add("IN_FILENAME2", inFilename2);
            env.Add("IN_FILENAME2_ZTOH", inFilename2ZtoH);
            env.Add("IN_DIR", inDir);
            env.Add("IN_EXT", inExt);

            env.Add("OUT_PATH", outPath);
            env.Add("OUT_PATH2", outPath2);
            env.Add("OUT_PATH_ZTOH", outPathZtoH);
            env.Add("OUT_PATH2_ZTOH", outPath2ZtoH);
            env.Add("OUT_FILENAME", outFilename);
            env.Add("OUT_FILENAME_ZTOH", outFilenameZtoH);
            env.Add("OUT_FILENAME2", outFilename2);
            env.Add("OUT_FILENAME2_ZTOH", outFilename2ZtoH);
            env.Add("OUT_DIR", outDir);

            env.Add("SERVICE_ID", Item.ServiceId.ToString());
            env.Add("SERVICE_NAME", Item.ServiceName);
            env.Add("SERVICE_NAME_ZTOH", ConvertStrZtoH(Item.ServiceName));
            env.Add("TS_TIME", Item.TsTime.ToString());
            env.Add("ITEM_MODE", Item.Mode.ToString());
            env.Add("ITEM_PRIORITY", Item.Priority.ToString());
            env.Add("EVENT_GENRE", displayGenre?.FullName ?? "-");
            env.Add("IMAGE_WIDTH", Item.ImageWidth.ToString());
            env.Add("IMAGE_HEIGHT", Item.ImageHeight.ToString());
            env.Add("EVENT_NAME", eventName);
            env.Add("EVENT_NAME2", eventName2);
            env.Add("EVENT_NAME_ZTOH", eventNameZtoH);
            env.Add("EVENT_NAME2_ZTOH", eventName2ZtoH);
            env.Add("TAG", string.Join(";", Item.Tags));

            if(Phase != ScriptPhase.OnAdd)
            {
                env.Add("PROFILE_NAME", Item.Profile.Name);
            }
            if(Log != null)
            {
                env.Add("SUCCESS", Log.Success ? "1" : "0");
                env.Add("ERROR_MESSAGE", Log.Reason ?? "");
                env.Add("IN_DURATION", Log.SrcVideoDuration.TotalSeconds.ToString());
                env.Add("OUT_DURATION", Log.OutVideoDuration.TotalSeconds.ToString());
                env.Add("IN_SIZE", Log.SrcFileSize.ToString());
                env.Add("OUT_SIZE", Log.OutFileSize.ToString());
                env.Add("LOGO_FILE", (Log.LogoFiles != null) ? string.Join(";", Log.LogoFiles) : "");
                env.Add("NUM_INCIDENT", Log.Incident.ToString());
                env.Add("JSON_PATH", Path.GetFullPath(Server.GetLogFileBase(Log.EncodeStartDate) + ".json"));
                env.Add("LOG_PATH", Path.GetFullPath(Server.GetLogFileBase(Log.EncodeStartDate) + ".log"));
            }

            // パイプ通信用
            env.Add("IN_PIPE_HANDLE", pipes.InHandle);
            env.Add("OUT_PIPE_HANDLE", pipes.OutHandle);
        }

        public async Task Execute()
        {
            var psi = new ProcessStartInfo("cmd.exe", "/C \"" + ScriptPath + "\"")
            {
                UseShellExecute = false,
                WorkingDirectory = Directory.GetCurrentDirectory(),
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                RedirectStandardInput = false,
                StandardOutputEncoding = Util.AmatsukazeDefaultEncoding,
                StandardErrorEncoding = Util.AmatsukazeDefaultEncoding,
                CreateNoWindow = true
            };

            // exe_filesをパスに追加
            var exeDir = Path.GetDirectoryName(typeof(UserScriptExecuter).Assembly.Location);
            // Specialized.StringDictionaryのkeyはcase insensitiveであることに注意
            psi.EnvironmentVariables["path"] =
                exeDir + ";" + exeDir + "\\cmd" + ";" + psi.EnvironmentVariables["path"];

            // パラメータを環境変数に追加
            SetupEnv(psi.EnvironmentVariables);

            using (process = new NormalProcess(psi)
            {
                OnOutput = OnOutput
            })
            {
                pipes.DisposeLocalCopyOfClientHandle();

                await Task.WhenAll(
                    process.WaitForExitAsync(),
                    RunCommandHost());
            }
        }

        public void Canel()
        {
            process?.Canel();
        }

        // 未実装
        public void Suspend() { }
        public void Resume() { }
    }
}

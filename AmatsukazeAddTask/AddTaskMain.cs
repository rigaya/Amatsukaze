using Amatsukaze.Server;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace Amatsukaze.AddTask
{
    public class GUIOPtion
    {
        public int ServerPort = ServerSupport.DEFAULT_PORT;
        public string ServerIP = "localhost";
        public string AmatsukazeRoot;

        public string FilePath;
        public string OutPath;
        public string Profile;
        public string AddQueueBat;
        public int Priority = 3;

        public int ItemID = -1;
        public ChangeItemType ChangeType = ChangeItemType.ResetState;

        public string NasDir;
        public bool NoMove;
        public bool ClearSucceeded;
        public bool WithRelated;

        public string RemoteDir;

        public string Subnet = "255.255.255.0";
        public byte[] MacAddress;

        public static void PrintHelp()
        {
            string help =
                Environment.GetCommandLineArgs()[0] + " <オプション>\r\n" +
                "タスク追加用オプション\r\n" +
                "  -f|--file <パス>        入力ファイルパス\r\n" +
                "  -s|--setting <プロファイル名> エンコード設定プロファイル\r\n" +
                "  -b|--bat <バッチファイル名> 追加時実行バッチ\r\n" +
                "  --priority <優先度>     優先度\r\n" +
                "  -o|--outdir <パス>      出力先ディレクトリ\r\n" +
                "  -d|--nasdir <パス>      NASのTSファイル置き場\r\n" +
                "  --remote-dir <パス>     サーバから入力ファイルにアクセスするときのディレクトリパス\r\n" +
                "                          サーバからだとパスが異なる場合に必要\r\n" +
                "                          ファイル名部分は入力ファイル名からコピーされるのでディレクトリパスのみ入力すること\r\n" +
                "                          NASにコピーするときはコピー先のディレクトリをサーバからアクセスするときのパスを指定すること\r\n" +
                "                          例) \\\\rec-pc\\share\\rec\r\n" +
                "                          Bash等から打つときは\\(バックスラッシュ)がエスケープ文字となることがあるため注意\r\n" +
                "  --no-move               NASにコピーしたTSファイルをtransferedフォルダに移動しない\r\n" +
                "  --clear-succeeded       NASにコピーする際、コピー先のsucceededフォルダを空にする\r\n" +
                "  --with-related          NASにコピーする際、関連ファイルも一緒にコピー・移動する\r\n" +
                "\r\n" +
                "タスク操作用オプション\r\n" +
                "  --item-id <ID>          操作対象ITEM ID\r\n" +
                "  --action <操作名>\r\n" +
                "    - retry               リトライ\n" +
                "    - updateprofile       プロファイルを更新してリトライ\r\n" +
                "    - duplicate           同じタスクを投入\r\n" +
                "    - cancel              キャンセル\r\n" +
                "    - priority            優先度変更\r\n" +
                "    - profile             プロファイル変更\r\n" +
                "    - removeitem          アイテム削除\r\n" +
                "    - forcestart          アイテムを強制的に開始\r\n" +
                "  -s|--setting <プロファイル名> --action profile での変更先\r\n" +
                "  --priority <優先度>           --action priority での変更先\r\n" +
                "\r\n" +
                "AmatsukazeServer 指定用オプション (共通)\r\n" +
                "  -ip|--ip <IPアドレス>   AmatsukazeServerアドレス\r\n" +
                "  -p|--port <ポート番号>  AmatsukazeServerポート番号\r\n" +
                "  -r|--amt-root <パス>    Amatsukazeのルートディレクトリ（サーバ起動用）\r\n" +
                "  --subnet <サブネットマスク>  Wake On Lan用サブネットマスク\r\n" +
                "  --mac <MACアドレス>  Wake On Lan用MACアドレス\r\n";
            Console.WriteLine(help);
        }

        public ChangeItemType GetChangeItemType(string arg)
        {
            switch (arg)
            {
                case "retry":
                    return ChangeItemType.ResetState;
                case "updateprofile":
                    return ChangeItemType.UpdateProfile;
                case "duplicate":
                    return ChangeItemType.Duplicate;
                case "cancel":
                    return ChangeItemType.Cancel;
                case "profile":
                    return ChangeItemType.Profile;
                case "priority":
                    return ChangeItemType.Priority;
                case "removeitem":
                    return ChangeItemType.RemoveItem;
                case "forcestart":
                    return ChangeItemType.ForceStart;
                default:
                    throw new Exception("不正な操作名です");
            }
        }

        public GUIOPtion(string[] args)
        {
            // デフォルトはexeのあるディレクトリの１つ上
            AmatsukazeRoot = Path.GetDirectoryName(System.AppContext.BaseDirectory);

            for (int i = 0; i < args.Length; ++i)
            {
                string arg = args[i];
                if (arg == "-p" || arg == "--port")
                {
                    ServerPort = int.Parse(args[i + 1]);
                    ++i;
                }
                else if (arg == "-ip" || arg == "--ip")
                {
                    ServerIP = args[i + 1];
                    ++i;
                }
                else if (arg == "-f" || arg == "--file")
                {
                    FilePath = Path.GetFullPath(args[i + 1]);
                    ++i;
                }
                else if (arg == "--item-id")
                {
                    ItemID = int.Parse(args[i + 1]);
                    ++i;
                }
                else if (arg == "--action")
                {
                    ChangeType = GetChangeItemType(args[i + 1]);
                    ++i;
                }
                else if (arg == "--remote-dir")
                {
                    RemoteDir = args[i + 1].TrimEnd(Path.DirectorySeparatorChar);
                    ++i;
                }
                else if (arg == "-s" || arg == "--setting")
                {
                    Profile = args[i + 1];
                    ++i;
                }
                else if (arg == "-b" || arg == "--bat")
                {
                    AddQueueBat = args[i + 1];
                    ++i;
                }
                else if (arg == "--priority")
                {
                    Priority = int.Parse(args[i + 1]);
                    ++i;
                }
                else if (arg == "-o" || arg == "--outdir")
                {
                    OutPath = Path.GetFullPath(args[i + 1]);
                    ++i;
                }
                else if (arg == "-d" || arg == "--nasdir")
                {
                    NasDir = args[i + 1];
                    ++i;
                }
                else if (arg == "-r" || arg == "--amt-root")
                {
                    AmatsukazeRoot = args[i + 1];
                    ++i;
                }
                else if (arg == "--no-move")
                {
                    NoMove = true;
                }
                else if (arg == "--clear-succeeded")
                {
                    ClearSucceeded = true;
                }
                else if (arg == "--with-related")
                {
                    WithRelated = true;
                }
                else if (arg == "--subnet")
                {
                    Subnet = args[i + 1];
                    ++i;
                }
                else if (arg == "--mac")
                {
                    var str = args[i + 1];
                    MacAddress = str
                        .Split(str.Contains(':') ? ':' : '-')
                        .Select(s => byte.Parse(s, System.Globalization.NumberStyles.HexNumber)).ToArray();
                    if(MacAddress.Length != 6)
                    {
                        throw new Exception("MACアドレスが不正です");
                    }
                    ++i;
                }
            }

            if(ItemID < 0 && string.IsNullOrEmpty(FilePath))
            {
                throw new Exception("入力ファイルパスを入力してください");
            }
        }
    }

    class WakeOnLan
    {
        private static void Send(IPAddress broad, byte[] macAddress)
        {
            MemoryStream stream = new MemoryStream();
            BinaryWriter writer = new BinaryWriter(stream);
            for (int i = 0; i < 6; i++)
            {
                writer.Write((byte)0xff);
            }
            for (int i = 0; i < 16; i++)
            {
                writer.Write(macAddress);
            }
            byte[] data = stream.ToArray();

            UdpClient client = new UdpClient();
            client.EnableBroadcast = true;
            client.Send(data, data.Length, new IPEndPoint(broad, 7));
            client.Send(data, data.Length, new IPEndPoint(broad, 9));
            client.Close();
        }

        private static IPAddress GetBroadcastAddress(IPAddress address, IPAddress subnetMask)
        {
            byte[] ipAdressBytes = address.GetAddressBytes();
            byte[] subnetMaskBytes = subnetMask.GetAddressBytes();

            if (ipAdressBytes.Length != subnetMaskBytes.Length)
            {
                throw new ArgumentException("Lengths of IP address and subnet mask do not match.");
            }

            byte[] broadcastAddress = new byte[ipAdressBytes.Length];
            for (int i = 0; i < broadcastAddress.Length; i++)
            {
                broadcastAddress[i] = (byte)(ipAdressBytes[i] | (subnetMaskBytes[i] ^ 255));
            }
            return new IPAddress(broadcastAddress);
        }

        public static void Wake(IPAddress address, IPAddress subnetMask, byte[] macAddress)
        {
            Send(GetBroadcastAddress(address, subnetMask), macAddress);
        }
    }

    class AddTask : IAddTaskClient
    {
        public GUIOPtion option;

        private CUIServerConnection server;
        AddQueueRequest addRequest;
        ChangeItemData resetRequest;
        private bool okReceived;

        private static bool IsRunningOnMono()
        {
            return Type.GetType("Mono.Runtime") != null;
        }

        public async Task Exec()
        {
            if (!string.IsNullOrEmpty(option.FilePath))
            {
                string srcpath = option.FilePath;
                byte[] hash = null;

                if (string.IsNullOrEmpty(option.NasDir) == false)
                {
                    if (File.Exists(option.FilePath) == false)
                    {
                        throw new Exception("入力ファイルが見つかりません");
                    }

                    if (option.ClearSucceeded)
                    {
                        // succeededを空にする
                        var succeeded = option.NasDir + Path.DirectorySeparatorChar + "succeeded";
                        if (Directory.Exists(succeeded))
                        {
                            foreach (var file in Directory.GetFiles(succeeded))
                            {
                                File.Delete(file);
                            }
                        }
                    }
                    // NASにコピー
                    var remotepath = option.NasDir + Path.DirectorySeparatorChar + Path.GetFileName(srcpath);
                    hash = await HashUtil.CopyWithHash(option.FilePath, remotepath);
                    srcpath = remotepath;
                    if (option.WithRelated)
                    {
                        var body = Path.GetFileNameWithoutExtension(option.FilePath);
                        var tsext = Path.GetExtension(option.FilePath);
                        var srcDir = Path.GetDirectoryName(option.FilePath);
                        foreach (var ext in ServerSupport.GetFileExtentions(null, true))
                        {
                            string srcPath = srcDir + Path.DirectorySeparatorChar + body + ext;
                            string dstPath = option.NasDir + Path.DirectorySeparatorChar + body + ext;
                            if (File.Exists(srcPath))
                            {
                                File.Copy(srcPath, dstPath);
                            }
                        }
                    }
                }

                if (string.IsNullOrEmpty(option.RemoteDir) == false)
                {
                    // これはサーバからのアクセス用パスなので"\\"で区切る
                    srcpath = Path.Combine(option.RemoteDir, Path.GetFileName(srcpath));
                }

                Console.WriteLine(srcpath + " を追加します");

                // リクエストを生成
                addRequest = new AddQueueRequest()
                {
                    DirPath = Path.GetDirectoryName(srcpath),
                    Outputs = new List<OutputInfo>()
                    {
                        new OutputInfo()
                        {
                            DstPath = option.OutPath,
                            Profile = option.Profile,
                            Priority = option.Priority,
                        }
                    },
                    Targets = new List<AddQueueItem>() {
                        new AddQueueItem() { Path = srcpath, Hash = hash }
                    },
                    Mode = ProcMode.AutoBatch,
                    RequestId = UniqueId(),
                    AddQueueBat = option.AddQueueBat
                };
            }
            else if (option.ItemID >= 0)
            {
                resetRequest = new ChangeItemData()
                {
                    ItemId = option.ItemID,
                    ChangeType = option.ChangeType,
                    RequestId = UniqueId()
                };
                if (option.ChangeType == ChangeItemType.Priority)
                {
                    resetRequest.Priority = option.Priority;
                }
                if (option.ChangeType == ChangeItemType.Profile)
                {
                    resetRequest.Profile = option.Profile;
                }
            }

            server = new CUIServerConnection(this);
            bool isLocal = !IsRunningOnMono() && ServerSupport.IsLocalIP(option.ServerIP);
            int maxRetry = isLocal ? 3 : 5;

            for (int i = 0; i < maxRetry; ++i)
            {
                if(i > 0)
                {
                    Console.WriteLine("再試行します・・・");
                }

                try
                {
                    // サーバに接続
                    server.Connect(option.ServerIP, option.ServerPort);
                }
                catch(Exception e0)
                {
                    Console.WriteLine(e0.StackTrace);
                    // サーバに繋がらなかった

                    // ローカルの場合は、起動を試みる
                    if (isLocal)
                    {
                        await ServerSupport.TerminateStandalone(option.AmatsukazeRoot);
                        ServerSupport.LaunchLocalServer(option.ServerPort, option.AmatsukazeRoot);

                        // 10秒待つ
                        await Task.Delay(10 * 1000);
                    }
                    else
                    {
                        // リモートの場合は、Wake On Lanする
                        if(option.MacAddress == null)
                        {
                            throw new Exception("リモートサーバへの接続に失敗しました。");
                        }

                        Console.WriteLine("Wake On Lanで起動を試みます。");

                        WakeOnLan.Wake(
                            IPAddress.Parse(option.ServerIP),
                            IPAddress.Parse(option.Subnet),
                            option.MacAddress);

                        // 40秒待つ
                        await Task.Delay(40 * 1000);
                    }

                    continue;
                }

                try
                {
                    // サーバにタスク登録
                    if (addRequest != null)
                    {
                        await server.AddQueue(addRequest);
                    }
                    else if (resetRequest != null)
                    {
                        await server.ChangeItemTask(resetRequest);
                    }
                    else
                    {
                        throw new Exception("リクエストが不正です");
                    }

                    // リクエストIDの完了通知ゲット or タイムアウトしたら終了
                    var timeout = Task.Delay(30 * 1000);
                    while (okReceived == false)
                    {
                        var recv = server.ProcOneMessage();
                        if(await Task.WhenAny(recv, timeout) == timeout)
                        {
                            Console.WriteLine("サーバのリクエスト受理を確認できませんでした。");
                            throw new Exception();
                        }
                    }
                }
                catch (Exception e1)
                {
                    Console.WriteLine(e1.StackTrace);
                    // なぜか失敗した
                    continue;
                }

                if (string.IsNullOrEmpty(option.NasDir) == false && !option.NoMove)
                {
                    // NASにコピーしたファイルはtransferredフォルダに移動
                    string trsDir = Path.GetDirectoryName(option.FilePath) + Path.DirectorySeparatorChar + "transferred";
                    Directory.CreateDirectory(trsDir);
                    string trsFile = trsDir + Path.DirectorySeparatorChar + Path.GetFileName(option.FilePath);
                    if(File.Exists(option.FilePath))
                    {
                        if (File.Exists(trsFile))
                        {
                            // 既に存在している同名ファイルは削除
                            File.Delete(trsFile);
                        }
                        File.Move(option.FilePath, trsFile);
                    }
                    if (option.WithRelated)
                    {
                        var body = Path.GetFileNameWithoutExtension(option.FilePath);
                        var tsext = Path.GetExtension(option.FilePath);
                        var srcDir = Path.GetDirectoryName(option.FilePath);
                        foreach (var ext in ServerSupport.GetFileExtentions(null, true))
                        {
                            string srcPath = srcDir + Path.DirectorySeparatorChar + body + ext;
                            string dstPath = trsDir + Path.DirectorySeparatorChar + body + ext;
                            if (File.Exists(srcPath))
                            {
                                if (File.Exists(dstPath))
                                {
                                    // 既に存在している同名ファイルは削除
                                    File.Delete(dstPath);
                                }
                                File.Move(srcPath, dstPath);
                            }
                        }
                    }
                }

                break;
            }
            server.Finish();
        }

        private string UniqueId()
        {
            return Environment.MachineName +
                System.Diagnostics.Process.GetCurrentProcess().Id;
        }

        #region IAddTaskServer

        public void Finish()
        {
            // 何もしない
        }

        public Task OnAddResult(string requestId)
        {
            if (addRequest != null && addRequest.RequestId == requestId)
            {
                okReceived = true;
            }
            else if (resetRequest != null && resetRequest.RequestId == requestId)
            {
                okReceived = true;
            }
            return Task.FromResult(0);
        }

        public Task OnOperationResult(OperationResult result)
        {
            Console.WriteLine(result.Message);
            return Task.FromResult(0);
        }

        #endregion
    }

    class AddTaskMain
    {
        static void Main(string[] args)
        {
            try
            {
                TaskSupport.SetSynchronizationContext();
                AsyncExec(new GUIOPtion(args));
                TaskSupport.EnterMessageLoop();
            }
            catch (Exception e)
            {
                Console.WriteLine(e.Message);
                GUIOPtion.PrintHelp();
                return;
            }
        }

        static async void AsyncExec(GUIOPtion option)
        {
            try
            {
                await new AddTask() { option = option }.Exec();
            }
            catch(Exception e)
            {
                Console.WriteLine(e.Message);
            }
            TaskSupport.Finish();
        }
    }
}

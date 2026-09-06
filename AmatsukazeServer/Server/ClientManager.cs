using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Lib;

namespace Amatsukaze.Server
{
    public class Client : NotificationBase
    {
        private ClientManager manager;
        private TcpClient client;
        private NetworkStream stream;

        public string HostName { get; private set; }
        public int Port { get; private set; }

        // 切断後も一時的な一覧から安全に参照できるよう、接続時の情報を保持する。
        public IPEndPoint RemoteIP { get; }
        public IPEndPoint LocalIP { get; }

        #region TotalSendCount変更通知プロパティ
        private int _TotalSendCount;

        public int TotalSendCount {
            get { return _TotalSendCount; }
            set {
                if (_TotalSendCount == value)
                    return;
                _TotalSendCount = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region TotalRecvCount変更通知プロパティ
        private int _TotalRecvCount;

        public int TotalRecvCount {
            get { return _TotalRecvCount; }
            set {
                if (_TotalRecvCount == value)
                    return;
                _TotalRecvCount = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        public Client(TcpClient client, ClientManager manager)
        {
            this.manager = manager;
            this.client = client;
            this.stream = client.GetStream();
            RemoteIP = (IPEndPoint)client.Client.RemoteEndPoint;
            LocalIP = (IPEndPoint)client.Client.LocalEndPoint;

            var endPoint = (IPEndPoint)client.Client.RemoteEndPoint;
            try
            {
                // クライアントの名前を取得
                HostName = Dns.GetHostEntry(endPoint.Address).HostName;
            }
            catch(Exception)
            {
                // 名前を取得できなかったらIPアドレスをそのまま使う
                HostName = endPoint.Address.ToString();
            }
            Port = endPoint.Port;

            Util.AddLog("クライアント(" + HostName + ":" + Port + ")と接続", null);
        }

        public async Task Start()
        {
            try
            {
                while (true)
                {
                    var rpc = await RPCTypes.Deserialize(stream);
                    manager.OnRequestReceived(this, rpc.id, rpc.arg);
                    TotalRecvCount++;
                }
            }
            catch (Exception)
            {
                Util.AddLog("クライアント(" + HostName + ":" + Port + ")との接続が切れました", null);
                Close();
            }
            manager.OnClientClosed(this);
        }

        public void Close()
        {
            // 送受信と終了処理から同時に呼ばれても、一度だけ閉じる。
            Interlocked.Exchange(ref client, null)?.Close();
        }

        public NetworkStream GetStream()
        {
            return stream;
        }
    }

    public class ClientManager : NotificationBase, IUserClient
    {
        private TcpListener listener;
        private bool finished = false;
        private readonly object clientListLock = new object();

        // WPF の一覧読み取りにも、内部の追加・削除と同じ同期を適用する。
        public object ClientListSyncRoot => clientListLock;

        public ObservableCollection<Client> ClientList { get; private set; }

        private IEncodeServer server;

        public ClientManager(IEncodeServer server)
        {
            this.server = server;
            ClientList = new ObservableCollection<Client>();
        }

        private Client[] GetClientSnapshot()
        {
            // 呼び出し元の処理中だけ使用し、フィールドやキャッシュには保存しない。
            lock (clientListLock)
            {
                return ClientList.ToArray();
            }
        }

        private int ClientCount
        {
            get { lock (clientListLock) { return ClientList.Count; } }
        }

        public void Finish()
        {
            finished = true;
            if (listener != null)
            {
                listener.Stop();
                listener = null;

                foreach (var client in GetClientSnapshot())
                {
                    client.Close();
                }
            }
        }

        public async Task Listen(int port)
        {
            int errorCount = 0;

            while (finished == false)
            {
                listener = new TcpListener(IPAddress.Any, port);
                listener.Start();
                try
                {
                    var local = (listener.LocalEndpoint as IPEndPoint)?.ToString() ?? "<unknown>";
                    Util.AddLog($"サーバ開始しました。ポート: {port} (LocalEndPoint={local})", null);
                }
                catch
                {
                    Util.AddLog("サーバ開始しました。ポート: " + port, null);
                }

                try
                {
                    while (true)
                    {
                        var client = new Client(await listener.AcceptTcpClientAsync(), this);
                        Util.AddLog($"[ClientManager] 接続受付: {client.RemoteIP}, 現在クライアント数: {ClientCount}", null);
                        lock (clientListLock)
                        {
                            ClientList.Add(client);
                        }
                        Util.AddLog($"[ClientManager] 接続登録完了: {client.RemoteIP}, 登録後クライアント数: {ClientCount}", null);
                        // 登録後に受信を開始する。終了時は Start 内で一覧から削除する。
                        _ = client.Start();
                        errorCount = 0;
                    }
                }
                catch (Exception e)
                {
                    if (finished == false)
                    {
                        Util.AddLog("Listen中にエラーが発生", e);

                        // 一定時間待つ
                        await Task.Delay((++errorCount) * 5 * 1000);
                    }
                }
                finally
                {
                    try
                    {
                        listener.Stop();
                    }
                    catch { }
                }
            }
        }

        private static bool IsRemoteHost(IPHostEntry iphostentry, IPAddress address)
        {
            IPHostEntry other = null;
            try
            {
                other = Dns.GetHostEntry(address);
            }
            catch
            {
                return true;
            }
            foreach (IPAddress addr in other.AddressList)
            {
                if (IPAddress.IsLoopback(addr) || Array.IndexOf(iphostentry.AddressList, addr) != -1)
                {
                    return false;
                }
            }
            return true;
        }

        // ローカル接続しているクライアントがいるか？
        public bool HasLocalClient()
        {
            IPHostEntry iphostentry = Dns.GetHostEntry(Dns.GetHostName());
            return GetClientSnapshot().Any(client => IsRemoteHost(iphostentry, client.RemoteIP.Address) == false);
        }

        public byte[] GetMacAddress()
        {
            // リモートのクライアントを見つけて、
            // 接続に使っているNICのMACアドレスを取得する
            IPHostEntry iphostentry = Dns.GetHostEntry(Dns.GetHostName());
            foreach (var client in GetClientSnapshot())
            {
                if (IsRemoteHost(iphostentry, client.RemoteIP.Address))
                {
                    return ServerSupport.GetMacAddress(client.LocalIP.Address);
                }
            }
            return null;
        }

        private async Task Send(RPCMethodId id, object obj)
        {
            byte[] bytes = RPCTypes.Serialize(id, obj);
            //Util.AddLog($"[ClientManager] 送信準備: {id}, バイト数: {bytes.Length}, クライアント数: {ClientList.Count}", null);
            var clients = GetClientSnapshot();
            try
            {
                foreach (var client in clients)
                {
                    try
                    {
                        //Util.AddLog($"[ClientManager] 送信中: {id} -> {client.RemoteIP}", null);
                        await client.GetStream().WriteAsync(bytes, 0, bytes.Length);
                        //Util.AddLog($"[ClientManager] 送信完了: {id} -> {client.RemoteIP}", null);
                        client.TotalSendCount++;
                    }
                    catch (Exception)
                    {
                        Util.AddLog("クライアント(" +
                            client.HostName + ":" + client.Port + ")との接続が切れました", null);
                        client.Close();
                        OnClientClosed(client);
                    }
                }
            }
            finally
            {
                // 非同期処理の Task が保持されても、一時配列に接続の参照を残さない。
                Array.Clear(clients, 0, clients.Length);
            }
        }

        internal void OnRequestReceived(Client client, RPCMethodId methodId, object arg)
        {
            Util.AddLog($"[ClientManager] 要求受信: {methodId}, 登録クライアント数: {ClientCount}", null);
            switch (methodId)
            {
                case RPCMethodId.SetProfile:
                    server.SetProfile((ProfileUpdate)arg);
                    break;
                case RPCMethodId.SetAutoSelect:
                    server.SetAutoSelect((AutoSelectUpdate)arg);
                    break;
                case RPCMethodId.AddQueue:
                    server.AddQueue((AddQueueRequest)arg);
                    break;
                case RPCMethodId.ChangeItem:
                    server.ChangeItem((ChangeItemData)arg);
                    break;
                case RPCMethodId.ChangeItemTask:
                    server.ChangeItemTask((ChangeItemData)arg);
                    break;
                case RPCMethodId.PauseEncode:
                    server.PauseEncode((PauseRequest)arg);
                    break;
                case RPCMethodId.CancelAddQueue:
                    server.CancelAddQueue();
                    break;
                case RPCMethodId.CancelSleep:
                    server.CancelSleep();
                    break;
                case RPCMethodId.SetCommonData:
                    server.SetCommonData((CommonData)arg);
                    break;
                case RPCMethodId.SetServiceSetting:
                    server.SetServiceSetting((ServiceSettingUpdate)arg);
                    break;
                case RPCMethodId.AddDrcsMap:
                    server.AddDrcsMap((DrcsImage)arg);
                    break;
                case RPCMethodId.EndServer:
                    server.EndServer();
                    break;
                case RPCMethodId.Request:
                    Debug.Print($"[ClientManager] Request処理開始: {((ServerRequest)arg).ToDebugString()}, クライアント数: {ClientCount}");
                    server.Request((ServerRequest)arg);
                    Debug.Print($"[ClientManager] Request処理完了: {((ServerRequest)arg).ToDebugString()}, クライアント数: {ClientCount}");
                    break;
                case RPCMethodId.RequestLogFile:
                    server.RequestLogFile((LogFileRequest)arg);
                    break;
                case RPCMethodId.RequestLogFilePath:
                    server.RequestLogFilePath((LogFileRequest)arg);
                    break;
                case RPCMethodId.RequestLogoData:
                    server.RequestLogoData((string)arg);
                    break;
                case RPCMethodId.RequestDrcsImages:
                    server.RequestDrcsImages();
                    break;
                case RPCMethodId.SendLogoFile:
                    server.SendLogoFile((LogoFileData)arg);
                    break;
            }
        }

        internal void OnClientClosed(Client client)
        {
            int index;
            int count;
            lock (clientListLock)
            {
                index = ClientList.IndexOf(client);
                if (index < 0) return;
                ClientList.RemoveAt(index);
                count = ClientList.Count;
            }
            // 切断済みでも保持される接続情報でログを記録する。
            Util.AddLog($"[ClientManager] 接続終了: index={index}, HostName={client?.HostName ?? "<null>"}:{client?.Port ?? -1}, 残りクライアント数: {count}", null);
        }

        #region IUserClient
        public Task OnUIData(UIData data)
        {
            return Send(RPCMethodId.OnUIData, data);
        }

        public Task OnConsoleUpdate(ConsoleUpdate str)
        {
            return Send(RPCMethodId.OnConsoleUpdate, str);
        }

        public Task OnEncodeState(EncodeState state)
        {
            return Send(RPCMethodId.OnEncodeState, state);
        }

        public Task OnLogFile(string str)
        {
            return Send(RPCMethodId.OnLogFile, str);
        }

        public Task OnLogFilePath(LogFilePathResponse response)
        {
            return Send(RPCMethodId.OnLogFilePath, response);
        }

        public Task OnCommonData(CommonData data)
        {
            return Send(RPCMethodId.OnCommonData, data);
        }

        public Task OnProfile(ProfileUpdate data)
        {
            return Send(RPCMethodId.OnProfile, data);
        }

        public Task OnAutoSelect(AutoSelectUpdate data)
        {
            return Send(RPCMethodId.OnAutoSelect, data);
        }

        public Task OnServiceSetting(ServiceSettingUpdate service)
        {
            return Send(RPCMethodId.OnServiceSetting, service);
        }

        public Task OnLogoData(LogoData logoData)
        {
            return Send(RPCMethodId.OnLogoData, logoData);
        }

        public Task OnDrcsData(DrcsImageUpdate update)
        {
            return Send(RPCMethodId.OnDrcsData, update);
        }

        public Task OnAddResult(string requestId)
        {
            return Send(RPCMethodId.OnAddResult, requestId);
        }

        public Task OnOperationResult(OperationResult result)
        {
            return Send(RPCMethodId.OnOperationResult, result);
        }
        #endregion
    }
}

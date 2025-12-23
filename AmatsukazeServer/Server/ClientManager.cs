using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Net;
using System.Net.Sockets;
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

        public IPEndPoint RemoteIP {
            get {
                return (IPEndPoint)client.Client.RemoteEndPoint;
            }
        }

        public IPEndPoint LocalIP {
            get {
                return (IPEndPoint)client.Client.LocalEndPoint;
            }
        }

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
            if (client != null)
            {
                client.Close();
                client = null;
            }
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
        private List<Task> receiveTask = new List<Task>();

        public ObservableCollection<Client> ClientList { get; private set; }

        private IEncodeServer server;

        public ClientManager(IEncodeServer server)
        {
            this.server = server;
            ClientList = new ObservableCollection<Client>();
        }

        public void Finish()
        {
            if (listener != null)
            {
                listener.Stop();
                listener = null;

                foreach (var client in ClientList)
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
                        Util.AddLog($"[ClientManager] 接続受付: {client.RemoteIP}, 現在クライアント数: {ClientList.Count}", null);
                        ClientList.Add(client);
                        Util.AddLog($"[ClientManager] 接続登録完了: {client.RemoteIP}, 登録後クライアント数: {ClientList.Count}", null);
                        receiveTask.Add(client.Start());
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
            return ClientList.Any(client => IsRemoteHost(iphostentry, client.RemoteIP.Address) == false);
        }

        public byte[] GetMacAddress()
        {
            // リモートのクライアントを見つけて、
            // 接続に使っているNICのMACアドレスを取得する
            IPHostEntry iphostentry = Dns.GetHostEntry(Dns.GetHostName());
            foreach (var client in ClientList)
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
            foreach (var client in ClientList.ToArray())
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

        internal void OnRequestReceived(Client client, RPCMethodId methodId, object arg)
        {
            Util.AddLog($"[ClientManager] 要求受信: {methodId}, 登録クライアント数: {ClientList.Count}", null);
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
                    Debug.Print($"[ClientManager] Request処理開始: {((ServerRequest)arg).ToDebugString()}, クライアント数: {ClientList.Count}");
                    server.Request((ServerRequest)arg);
                    Debug.Print($"[ClientManager] Request処理完了: {((ServerRequest)arg).ToDebugString()}, クライアント数: {ClientList.Count}");
                    break;
                case RPCMethodId.RequestLogFile:
                    server.RequestLogFile((LogFileRequest)arg);
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
            int index = ClientList.IndexOf(client);
            if (index >= 0)
            {
                receiveTask.RemoveAt(index);
                ClientList.RemoveAt(index);
                // client.Close() 済みだと client.RemoteIP が null 参照になる可能性があるため参照しない
                Util.AddLog($"[ClientManager] 接続終了: index={index}, HostName={client?.HostName ?? "<null>"}:{client?.Port ?? -1}, 残りクライアント数: {ClientList.Count}", null);
            }
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

using Amatsukaze.Models;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Livet.Commands;
using Livet.EventListeners;
using Livet.Messaging;
using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json;

namespace Amatsukaze.ViewModels
{
    public class MakeScriptViewModel : NamedViewModel
    {
        /* コマンド、プロパティの定義にはそれぞれ 
         * 
         *  lvcom   : ViewModelCommand
         *  lvcomn  : ViewModelCommand(CanExecute無)
         *  llcom   : ListenerCommand(パラメータ有のコマンド)
         *  llcomn  : ListenerCommand(パラメータ有のコマンド・CanExecute無)
         *  lprop   : 変更通知プロパティ(.NET4.5ではlpropn)
         *  
         * を使用してください。
         * 
         * Modelが十分にリッチであるならコマンドにこだわる必要はありません。
         * View側のコードビハインドを使用しないMVVMパターンの実装を行う場合でも、ViewModelにメソッドを定義し、
         * LivetCallMethodActionなどから直接メソッドを呼び出してください。
         * 
         * ViewModelのコマンドを呼び出せるLivetのすべてのビヘイビア・トリガー・アクションは
         * 同様に直接ViewModelのメソッドを呼び出し可能です。
         */

        /* ViewModelからViewを操作したい場合は、View側のコードビハインド無で処理を行いたい場合は
         * Messengerプロパティからメッセージ(各種InteractionMessage)を発信する事を検討してください。
         */

        /* Modelからの変更通知などの各種イベントを受け取る場合は、PropertyChangedEventListenerや
         * CollectionChangedEventListenerを使うと便利です。各種ListenerはViewModelに定義されている
         * CompositeDisposableプロパティ(LivetCompositeDisposable型)に格納しておく事でイベント解放を容易に行えます。
         * 
         * ReactiveExtensionsなどを併用する場合は、ReactiveExtensionsのCompositeDisposableを
         * ViewModelのCompositeDisposableプロパティに格納しておくのを推奨します。
         * 
         * LivetのWindowテンプレートではViewのウィンドウが閉じる際にDataContextDisposeActionが動作するようになっており、
         * ViewModelのDisposeが呼ばれCompositeDisposableプロパティに格納されたすべてのIDisposable型のインスタンスが解放されます。
         * 
         * ViewModelを使いまわしたい時などは、ViewからDataContextDisposeActionを取り除くか、発動のタイミングをずらす事で対応可能です。
         */

        /* UIDispatcherを操作する場合は、DispatcherHelperのメソッドを操作してください。
         * UIDispatcher自体はApp.xaml.csでインスタンスを確保してあります。
         * 
         * LivetのViewModelではプロパティ変更通知(RaisePropertyChanged)やDispatcherCollectionを使ったコレクション変更通知は
         * 自動的にUIDispatcher上での通知に変換されます。変更通知に際してUIDispatcherを操作する必要はありません。
         */
        public ClientModel Model { get; set; }

        public void Initialize() {
        }

        public bool IsRemoteClient {
            get {
                return App.Option.LaunchType == Server.LaunchType.Client;
            }
        }

        public List<ScriptTypeOption> ScriptTypeOptions { get; } = new List<ScriptTypeOption>()
        {
            new ScriptTypeOption() { Value = "bat", Label = "bat (Windowsバッチ)" },
            new ScriptTypeOption() { Value = "sh", Label = "sh (シェルスクリプト)" }
        };

        #region SelectedScriptType変更通知プロパティ
        private string _SelectedScriptType = "bat";

        public string SelectedScriptType {
            get { return _SelectedScriptType; }
            set {
                if (_SelectedScriptType == value)
                    return;
                _SelectedScriptType = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region Description変更通知プロパティ
        private string _Description;

        public string Description {
            get { return _Description; }
            set { 
                if (_Description == value)
                    return;
                _Description = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        #region MakeBatchFileCommand
        private ViewModelCommand _MakeBatchFileCommand;

        public ViewModelCommand MakeBatchFileCommand {
            get {
                if (_MakeBatchFileCommand == null)
                {
                    _MakeBatchFileCommand = new ViewModelCommand(MakeBatchFile);
                }
                return _MakeBatchFileCommand;
            }
        }

        public async void MakeBatchFile()
        {
            Description = "";

            var data = ServerSupport.DeepCopy(Model.MakeScriptData.Model);
            data.Profile = DisplayProfile.GetProfileName(Model.MakeScriptData.SelectedProfile);

            string subnet = null;
            string mac = null;
            if (IsRemoteClient && data.IsWakeOnLan)
            {
                var localIP = Model.LocalIP;
                if (localIP == null)
                {
                    Description = "IPアドレス取得に失敗";
                    return;
                }
                if (localIP.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
                {
                    Description = "IPv4以外の接続には対応していません";
                    return;
                }
                var subnetaddr = ServerSupport.GetSubnetMask(((IPEndPoint)localIP).Address);
                if (subnetaddr == null)
                {
                    Description = "サブネットマスク取得に失敗";
                    return;
                }
                subnet = subnetaddr.ToString();
                var macbytes = Model.MacAddress;
                if (macbytes == null)
                {
                    Description = "MACアドレス取得に失敗";
                    return;
                }
                mac = string.Join(":", macbytes.Select(s => s.ToString("X")));
            }

            var scriptType = GetNormalizedScriptType();

            var saveFileDialog = new SaveFileDialog();
            saveFileDialog.Filter = "バッチファイル(.bat)|*.bat|シェルスクリプト(.sh)|*.sh|All Files (*.*)|*.*";
            saveFileDialog.FilterIndex = scriptType == "sh" ? 2 : 1;
            saveFileDialog.DefaultExt = scriptType;
            saveFileDialog.AddExtension = true;
            saveFileDialog.FileName = scriptType == "sh" ? "AmatsukazeAddTask.sh" : "AmatsukazeAddTask.bat";
            bool? result = saveFileDialog.ShowDialog();
            if (result != true)
            {
                return;
            }

            var outputPath = EnsureScriptExtension(saveFileDialog.FileName, saveFileDialog.FilterIndex, scriptType);

            try
            {
                if (IsRemoteClient)
                {
                    var port = RestApiHost.GetEnabledPort(Model.ServerPort);
                    var baseUrl = $"http://{Model.ServerIP}:{port}";
                    using var http = new HttpClient { BaseAddress = new Uri(baseUrl) };
                    var req = new MakeScriptGenerateRequest
                    {
                        MakeScriptData = data,
                        TargetHost = "remote",
                        ScriptType = scriptType,
                        RemoteHost = Model.ServerIP,
                        Subnet = subnet,
                        Mac = mac
                    };
                    var response = await http.PostAsJsonAsync("/api/makescript/file", req, new JsonSerializerOptions
                    {
                        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
                    });
                    if (!response.IsSuccessStatusCode)
                    {
                        var error = await response.Content.ReadAsStringAsync();
                        Description = string.IsNullOrWhiteSpace(error) ? "バッチファイル作成に失敗" : error;
                        return;
                    }
                    var content = await response.Content.ReadAsStringAsync();
                    File.WriteAllText(outputPath, content, Util.AmatsukazeDefaultEncoding);
                }
                else
                {
                    var serverPort = Model.ServerPort > 0 ? Model.ServerPort : ServerSupport.DEFAULT_PORT;
                    if (!MakeScriptBuilder.TryBuild(
                        data,
                        "local",
                        scriptType,
                        Model.ServerIP,
                        subnet,
                        mac,
                        serverPort,
                        Util.IsServerWindows(),
                        AppContext.BaseDirectory,
                        Directory.GetCurrentDirectory(),
                        out var content,
                        out var error))
                    {
                        Description = string.IsNullOrWhiteSpace(error) ? "バッチファイル作成に失敗" : error;
                        return;
                    }
                    File.WriteAllText(outputPath, content, Util.AmatsukazeDefaultEncoding);
                }
            }
            catch (Exception e)
            {
                Description = "バッチファイル作成に失敗: " + e.Message;
                return;
            }

            var resvm = new MakeBatchResultViewModel() { Path = outputPath };

            await Messenger.RaiseAsync(new TransitionMessage(
                typeof(Views.MakeBatchResultWindow), resvm, TransitionMode.Modal, "Key"));

            await Model.SendMakeScriptData();
        }
        #endregion

        private class MakeScriptGenerateRequest
        {
            public MakeScriptData MakeScriptData { get; set; }
            public string TargetHost { get; set; }
            public string ScriptType { get; set; }
            public string RemoteHost { get; set; }
            public string Subnet { get; set; }
            public string Mac { get; set; }
        }

        public class ScriptTypeOption
        {
            public string Value { get; set; }
            public string Label { get; set; }
        }

        private string GetNormalizedScriptType()
        {
            return string.Equals(SelectedScriptType, "sh", StringComparison.OrdinalIgnoreCase) ? "sh" : "bat";
        }

        private static string EnsureScriptExtension(string fileName, int filterIndex, string defaultScriptType)
        {
            if (!string.IsNullOrWhiteSpace(Path.GetExtension(fileName)))
            {
                return fileName;
            }

            var scriptType = filterIndex == 2 ? "sh" : defaultScriptType;
            return fileName + "." + scriptType;
        }

        #region StopServerCommand
        private ViewModelCommand _StopServerCommand;

        public ViewModelCommand StopServerCommand {
            get {
                if (_StopServerCommand == null)
                {
                    _StopServerCommand = new ViewModelCommand(StopServer);
                }
                return _StopServerCommand;
            }
        }

        public async void StopServer()
        {
            if (ServerSupport.IsLocalIP(Model.ServerIP))
            {
                var message = new ConfirmationMessage(
                    "AmatsukazeServerを終了しますか？",
                    "AmatsukazeServer",
                    System.Windows.MessageBoxImage.Information,
                    System.Windows.MessageBoxButton.OKCancel,
                    "Confirm");

                await Messenger.RaiseAsync(message);

                if (message.Response == true)
                {
                    await Model.Server?.EndServer();
                }
            }
            else
            {
                //
            }
        }
        #endregion

    }
}

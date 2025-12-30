using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.ComponentModel;

using Livet;
using Livet.Commands;
using Livet.Messaging;
using Livet.Messaging.IO;
using Livet.EventListeners;
using Livet.Messaging.Windows;

using Amatsukaze.Models;
using Amatsukaze.Lib;
using Amatsukaze.Server;
using System.IO;
using System.Threading.Tasks;
using System.Windows;

namespace Amatsukaze.ViewModels
{
    public class ServiceSettingViewModel : NamedViewModel
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

        public void Initialize()
        {
        }

        #region SelectedServiceIndex変更通知プロパティ
        private int _SelectedServiceIndex = -1;

        public int SelectedServiceIndex {
            get { return _SelectedServiceIndex; }
            set { 
                if (_SelectedServiceIndex == value)
                    return;
                _SelectedServiceIndex = value;
                RaisePropertyChanged("SelectedServiceItem");
                RaisePropertyChanged("SelectedLogoItem");
                RaisePropertyChanged();
            }
        }

        public DisplayService SelectedServiceItem
        {
            get
            {
                if (_SelectedServiceIndex >= 0 && _SelectedServiceIndex < Model.ServiceSettings.Count)
                {
                    return Model.ServiceSettings[_SelectedServiceIndex];
                }
                return null;
            }
        }
        #endregion

        #region SelectedLogoIndex変更通知プロパティ
        private int _SelectedLogoIndex = -1;

        public int SelectedLogoIndex
        {
            get { return _SelectedLogoIndex; }
            set
            {
                if (_SelectedLogoIndex == value)
                    return;
                _SelectedLogoIndex = value;
                RaisePropertyChanged("SelectedLogoItem");
                RaisePropertyChanged();
            }
        }

        public DisplayLogo SelectedLogoItem
        {
            get
            {
                var service = SelectedServiceItem;
                if(service != null && service.LogoList != null &&
                    _SelectedLogoIndex >= 0 &&
                    _SelectedLogoIndex < service.LogoList.Length)
                {
                    return service.LogoList[_SelectedLogoIndex];
                }
                return null;
            }
        }
        #endregion

        #region ApplyDateCommand
        private ViewModelCommand _ApplyDateCommand;

        public ViewModelCommand ApplyDateCommand {
            get {
                if (_ApplyDateCommand == null)
                {
                    _ApplyDateCommand = new ViewModelCommand(ApplyDate);
                }
                return _ApplyDateCommand;
            }
        }

        public void ApplyDate()
        {
            var logo = SelectedLogoItem;
            if(logo != null)
            {
                logo.ApplyDate();
            }
        }
        #endregion

        #region AddNoLogoCommand
        private ViewModelCommand _AddNoLogoCommand;

        public ViewModelCommand AddNoLogoCommand {
            get {
                if (_AddNoLogoCommand == null)
                {
                    _AddNoLogoCommand = new ViewModelCommand(AddNoLogo);
                }
                return _AddNoLogoCommand;
            }
        }

        public void AddNoLogo()
        {
            var service = SelectedServiceItem;
            if(service != null)
            {
                Model.Server?.SetServiceSetting(new ServiceSettingUpdate() {
                    Type = ServiceSettingUpdateType.AddNoLogo,
                    ServiceId = service.Data.ServiceId
                });
            }
        }
        #endregion

        #region RemoveNoLogoCommand
        private ViewModelCommand _RemoveNoLogoCommand;

        public ViewModelCommand RemoveNoLogoCommand {
            get {
                if (_RemoveNoLogoCommand == null)
                {
                    _RemoveNoLogoCommand = new ViewModelCommand(RemoveNoLogo);
                }
                return _RemoveNoLogoCommand;
            }
        }

        public void RemoveNoLogo()
        {
            var logo = SelectedLogoItem;
            if (logo != null)
            {
                if(logo.Setting.FileName == LogoSetting.NO_LOGO)
                {
                    Model.RemoveLogo(logo);
                }
            }
        }
        #endregion

        #region RemoveServiceSettingCommand
        private ViewModelCommand _RemoveServiceSettingCommand;

        public ViewModelCommand RemoveServiceSettingCommand {
            get {
                if (_RemoveServiceSettingCommand == null)
                {
                    _RemoveServiceSettingCommand = new ViewModelCommand(RemoveServiceSetting);
                }
                return _RemoveServiceSettingCommand;
            }
        }

        public void RemoveServiceSetting()
        {
            var service = SelectedServiceItem;
            if (service != null)
            {
                Model.Server?.SetServiceSetting(new ServiceSettingUpdate() {
                    Type = ServiceSettingUpdateType.Remove,
                    ServiceId = service.Data.ServiceId
                });
            }
        }
        #endregion

        #region AddServiceSettingCommand
        private ViewModelCommand _AddServiceSettingCommand;

        public ViewModelCommand AddServiceSettingCommand
        {
            get
            {
                if (_AddServiceSettingCommand == null)
                {
                    _AddServiceSettingCommand = new ViewModelCommand(AddServiceSetting);
                }
                return _AddServiceSettingCommand;
            }
        }

        public async void AddServiceSetting()
        {
            if (Model == null)
            {
                return;
            }

            var vm = new NewServiceSettingViewModel()
            {
                Model = Model,
                IsDuplicateSid = sid => Model.ServiceSettings.Any(s => s?.Data?.ServiceId == sid),
            };

            await Messenger.RaiseAsync(new TransitionMessage(
                typeof(Views.NewServiceSettingWindow), vm, TransitionMode.Modal, "FromServiceSetting"));

            if (!vm.Success)
            {
                return;
            }

            var element = new ServiceSettingElement()
            {
                ServiceId = vm.ServiceId,
                ServiceName = vm.ServiceName,
                DisableCMCheck = false,
                JLSCommand = Model.JlsCommandFiles?.FirstOrDefault(),
                JLSOption = "",
                LogoSettings = new List<LogoSetting>(),
            };

            await (Model.Server?.SetServiceSetting(new ServiceSettingUpdate()
            {
                Type = ServiceSettingUpdateType.Update,
                ServiceId = element.ServiceId,
                Data = element,
            }) ?? System.Threading.Tasks.Task.FromResult(0));
        }
        #endregion

        public async Task ImportLogoFilesAsync(IEnumerable<string> paths)
        {
            if (Model?.Server == null)
            {
                MessageBox.Show("サーバーに接続されていません。");
                return;
            }

            var lgdFiles = (paths ?? Array.Empty<string>())
                .Where(p => !string.IsNullOrWhiteSpace(p))
                .ToArray();
            if (lgdFiles.Length == 0)
            {
                return;
            }

            async Task<int?> EnsureServiceForSidAsync(int? fixedSid, bool lockSid)
            {
                // 既に選択されているならそれを使う
                if (SelectedServiceItem != null)
                {
                    return SelectedServiceItem.Data.ServiceId;
                }

                var vm = new NewServiceSettingViewModel()
                {
                    Model = Model,
                    IsDuplicateSid = sid => Model.ServiceSettings.Any(s => s?.Data?.ServiceId == sid),
                };
                if (fixedSid.HasValue)
                {
                    vm.SidText = fixedSid.Value.ToString();
                    vm.IsSidLocked = lockSid;
                }

                await Messenger.RaiseAsync(new TransitionMessage(
                    typeof(Views.NewServiceSettingWindow), vm, TransitionMode.Modal, "FromServiceSetting"));

                if (!vm.Success)
                {
                    return null;
                }

                var element = new ServiceSettingElement()
                {
                    ServiceId = vm.ServiceId,
                    ServiceName = vm.ServiceName,
                    DisableCMCheck = false,
                    JLSCommand = Model.JlsCommandFiles?.FirstOrDefault(),
                    JLSOption = "",
                    LogoSettings = new List<LogoSetting>(),
                };

                await Model.Server.SetServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.Update,
                    ServiceId = element.ServiceId,
                    Data = element,
                });

                // 追加したサービスを選択状態にする
                var idx = -1;
                for (int i = 0; i < Model.ServiceSettings.Count; i++)
                {
                    if (Model.ServiceSettings[i]?.Data?.ServiceId == element.ServiceId)
                    {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 0)
                {
                    SelectedServiceIndex = idx;
                }

                return element.ServiceId;
            }

            // .lgdの形式に応じて処理分岐（拡張形式: ServiceId一致条件で登録 / AviUtl形式: 変換して登録）
            using var ctx = new AMTContext();
            int? aviutlImgW = null;
            int? aviutlImgH = null;
            foreach (var srcPath in lgdFiles)
            {
                try
                {
                    if (!File.Exists(srcPath))
                    {
                        continue;
                    }

                    // まず拡張形式として開けるか試す（開ければServiceIdを持つ）
                    int? extendedSid = null;
                    string extendedServiceName = null;
                    try
                    {
                        using (var logo = new LogoFile(ctx, srcPath))
                        {
                            extendedSid = logo.ServiceId;
                            extendedServiceName = logo.Name;
                        }
                    }
                    catch (IOException)
                    {
                        // AviUtl形式（ベースのみ）として扱う
                        extendedSid = null;
                    }

                    if (extendedSid.HasValue)
                    {
                        // Amatsukaze拡張形式
                        var fileSid = extendedSid.Value;
                        string NormalizeServiceName(string name)
                        {
                            if (string.IsNullOrWhiteSpace(name))
                            {
                                return name;
                            }
                            var s = name.Trim();
                            // 末尾が "(...)" の場合は括弧部分を落とす（直前に空白があればそれも除去）
                            if (s.EndsWith(")"))
                            {
                                var idx = s.LastIndexOf('(');
                                if (idx >= 0 && idx < s.Length - 1)
                                {
                                    s = s.Substring(0, idx).TrimEnd();
                                }
                            }
                            return s;
                        }

                        if (SelectedServiceItem != null)
                        {
                            var selectedSid = SelectedServiceItem.Data.ServiceId;
                            if (selectedSid != fileSid)
                            {
                                // 変更: 不一致の場合は一致するサービスへ選択を切り替えて追加する
                                var idx = -1;
                                for (int i = 0; i < Model.ServiceSettings.Count; i++)
                                {
                                    if (Model.ServiceSettings[i]?.Data?.ServiceId == fileSid)
                                    {
                                        idx = i;
                                        break;
                                    }
                                }
                                if (idx >= 0)
                                {
                                    SelectedServiceIndex = idx;
                                }
                                else
                                {
                                    // 一致するサービスが無い場合は、拡張lgdのサービス名で自動登録する
                                    var element = new ServiceSettingElement()
                                    {
                                        ServiceId = fileSid,
                                        ServiceName = string.IsNullOrWhiteSpace(extendedServiceName) ? $"SID{fileSid}" : NormalizeServiceName(extendedServiceName),
                                        DisableCMCheck = false,
                                        JLSCommand = Model.JlsCommandFiles?.FirstOrDefault(),
                                        JLSOption = "",
                                        LogoSettings = new List<LogoSetting>(),
                                    };

                                    await Model.Server.SetServiceSetting(new ServiceSettingUpdate()
                                    {
                                        Type = ServiceSettingUpdateType.Update,
                                        ServiceId = element.ServiceId,
                                        Data = element,
                                    });

                                    // 追加したサービスを選択状態にする
                                    var createdIdx = -1;
                                    for (int i = 0; i < Model.ServiceSettings.Count; i++)
                                    {
                                        if (Model.ServiceSettings[i]?.Data?.ServiceId == element.ServiceId)
                                        {
                                            createdIdx = i;
                                            break;
                                        }
                                    }
                                    if (createdIdx >= 0)
                                    {
                                        SelectedServiceIndex = createdIdx;
                                    }
                                }
                            }
                        }
                        else
                        {
                            // サービス未選択なら、同じServiceIDのサービスがあればそこへ、無ければ固定SIDで新規作成
                            var exists = Model.ServiceSettings.Any(s => s?.Data?.ServiceId == fileSid);
                            if (exists)
                            {
                                // そのサービスを選択しておく（UI反映）
                                for (int i = 0; i < Model.ServiceSettings.Count; i++)
                                {
                                    if (Model.ServiceSettings[i]?.Data?.ServiceId == fileSid)
                                    {
                                        SelectedServiceIndex = i;
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                // 拡張lgdにはサービス名もあるため、ユーザー入力なしで自動登録する
                                var element = new ServiceSettingElement()
                                {
                                    ServiceId = fileSid,
                                    ServiceName = string.IsNullOrWhiteSpace(extendedServiceName) ? $"SID{fileSid}" : NormalizeServiceName(extendedServiceName),
                                    DisableCMCheck = false,
                                    JLSCommand = Model.JlsCommandFiles?.FirstOrDefault(),
                                    JLSOption = "",
                                    LogoSettings = new List<LogoSetting>(),
                                };

                                await Model.Server.SetServiceSetting(new ServiceSettingUpdate()
                                {
                                    Type = ServiceSettingUpdateType.Update,
                                    ServiceId = element.ServiceId,
                                    Data = element,
                                });

                                // 追加したサービスを選択状態にする
                                var idx = -1;
                                for (int i = 0; i < Model.ServiceSettings.Count; i++)
                                {
                                    if (Model.ServiceSettings[i]?.Data?.ServiceId == element.ServiceId)
                                    {
                                        idx = i;
                                        break;
                                    }
                                }
                                if (idx >= 0)
                                {
                                    SelectedServiceIndex = idx;
                                }
                            }
                        }

                        // 拡張形式はServiceIdを書き換えず、そのまま送る（送信パラメータもロゴのServiceIdに合わせる）
                        var data = File.ReadAllBytes(srcPath);
                        await Model.Server.SendLogoFile(new LogoFileData()
                        {
                            Data = data,
                            ServiceId = fileSid,
                            LogoIdx = 1
                        });
                    }
                    else
                    {
                        // AviUtl形式（ベースのみ）: 変換して送信する。サービスIDは選択中（または新規追加）を使う。
                        var targetSid = await EnsureServiceForSidAsync(null, lockSid: false);
                        if (!targetSid.HasValue)
                        {
                            continue;
                        }

                        var tmpPath = Path.Combine(Path.GetTempPath(), "amatsukaze-import-" + Guid.NewGuid().ToString("N") + ".lgd");
                        try
                        {
                            if (aviutlImgW == null || aviutlImgH == null)
                            {
                                var resVM = new LogoResolutionViewModel();
                                await Messenger.RaiseAsync(new TransitionMessage(
                                    typeof(Views.LogoResolutionWindow), resVM, TransitionMode.Modal, "FromServiceSetting"));
                                if (!resVM.Success)
                                {
                                    continue;
                                }
                                aviutlImgW = resVM.ImgW;
                                aviutlImgH = resVM.ImgH;
                            }

                            LogoFile.ConvertAviUtlToExtended(ctx, srcPath, tmpPath, targetSid.Value, aviutlImgW.Value, aviutlImgH.Value);

                            var data = File.ReadAllBytes(tmpPath);
                            await Model.Server.SendLogoFile(new LogoFileData()
                            {
                                Data = data,
                                ServiceId = targetSid.Value,
                                LogoIdx = 1
                            });
                        }
                        finally
                        {
                            try
                            {
                                if (File.Exists(tmpPath))
                                {
                                    File.Delete(tmpPath);
                                }
                            }
                            catch { }
                        }
                    }
                }
                catch (Exception ex)
                {
                    MessageBox.Show(ex.Message);
                }
            }
        }

    }
}

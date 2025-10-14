using Amatsukaze.Models;
using Amatsukaze.Server;
using Livet.Commands;
using Livet.EventListeners;
using System.Windows.Markup;

namespace Amatsukaze.ViewModels
{
    public class SettingViewModel : NamedViewModel
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

        public void Initialize() { }

        #region SendSettingCommand
        private ViewModelCommand _SendSettingCommand;

        public ViewModelCommand SendSettingCommand
        {
            get
            {
                if (_SendSettingCommand == null)
                {
                    _SendSettingCommand = new ViewModelCommand(SendSetting);
                }
                return _SendSettingCommand;
            }
        }

        public void SendSetting()
        {
            Model.SendSetting();
        }
        #endregion

        #region ClearAmatsukazePathCommand
        private ViewModelCommand _ClearAmatsukazePathCommand;

        public ViewModelCommand ClearAmatsukazePathCommand {
            get {
                if (_ClearAmatsukazePathCommand == null)
                {
                    _ClearAmatsukazePathCommand = new ViewModelCommand(ClearAmatsukazePath);
                }
                return _ClearAmatsukazePathCommand;
            }
        }

        public void ClearAmatsukazePath()
        {
            Model.Setting.AmatsukazePath = null;
        }
        #endregion

        #region ClearX264PathCommand
        private ViewModelCommand _ClearX264PathCommand;

        public ViewModelCommand ClearX264PathCommand {
            get {
                if (_ClearX264PathCommand == null)
                {
                    _ClearX264PathCommand = new ViewModelCommand(ClearX264Path);
                }
                return _ClearX264PathCommand;
            }
        }

        public void ClearX264Path()
        {
            Model.Setting.X264Path = null;
        }
        #endregion

        #region ClearX265PathCommand
        private ViewModelCommand _ClearX265PathCommand;

        public ViewModelCommand ClearX265PathCommand {
            get {
                if (_ClearX265PathCommand == null)
                {
                    _ClearX265PathCommand = new ViewModelCommand(ClearX265Path);
                }
                return _ClearX265PathCommand;
            }
        }

        public void ClearX265Path()
        {
            Model.Setting.X265Path = null;
        }
        #endregion

        #region ClearSVTAV1PathCommand
        private ViewModelCommand _ClearSVTAV1PathCommand;

        public ViewModelCommand ClearSVTAV1PathCommand
        {
            get
            {
                if (_ClearSVTAV1PathCommand == null)
                {
                    _ClearSVTAV1PathCommand = new ViewModelCommand(ClearSVTAV1Path);
                }
                return _ClearSVTAV1PathCommand;
            }
        }

        public void ClearSVTAV1Path()
        {
            Model.Setting.SVTAV1Path = null;
        }
        #endregion

        #region ClearMuxerPathCommand
        private ViewModelCommand _ClearMuxerPathCommand;

        public ViewModelCommand ClearMuxerPathCommand {
            get {
                if (_ClearMuxerPathCommand == null)
                {
                    _ClearMuxerPathCommand = new ViewModelCommand(ClearMuxerPath);
                }
                return _ClearMuxerPathCommand;
            }
        }

        public void ClearMuxerPath()
        {
            Model.Setting.MuxerPath = null;
        }
        #endregion

        #region ClearMKVMergePathCommand
        private ViewModelCommand _ClearMKVMergePathCommand;

        public ViewModelCommand ClearMKVMergePathCommand {
            get {
                if (_ClearMKVMergePathCommand == null)
                {
                    _ClearMKVMergePathCommand = new ViewModelCommand(ClearMKVMergePath);
                }
                return _ClearMKVMergePathCommand;
            }
        }

        public void ClearMKVMergePath()
        {
            Model.Setting.MKVMergePath = null;
        }
        #endregion

        #region ClearMP4BoxPathCommand
        private ViewModelCommand _ClearMP4BoxPathCommand;

        public ViewModelCommand ClearMP4BoxPathCommand {
            get {
                if (_ClearMP4BoxPathCommand == null)
                {
                    _ClearMP4BoxPathCommand = new ViewModelCommand(ClearMP4BoxPath);
                }
                return _ClearMP4BoxPathCommand;
            }
        }

        public void ClearMP4BoxPath()
        {
            Model.Setting.MP4BoxPath = null;
        }
        #endregion

        #region ClearTimelineEditorPathCommand
        private ViewModelCommand _ClearTimelineEditorPathCommand;

        public ViewModelCommand ClearTimelineEditorPathCommand {
            get {
                if (_ClearTimelineEditorPathCommand == null)
                {
                    _ClearTimelineEditorPathCommand = new ViewModelCommand(ClearTimelineEditorPath);
                }
                return _ClearTimelineEditorPathCommand;
            }
        }

        public void ClearTimelineEditorPath()
        {
            Model.Setting.TimelineEditorPath = null;
        }
        #endregion

        #region ClearChapterExepathCommand
        private ViewModelCommand _ClearChapterExepathCommand;

        public ViewModelCommand ClearChapterExepathCommand {
            get {
                if (_ClearChapterExepathCommand == null)
                {
                    _ClearChapterExepathCommand = new ViewModelCommand(ClearChapterExepath);
                }
                return _ClearChapterExepathCommand;
            }
        }

        public void ClearChapterExepath()
        {
            Model.Setting.ChapterExePath = null;
        }
        #endregion

        #region ClearJoinLogoScpPathCommand
        private ViewModelCommand _ClearJoinLogoScpPathCommand;

        public ViewModelCommand ClearJoinLogoScpPathCommand {
            get {
                if (_ClearJoinLogoScpPathCommand == null)
                {
                    _ClearJoinLogoScpPathCommand = new ViewModelCommand(ClearJoinLogoScpPath);
                }
                return _ClearJoinLogoScpPathCommand;
            }
        }

        public void ClearJoinLogoScpPath()
        {
            Model.Setting.JoinLogoScpPath = null;
        }
        #endregion

        #region ClearTsreadexPathCommand
        private ViewModelCommand _ClearTsreadexPathCommand;
        
        public ViewModelCommand ClearTsreadexPathCommand
        {
            get {
                if (_ClearTsreadexPathCommand == null)
                {
                    _ClearTsreadexPathCommand = new ViewModelCommand(ClearTsreadexPath);
                }
                return _ClearTsreadexPathCommand;
            }
        }
        public void ClearTsreadexPath()
        {
            Model.Setting.TsReadExPath = null;
        }
        #endregion

        #region ClearB24tovttPathCommand

        private ViewModelCommand _ClearB24tovttPathCommand;

        public ViewModelCommand ClearB24tovttPathCommand
        {
            get {
                if (_ClearB24tovttPathCommand == null)
                {
                    _ClearB24tovttPathCommand = new ViewModelCommand(ClearB24tovttPath);
                }
                return _ClearB24tovttPathCommand;
            }
        }
        public void ClearB24tovttPath()
        {
            Model.Setting.B24ToVttPath = null;
        }
        #endregion

        #region ClearPsisiarcPathCommand

        private ViewModelCommand _ClearPsisiarcPathCommand;

        public ViewModelCommand ClearPsisiarcPathCommand
        {
            get {
                if (_ClearPsisiarcPathCommand == null)
                {
                    _ClearPsisiarcPathCommand = new ViewModelCommand(ClearPsisiarcPath);
                }
                return _ClearPsisiarcPathCommand;
            }
        }
        public void ClearPsisiarcPath()
        {
            Model.Setting.PsisiarcPath = null;
        }
        #endregion

        #region ClearNicoConvASSPathCommand
        private ViewModelCommand _ClearNicoConvASSPathCommand;

        public ViewModelCommand ClearNicoConvASSPathCommand
        {
            get
            {
                if (_ClearNicoConvASSPathCommand == null)
                {
                    _ClearNicoConvASSPathCommand = new ViewModelCommand(ClearNicoConvASSPath);
                }
                return _ClearNicoConvASSPathCommand;
            }
        }

        public void ClearNicoConvASSPath()
        {
            Model.Setting.NicoConvASSPath = null;
        }
        #endregion

        #region ClearTsReplacePathCommand
        private ViewModelCommand _ClearTsReplacePathCommand;

        public ViewModelCommand ClearTsReplacePathCommand
        {
            get
            {
                if (_ClearTsReplacePathCommand == null)
                {
                    _ClearTsReplacePathCommand = new ViewModelCommand(ClearTsReplacePath);
                }
                return _ClearTsReplacePathCommand;
            }
        }

        public void ClearTsReplacePath()
        {
            Model.Setting.TsReplacePath = null;
        }
        #endregion

        #region ClearSCRenamePathCommand
        private ViewModelCommand _ClearSCRenamePathCommand;

        public ViewModelCommand ClearSCRenamePathCommand
        {
            get
            {
                if (_ClearSCRenamePathCommand == null)
                {
                    _ClearSCRenamePathCommand = new ViewModelCommand(ClearSCRenamePath);
                }
                return _ClearSCRenamePathCommand;
            }
        }

        public void ClearSCRenamePath()
        {
            Model.Setting.SCRenamePath = null;
        }
        #endregion

        #region ClearFdkaacPathCommand
        private ViewModelCommand _ClearFdkaacPathCommand;

        public ViewModelCommand ClearFdkaacPathCommand
        {
            get
            {
                if (_ClearFdkaacPathCommand == null)
                {
                    _ClearFdkaacPathCommand = new ViewModelCommand(ClearFdkaacPath);
                }
                return _ClearFdkaacPathCommand;
            }
        }

        public void ClearFdkaacPath()
        {
            Model.Setting.FdkaacPath = null;
        }
        #endregion

        #region ClearOpusEncPathCommand
        private ViewModelCommand _ClearOpusEncPathCommand;

        public ViewModelCommand ClearOpusEncPathCommand
        {
            get
            {
                if (_ClearOpusEncPathCommand == null)
                {
                    _ClearOpusEncPathCommand = new ViewModelCommand(ClearOpusEncPath);
                }
                return _ClearOpusEncPathCommand;
            }
        }

        public void ClearOpusEncPath()
        {
            Model.Setting.OpusEncPath = null;
        }
        #endregion

        #region DeleteNoActionExeCommand
        private ViewModelCommand _DeleteNoActionExeCommand;

        public ViewModelCommand DeleteNoActionExeCommand
        {
            get
            {
                if (_DeleteNoActionExeCommand == null)
                {
                    _DeleteNoActionExeCommand = new ViewModelCommand(DeleteNoActionExe);
                }
                return _DeleteNoActionExeCommand;
            }
        }
        public void DeleteNoActionExe()
        {
            Model.Setting.RemoveNoActionExeList();
        }
        #endregion

        #region AddNoActionExeCommand
        private ViewModelCommand _AddNoActionExeCommand;

        public ViewModelCommand AddNoActionExeCommand
        {
            get
            {
                if (_AddNoActionExeCommand == null)
                {
                    _AddNoActionExeCommand = new ViewModelCommand(AddNoActionExe);
                }
                return _AddNoActionExeCommand;
            }
        }
        public void AddNoActionExe()
        {
            Model.Setting.AddNoActionExeList();
        }
        #endregion


        #region ClearFontFamilyCommand
        private ViewModelCommand _ClearFontFamilyCommand;

        public ViewModelCommand ClearFontFamilyCommand
        {
            get
            {
                if (_ClearFontFamilyCommand == null)
                {
                    _ClearFontFamilyCommand = new ViewModelCommand(ClearFontFamily);
                }
                return _ClearFontFamilyCommand;
            }
        }

        public void ClearFontFamily()
        {
            Model.Setting.ConsoleFont = new System.Windows.Media.FontFamily();
        }
        #endregion


    }
}

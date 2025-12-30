using Amatsukaze.Models;
using Livet;
using Livet.Commands;
using Livet.Messaging.Windows;
using System;

namespace Amatsukaze.ViewModels
{
    public class NewServiceSettingViewModel : ViewModel
    {
        public ClientModel Model { get; set; }

        public Func<int, bool> IsDuplicateSid;

        public bool Success;

        public string Caption { get { return "Amatsukaze サービス設定追加"; } }

        public int ServiceId { get; private set; }

        public void Initialize()
        {
        }

        #region IsSidLocked変更通知プロパティ
        private bool _IsSidLocked;

        public bool IsSidLocked
        {
            get { return _IsSidLocked; }
            set
            {
                if (_IsSidLocked == value)
                {
                    return;
                }
                _IsSidLocked = value;
                RaisePropertyChanged();
            }
        }
        #endregion

        private string Validate()
        {
            if (string.IsNullOrWhiteSpace(_ServiceName))
            {
                return "サービス名を入力してください。";
            }

            if (!TryParseSid(_SidText, out var sid))
            {
                return "チャンネルSIDは正の数字を入力してください。";
            }

            if (IsDuplicateSid != null && IsDuplicateSid(sid))
            {
                return "チャンネルSIDが重複しています。";
            }

            return "";
        }

        private static bool TryParseSid(string text, out int sid)
        {
            if (int.TryParse(text, out sid))
            {
                return sid > 0;
            }
            sid = 0;
            return false;
        }

        #region OkCommand
        private ViewModelCommand _OkCommand;

        public ViewModelCommand OkCommand
        {
            get
            {
                if (_OkCommand == null)
                {
                    _OkCommand = new ViewModelCommand(Ok);
                }
                return _OkCommand;
            }
        }

        public async void Ok()
        {
            var error = Validate();
            Description = error;
            if (!string.IsNullOrEmpty(error))
            {
                return;
            }

            TryParseSid(_SidText, out var sid);
            ServiceId = sid;
            ServiceName = _ServiceName.Trim();

            Success = true;
            await Messenger.RaiseAsync(new WindowActionMessage(WindowAction.Close, "Close"));
        }
        #endregion

        #region CancelCommand
        private ViewModelCommand _CancelCommand;

        public ViewModelCommand CancelCommand
        {
            get
            {
                if (_CancelCommand == null)
                {
                    _CancelCommand = new ViewModelCommand(Cancel);
                }
                return _CancelCommand;
            }
        }

        public async void Cancel()
        {
            Success = false;
            await Messenger.RaiseAsync(new WindowActionMessage(WindowAction.Close, "Close"));
        }
        #endregion

        #region ServiceName変更通知プロパティ
        private string _ServiceName;

        public string ServiceName
        {
            get { return _ServiceName; }
            set
            {
                if (_ServiceName == value)
                {
                    return;
                }
                _ServiceName = value;
                Description = Validate();
                RaisePropertyChanged();
            }
        }
        #endregion

        #region SidText変更通知プロパティ
        private string _SidText;

        public string SidText
        {
            get { return _SidText; }
            set
            {
                if (_SidText == value)
                {
                    return;
                }
                _SidText = value;
                Description = Validate();
                RaisePropertyChanged();
            }
        }
        #endregion

        #region Description変更通知プロパティ
        private string _Description;

        public string Description
        {
            get { return _Description; }
            set
            {
                if (_Description == value)
                {
                    return;
                }
                _Description = value;
                RaisePropertyChanged();
            }
        }
        #endregion
    }
}



using Livet;
using Livet.Commands;
using Livet.Messaging.Windows;
using System;

namespace Amatsukaze.ViewModels
{
    public class LogoResolutionViewModel : ViewModel
    {
        public bool Success { get; private set; }

        public int ImgW { get; private set; }
        public int ImgH { get; private set; }

        public string Caption => "Amatsukaze ロゴ解像度入力";

        public void Initialize()
        {
        }

        private static int RoundDownToEven(int v)
        {
            return (v / 2) * 2;
        }

        private string Validate()
        {
            if (!int.TryParse(_ImgWText, out var w) || w <= 0)
            {
                return "横解像度は正の整数を入力してください。";
            }
            if (!int.TryParse(_ImgHText, out var h) || h <= 0)
            {
                return "縦解像度は正の整数を入力してください。";
            }
            return "";
        }

        #region ImgWText変更通知プロパティ
        private string _ImgWText = "1920";

        public string ImgWText
        {
            get { return _ImgWText; }
            set
            {
                if (_ImgWText == value)
                {
                    return;
                }
                _ImgWText = value;
                Description = Validate();
                RaisePropertyChanged();
            }
        }
        #endregion

        #region ImgHText変更通知プロパティ
        private string _ImgHText = "1080";

        public string ImgHText
        {
            get { return _ImgHText; }
            set
            {
                if (_ImgHText == value)
                {
                    return;
                }
                _ImgHText = value;
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

            int.TryParse(_ImgWText, out var w);
            int.TryParse(_ImgHText, out var h);

            // 奇数の場合は偶数へ切り下げて丸める（2x2前提の処理が多いため）
            var rw = RoundDownToEven(w);
            var rh = RoundDownToEven(h);
            if (rw <= 0 || rh <= 0)
            {
                Description = "解像度が小さすぎます。2以上の値を入力してください。";
                return;
            }

            ImgW = rw;
            ImgH = rh;
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
    }
}



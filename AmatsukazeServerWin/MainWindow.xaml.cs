using System;
using System.Drawing;
using System.Windows;
using System.Windows.Media.Imaging;
using AmatsukazeServerWin.Utilities;

namespace AmatsukazeServerWin
{
    public partial class MainWindow : Window
    {
        public MainWindow(string[] args)
        {
            // Windows固有の実装を設定
            BitmapManager.SetBitmapFactory(new WpfBitmapFactory());
            SystemUtility.SetSystemUtility(new WindowsSystemUtility());

            // 以下は元のコードをそのまま残す
            InitializeComponent();
            // ... 残りのコード
        }
    }
} 
using Amatsukaze.Components;
using System.Windows;

namespace Amatsukaze.Views
{
    /// <summary>
    /// NewServiceSettingWindow.xaml の相互作用ロジック
    /// </summary>
    public partial class NewServiceSettingWindow : Window
    {
        public NewServiceSettingWindow()
        {
            InitializeComponent();
            Utils.SetWindowProperties(this);
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            Utils.SetWindowCenter(this);
        }
    }
}



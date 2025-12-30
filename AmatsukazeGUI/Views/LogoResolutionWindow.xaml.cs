using Amatsukaze.Components;
using System.Windows;

namespace Amatsukaze.Views
{
    /// <summary>
    /// LogoResolutionWindow.xaml の相互作用ロジック
    /// </summary>
    public partial class LogoResolutionWindow : Window
    {
        public LogoResolutionWindow()
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



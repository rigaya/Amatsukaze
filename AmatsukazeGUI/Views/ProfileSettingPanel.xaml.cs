using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;

namespace Amatsukaze.Views
{
    /// <summary>
    /// ProfileSettingPanel.xaml の相互作用ロジック
    /// </summary>
    public partial class ProfileSettingPanel : UserControl
    {
        public ProfileSettingPanel()
        {
            InitializeComponent();
        }

        private void OpenFilterAdjust(object sender, RoutedEventArgs e)
        {
            if (DataContext is not ViewModels.ProfileSettingViewModel vm || vm.Model.SelectedProfile == null)
                return;

            var owner = Window.GetWindow(this);
            if (vm.Model.SelectedProfile.HasUnappliedChanges())
            {
                MessageBox.Show(owner, "未適用の変更があります。適用してからフィルタ調整を開いてください。",
                    "フィルタ調整", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }
            new FilterAdjustSourcesWindow(vm.Model) { Owner = owner }.ShowDialog();
        }

        private void TextBox_PreviewDragOver(object sender, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
            {
                e.Effects = System.Windows.DragDropEffects.Copy;
                e.Handled = true;
            }
        }

        private void TextBox_Drop(object sender, DragEventArgs e)
        {
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            var target = sender as TextBox;
            if (files != null && target != null)
            {
                target.Text = files[0];
            }
        }
    }
}

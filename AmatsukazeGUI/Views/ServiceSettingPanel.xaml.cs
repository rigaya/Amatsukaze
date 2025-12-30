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
    /// ServiceSettingPanel.xaml の相互作用ロジック
    /// </summary>
    public partial class ServiceSettingPanel : UserControl
    {
        public ServiceSettingPanel()
        {
            InitializeComponent();
        }

        private void Calendar_SelectedDatesChanged(object sender, SelectionChangedEventArgs e)
        {
            Mouse.Capture(null);
        }

        private void LogoList_PreviewDragOver(object sender, DragEventArgs e)
        {
            e.Effects = DragDropEffects.None;
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (files != null &&
                files.Any(f => string.Equals(System.IO.Path.GetExtension(f), ".lgd", StringComparison.OrdinalIgnoreCase)))
            {
                e.Effects = DragDropEffects.Copy;
            }

            e.Handled = true;
        }

        private async void LogoList_Drop(object sender, DragEventArgs e)
        {
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (files == null)
            {
                return;
            }

            var lgdFiles = files
                .Where(f => string.Equals(System.IO.Path.GetExtension(f), ".lgd", StringComparison.OrdinalIgnoreCase))
                .ToArray();
            if (lgdFiles.Length == 0)
            {
                return;
            }

            if (DataContext is Amatsukaze.ViewModels.ServiceSettingViewModel vm)
            {
                await vm.ImportLogoFilesAsync(lgdFiles);
            }
        }
    }
}

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
using Amatsukaze.ViewModels;

namespace Amatsukaze.Views
{
    /// <summary>
    /// SettingPanel.xaml の相互作用ロジック
    /// </summary>
    public partial class SettingPanel : UserControl
    {
        public SettingPanel()
        {
            InitializeComponent();
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

        private void TrimAdjustResetDefaults_Click(object sender, RoutedEventArgs e)
        {
            if (!(DataContext is SettingViewModel vm) || vm.Model?.Setting == null)
            {
                return;
            }

            var setting = vm.Model.Setting;
            setting.TrimAdjustPreviewScaleMode = 1;
            setting.TrimAdjustShortcutPrevEditPoint = "ArrowUp";
            setting.TrimAdjustShortcutBack4 = "Alt+ArrowLeft";
            setting.TrimAdjustShortcutBack3 = "Shift+ArrowLeft";
            setting.TrimAdjustShortcutBack2 = "Ctrl+ArrowLeft";
            setting.TrimAdjustShortcutBack1 = "ArrowLeft";
            setting.TrimAdjustShortcutForward1 = "ArrowRight";
            setting.TrimAdjustShortcutForward2 = "Ctrl+ArrowRight";
            setting.TrimAdjustShortcutForward3 = "Shift+ArrowRight";
            setting.TrimAdjustShortcutForward4 = "Alt+ArrowRight";
            setting.TrimAdjustShortcutNextEditPoint = "ArrowDown";
            setting.TrimAdjustShortcutToggleEditPoint = "Space";
            setting.TrimAdjustMoveFramesBack4 = 900;
            setting.TrimAdjustMoveFramesBack3 = 450;
            setting.TrimAdjustMoveFramesBack2 = 300;
            setting.TrimAdjustMoveFramesForward2 = 300;
            setting.TrimAdjustMoveFramesForward3 = 450;
            setting.TrimAdjustMoveFramesForward4 = 900;
        }
    }
}

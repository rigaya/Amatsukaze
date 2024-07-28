using Amatsukaze.Server;
using Amatsukaze.ViewModels;
using System;
using System.Collections.Generic;
using System.IO;
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
using System.Windows.Threading;

namespace Amatsukaze.Views
{
    /// <summary>
    /// QueuePanel.xaml の相互作用ロジック
    /// </summary>
    public partial class QueuePanel : UserControl
    {
        public QueuePanel()
        {
            InitializeComponent();

            // コンテキストメニューの外の名前空間を見えるようにする
            NameScope.SetNameScope(queueMenu, NameScope.GetNameScope(this));
            NameScope.SetNameScope(buttonProfileMenu, NameScope.GetNameScope(this));
        }

        private void ListBox_PreviewDragOver(object sender, DragEventArgs e)
        {
            e.Handled = e.Data.GetDataPresent(DataFormats.FileDrop) ||
                        e.Data.GetDataPresent(DataFormats.Text);
        }

        private void ListBox_Drop(object sender, DragEventArgs e)
        {
            var vm = DataContext as QueueViewModel;
            if (vm != null)
            {
                if (e.Data.GetDataPresent(DataFormats.FileDrop))
                {
                    vm.FileDropped(e.Data.GetData(DataFormats.FileDrop) as string[], false);
                }
            }
        }

        private DispatcherTimer dispatcherTimer;

        private void UserControl_Loaded(object sender, RoutedEventArgs e)
        {
            var window = Window.GetWindow(this);
            window.GotFocus += UpdateTick;
            window.KeyDown += HandleKeyEvent;
            window.KeyUp += HandleKeyEvent;
            queueMenu.KeyDown += HandleKeyEvent;
            queueMenu.KeyUp += HandleKeyEvent;

            dispatcherTimer = new DispatcherTimer(DispatcherPriority.ContextIdle);
            dispatcherTimer.Interval = new TimeSpan(0, 0, 1);
            dispatcherTimer.Tick += new EventHandler(UpdateTick);
            dispatcherTimer.Start();
        }

        private void UpdateTick(object sender, EventArgs e)
        {
            var vm = DataContext as QueueViewModel;
            if (vm != null)
            {
                if (Window.GetWindow(this)?.IsActive ?? false)
                    vm.ShiftDown = (Keyboard.IsKeyDown(Key.LeftShift) || Keyboard.IsKeyDown(Key.RightShift));
            }
        }

        private void HandleKeyEvent(object sender, KeyEventArgs e)
        {
            var vm = DataContext as QueueViewModel;
            if(vm != null)
            {
                if (e.Key == Key.LeftShift || e.Key == Key.RightShift)
                {
                    // Shiftキーだった
                    vm.ShiftDown = e.IsDown;
                }
            }
        }
        bool SaveScrollPosition = false;
        private void QueueList_Loaded(object sender, RoutedEventArgs e)
        {
            var vm = DataContext as QueueViewModel;
            if(vm != null)
            {
                var listBox = sender as ListBox;
                var scrollViewer = ScrollViewerHelper.GetScrollViewer(listBox);

                if (scrollViewer != null)
                {
                    try
                    {
                        scrollViewer.ScrollToVerticalOffset(vm.QueueListScrollVerticalOffset);
                        scrollViewer.ScrollToHorizontalOffset(vm.QueueListScrollHorizontalOffset);
                    }
                    catch { }
                }
                SaveScrollPosition = true;
            }
        }
        private void QueueList_Unloaded(object sender, RoutedEventArgs e)
        {
            SaveScrollPosition = false;
        }

        private void QueueList_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            var vm = DataContext as QueueViewModel;
            if (vm != null && SaveScrollPosition)
            {
                var listBox = sender as ListBox;
                var scrollViewer = ScrollViewerHelper.GetScrollViewer(listBox);

                if (scrollViewer != null)
                {
                    vm.QueueListScrollVerticalOffset = scrollViewer.VerticalOffset;
                    vm.QueueListScrollHorizontalOffset = scrollViewer.HorizontalOffset;
                }
            }
        }
    }

    public static class ScrollViewerHelper
    {
        public static ScrollViewer GetScrollViewer(DependencyObject depObj)
        {
            if (depObj is ScrollViewer) return depObj as ScrollViewer;

            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(depObj); i++)
            {
                var child = VisualTreeHelper.GetChild(depObj, i);
                var result = GetScrollViewer(child);
                if (result != null) return result;
            }
            return null;
        }
    }
}

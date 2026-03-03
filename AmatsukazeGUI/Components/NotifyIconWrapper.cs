using System.ComponentModel;
using System.Windows;
using Hardcodet.Wpf.TaskbarNotification;

namespace Amatsukaze.Components
{
    public class NotifyIconWrapper : Component
    {
        private static readonly Uri IconDefault = new Uri("pack://application:,,,/AmatsukazeGUI;component/ServerIconGrey.ico");
        private static readonly Uri IconRunning = new Uri("pack://application:,,,/AmatsukazeGUI;component/ServerIconBlue.ico");

        private TaskbarIcon notifyIcon;
        public Window Window;

        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        public string Text {
            get { return notifyIcon.ToolTipText; }
            set { notifyIcon.ToolTipText = value; }
        }

        public NotifyIconWrapper()
        {
            Initialize();
        }

        public NotifyIconWrapper(IContainer container)
        {
            container.Add(this);
            Initialize();
        }

        private void Initialize()
        {
            notifyIcon = new TaskbarIcon();
            notifyIcon.IconSource = new System.Windows.Media.Imaging.BitmapImage(IconDefault);
            notifyIcon.ToolTipText = "AmatsukazeServer";
            notifyIcon.TrayMouseDoubleClick += NotifyIcon_TrayMouseDoubleClick;
        }

        /// <summary>キューが稼働中かどうかに応じてタスクトレイアイコンを切り替える。非UIスレッドからも呼び出し可能。</summary>
        public void SetRunningIcon(bool running)
        {
            var uri = running ? IconRunning : IconDefault;
            var dispatcher = Application.Current?.Dispatcher;
            if (dispatcher == null || dispatcher.CheckAccess())
            {
                notifyIcon.IconSource = new System.Windows.Media.Imaging.BitmapImage(uri);
            }
            else
            {
                dispatcher.BeginInvoke((Action)(() =>
                {
                    notifyIcon.IconSource = new System.Windows.Media.Imaging.BitmapImage(uri);
                }));
            }
        }

        private void NotifyIcon_TrayMouseDoubleClick(object sender, RoutedEventArgs e)
        {
            if (Window == null) return;

            if (Window.WindowState == WindowState.Minimized)
            {
                Window.WindowState = WindowState.Normal;
            }
            Window.Activate();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                notifyIcon?.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}

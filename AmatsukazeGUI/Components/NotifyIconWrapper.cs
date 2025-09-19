using System.ComponentModel;
using System.Windows;
using Hardcodet.Wpf.TaskbarNotification;

namespace Amatsukaze.Components
{
    public class NotifyIconWrapper : Component
    {
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
            try {
                notifyIcon.IconSource = new System.Windows.Media.Imaging.BitmapImage(new Uri("pack://application:,,,/AmatsukazeGUI;component/ServerIcon.ico"));
            }
            finally
            {
            }
            notifyIcon.ToolTipText = "AmatsukazeServer";
            notifyIcon.TrayMouseDoubleClick += NotifyIcon_TrayMouseDoubleClick;
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

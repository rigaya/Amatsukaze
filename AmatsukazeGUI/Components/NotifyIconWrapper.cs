﻿using System.ComponentModel;
using System.Windows;
using Hardcodet.Wpf.TaskbarNotification;

namespace Amatsukaze.Components
{
    public class NotifyIconWrapper : Component
    {
        private TaskbarIcon notifyIcon;
        public Window Window;

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
            notifyIcon.IconSource = Application.Current.MainWindow?.Icon;
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

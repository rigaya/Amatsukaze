using System;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Windows;
using Amatsukaze.Lib;
using Amatsukaze.Server;

namespace Amatsukaze.Win
{
    public class Program
    {
        [STAThread]
        public static void Main(string[] args)
        {
            try
            {
                // MainWindowで初期化を行うため、ここでの初期化は削除
                
                // サーバーアプリケーションを起動
                var app = new Application();
                app.Run(new MainWindow(args));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(ex.ToString());
                MessageBox.Show(ex.ToString(), "Amatsukaze Server Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }
} 
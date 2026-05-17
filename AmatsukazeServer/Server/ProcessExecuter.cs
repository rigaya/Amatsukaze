using Amatsukaze.Lib;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Runtime.InteropServices;

namespace Amatsukaze.Server
{
    public interface IProcessExecuter : IDisposable
    {
        void Canel();
        void Suspend();
        void Resume();
    }

    public class NormalProcess : IProcessExecuter
    {
        public Process Process { get; private set; }

        public Func<byte[], int, int, Task> OnOutput;

        private IntPtr[] SuspendedThreads;

        static NormalProcess()
        {
            // コードページエンコーディングを登録
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        }

        public NormalProcess(ProcessStartInfo psi)
        {
            Process = System.Diagnostics.Process.Start(psi);
        }

        #region IDisposable Support
        private bool disposedValue = false; // 重複する呼び出しを検出するには

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                if (disposing)
                {
                    // TODO: マネージ状態を破棄します (マネージ オブジェクト)。
                }

                // アンマネージ リソース (アンマネージ オブジェクト) を解放し、下のファイナライザーをオーバーライドします。
                if(SuspendedThreads != null)
                {
                    foreach (var pOpenThread in SuspendedThreads)
                    {
                        SystemUtility.CloseHandleNative(pOpenThread);
                    };
                }
                Process.Dispose();
                // 大きなフィールドを null に設定します。
                SuspendedThreads = null;
                Process = null;

                disposedValue = true;
            }
        }

        // TODO: 上の Dispose(bool disposing) にアンマネージ リソースを解放するコードが含まれる場合にのみ、ファイナライザーをオーバーライドします。
        // ~NormalProcess() {
        //   // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
        //   Dispose(false);
        // }

        // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
        public void Dispose()
        {
            // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
            Dispose(true);
            // TODO: 上のファイナライザーがオーバーライドされる場合は、次の行のコメントを解除してください。
            // GC.SuppressFinalize(this);
        }
        #endregion

        private bool IsProcessRunning()
        {
            try
            {
                return Process != null && !Process.HasExited && Process.ProcessName != string.Empty;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
        }

        public void Suspend()
        {
            if (!IsProcessRunning())
                return;

            if (SuspendedThreads != null)
                return;

            var suspendedThreads = new List<IntPtr>();
            try
            {
                foreach (var thread in Process.Threads.OfType<ProcessThread>())
                {
                    var pOpenThread = SystemUtility.OpenThreadNative(ThreadAccess.SUSPEND_RESUME, false, (uint)thread.Id);
                    if (pOpenThread == IntPtr.Zero)
                    {
                        continue;
                    }

                    if (SystemUtility.SuspendThreadNative(pOpenThread) == 0xFFFFFFFFU)
                    {
                        SystemUtility.CloseHandleNative(pOpenThread);
                        continue;
                    }

                    suspendedThreads.Add(pOpenThread);
                }
                SuspendedThreads = suspendedThreads.ToArray();
            }
            catch
            {
                foreach (var pOpenThread in suspendedThreads)
                {
                    try
                    {
                        SystemUtility.ResumeThreadNative(pOpenThread);
                    }
                    finally
                    {
                        SystemUtility.CloseHandleNative(pOpenThread);
                    }
                }
                throw;
            }
        }

        public void Resume()
        {
            if (SuspendedThreads == null)
                return;

            var resumeFailedThreads = new List<IntPtr>();
            foreach(var pOpenThread in SuspendedThreads)
            {
                try
                {
                    if (SystemUtility.ResumeThreadNative(pOpenThread) == -1)
                    {
                        resumeFailedThreads.Add(pOpenThread);
                        continue;
                    }
                    SystemUtility.CloseHandleNative(pOpenThread);
                }
                catch
                {
                    resumeFailedThreads.Add(pOpenThread);
                }
            };
            SuspendedThreads = resumeFailedThreads.Count > 0 ? resumeFailedThreads.ToArray() : null;
            if (resumeFailedThreads.Count > 0)
            {
                throw new InvalidOperationException("一部のスレッドの再開に失敗しました");
            }
        }

        private async Task RedirectOut(Stream stream)
        {
            try
            {
                // バイト配列を直接読み取り
                byte[] buffer = new byte[1024];
                while (true)
                {
                    var readBytes = await stream.ReadAsync(buffer, 0, buffer.Length);
                    if (readBytes == 0)
                    {
                        // 終了
                        return;
                    }
                    // OnOutput は別スレッドから差し替え/破棄され得るので、ローカルに退避してレースを避ける
                    var onOutput = OnOutput;
                    if (onOutput != null)
                    {
                        // そのままバイト配列を渡す
                        await onOutput(buffer, 0, readBytes);
                    }
                }
            }
            catch (Exception e)
            {
                // ToString() でスタックトレースまで出す
                Debug.Print("RedirectOut exception " + e.ToString());
            }
        }

        public Task WaitForExitAsync()
        {
            return Task.WhenAll(
                RedirectOut(Process.StandardOutput.BaseStream),
                RedirectOut(Process.StandardError.BaseStream),
                Task.Run(() => Process.WaitForExit()));
        }

        public void Canel()
        {
            if(Process != null && Process.HasExited == false)
            {
                string processName;
                string arguments;

                if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                {
                    processName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "taskkill.exe");
                    arguments = string.Format("/PID {0} /T /F", Process.Id);
                }
                else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                {
                    processName = "kill";
                    arguments = $"-9 {Process.Id}";
                }
                else
                {
                    return;
                }

                using (var procKiller = new System.Diagnostics.Process())
                {
                    procKiller.StartInfo.FileName = processName;
                    procKiller.StartInfo.Arguments = arguments;
                    procKiller.StartInfo.CreateNoWindow = true;
                    procKiller.StartInfo.UseShellExecute = false;
                    procKiller.Start();
                    procKiller.WaitForExit();
                }
            }
        }
    }
}

using System;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    // UpdateManager が所有する semaphore を例外安全な単一 writer lease にする。
    internal sealed class UpdateWriterLease : IDisposable
    {
        private readonly SemaphoreSlim semaphore;
        private bool disposed;

        private UpdateWriterLease(SemaphoreSlim semaphore)
        {
            this.semaphore = semaphore;
        }

        public static async Task<UpdateWriterLease> AcquireAsync(SemaphoreSlim semaphore,
            CancellationToken cancellationToken)
        {
            if (!await semaphore.WaitAsync(0, cancellationToken).ConfigureAwait(false))
            {
                throw new UpdateInstallException("UPDATE_BUSY", "S01_PRECHECK",
                    "別の更新適用が実行中です");
            }
            return new UpdateWriterLease(semaphore);
        }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            try
            {
                semaphore.Release();
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S13_ROLLBACK",
                    "UPDATE_WRITER_RELEASE_FAILED", ex);
            }
        }
    }

    // ユーザーの停止状態とは独立して、更新適用中だけキューを停止する。
    internal sealed class UpdateMaintenanceLease : IDisposable
    {
        private readonly Func<bool, Task> setPause;
        private bool disposed;
        private bool keepPaused;

        private UpdateMaintenanceLease(Func<bool, Task> setPause)
        {
            this.setPause = setPause;
        }

        public static Task<UpdateMaintenanceLease> AcquireAsync(EncodeServer server,
            CancellationToken cancellationToken) => AcquireAsync(
                () => server.NowEncoding, server.SetMaintenancePause, cancellationToken);

        internal static async Task<UpdateMaintenanceLease> AcquireAsync(Func<bool> nowEncoding,
            Func<bool, Task> setPause, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                await setPause(true).ConfigureAwait(false);
            }
            catch
            {
                // 停止フラグ設定後の状態通知だけが失敗した場合にも停止を残さない。
                await ResumeWithoutThrowAsync(setPause).ConfigureAwait(false);
                throw;
            }
            if (nowEncoding())
            {
                await ResumeWithoutThrowAsync(setPause).ConfigureAwait(false);
                throw new UpdateInstallException("ENCODING_ACTIVE", "S01_PRECHECK",
                    "エンコード実行中のため更新を適用できません");
            }
            return new UpdateMaintenanceLease(setPause);
        }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            if (keepPaused) return;
            // 停止フラグの解除は同期的に行われる。通知失敗は本体の結果を上書きしない。
            try
            {
                setPause(false).GetAwaiter().GetResult();
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S13_ROLLBACK",
                    "MAINTENANCE_RESUME_NOTIFY_FAILED", ex);
            }
        }

        // 本体 updater へ処理を引き渡した後は、サーバー終了までキュー停止を維持する。
        public void KeepPaused()
        {
            if (disposed) throw new ObjectDisposedException(nameof(UpdateMaintenanceLease));
            keepPaused = true;
        }

        private static async Task ResumeWithoutThrowAsync(Func<bool, Task> setPause)
        {
            try
            {
                await setPause(false).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                UpdateLog.WriteFallbackError("S13_ROLLBACK",
                    "MAINTENANCE_RESUME_NOTIFY_FAILED", ex);
            }
        }
    }
}

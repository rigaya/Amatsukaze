using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Rest
{
    public sealed class FilterPreviewEngine : IDisposable
    {
        public sealed record Reply(IReadOnlyDictionary<string, string> Metadata, byte[] Data);
        private readonly Process process;
        private readonly ConcurrentDictionary<(long id, long gen), TaskCompletionSource<Reply>> pending = new();
        private readonly ConcurrentQueue<string> logs = new();
        private readonly object requestSync = new();
        private Task writeTail = Task.CompletedTask;
        private Exception outputError;
        private readonly TaskCompletionSource<bool> ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private long nextId;
        private long currentGeneration;
        private bool disposed;

        public FilterPreviewEngine(string executable, IEnumerable<string> arguments)
        {
            var start = new ProcessStartInfo(executable) { UseShellExecute = false, RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
            foreach (var arg in arguments) start.ArgumentList.Add(arg);
            process = Process.Start(start) ?? throw new IOException("プレビューエンジンを起動できません");
            _ = ReadOutputAsync();
            _ = ReadErrorsAsync();
        }

        public string[] GetLogs() => logs.ToArray();

        public Task WaitReadyAsync(CancellationToken cancellation) => ready.Task.WaitAsync(TimeSpan.FromSeconds(15), cancellation);

        private async Task ReadErrorsAsync()
        {
            try
            {
                while (await process.StandardError.ReadLineAsync().ConfigureAwait(false) is string line)
                {
                    if (line.Length > 4096) line = line[..4096];
                    logs.Enqueue(line);
                    while (logs.Count > 100) logs.TryDequeue(out _);
                    Util.AddLog("[FilterPreview] " + line, null);
                }
            }
            catch (ObjectDisposedException) { }
            catch (IOException) { }
        }

        private static async Task<string> ReadLineAsync(Stream stream)
        {
            using var buffer = new MemoryStream();
            var one = new byte[1];
            while (await stream.ReadAsync(one.AsMemory(0, 1)).ConfigureAwait(false) != 0)
            {
                if (one[0] == 10) return Encoding.UTF8.GetString(buffer.ToArray()).TrimEnd('\r');
                if (buffer.Length >= 16384) throw new IOException("エンジン応答のヘッダが長すぎます");
                buffer.WriteByte(one[0]);
            }
            throw new EndOfStreamException("プレビューエンジンが終了しました");
        }

        private async Task ReadOutputAsync()
        {
            try
            {
                var stream = process.StandardOutput.BaseStream;
                var first = await ReadLineAsync(stream).ConfigureAwait(false);
                if (first != "AMTPREVIEW READY version=1") throw new IOException("プレビューエンジンのバージョンが不正です: " + first);
                ready.TrySetResult(true);
                while (true)
                {
                    var line = await ReadLineAsync(stream).ConfigureAwait(false);
                    var tokens = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (tokens.Length < 2 || tokens[0] != "AMTPREVIEW") throw new IOException("エンジン応答が不正です");
                    var values = new Dictionary<string, string>(StringComparer.Ordinal);
                    // ERRORの本文にはid=等を含む外部ログが入るため、ヘッダとして再解釈しない。
                    var messageStart = tokens[1] == "ERROR" ? line.IndexOf(" message=", StringComparison.Ordinal) : -1;
                    var headerTokens = messageStart >= 0 ? line[..messageStart].Split(' ', StringSplitOptions.RemoveEmptyEntries) : tokens;
                    foreach (var token in headerTokens.Skip(2))
                    {
                        int split = token.IndexOf('=');
                        if (split > 0) values[token[..split]] = token[(split + 1)..];
                    }
                    if (messageStart >= 0) values["message"] = line[(messageStart + " message=".Length)..];
                    if (!values.TryGetValue("gen", out var generation) || !long.TryParse(generation, out var gen)) throw new IOException("エンジン応答に世代がありません");
                    long id = values.TryGetValue("id", out var value) && long.TryParse(value, out var parsed) ? parsed : 0;
                    var bytes = Array.Empty<byte>();
                    if (tokens[1] == "FRAME")
                    {
                        if (!values.TryGetValue("bytes", out var count) || !int.TryParse(count, out var length) || length < 0 || length > 256 * 1024 * 1024) throw new IOException("画像サイズが不正です");
                        bytes = new byte[length];
                        await stream.ReadExactlyAsync(bytes).ConfigureAwait(false);
                    }
                    lock (requestSync)
                    {
                        if (pending.TryRemove((id, gen), out var waiter))
                        {
                            if (tokens[1] == "ERROR") waiter.TrySetException(new IOException(values.GetValueOrDefault("message", "エンジンエラー")));
                            else waiter.TrySetResult(new Reply(values, bytes));
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                ready.TrySetException(ex);
                lock (requestSync)
                {
                    outputError = ex;
                    foreach (var entry in pending)
                        if (pending.TryRemove(entry.Key, out var waiter)) waiter.TrySetException(ex);
                }
            }
        }

        public Task<Reply> RequestAsync(Dictionary<string, object> command, long generation, bool frame, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            lock (requestSync)
            {
                cancellation.ThrowIfCancellationRequested();
                if (outputError != null) return Task.FromException<Reply>(outputError);
                long id = frame ? ++nextId : 0;
                if (frame && generation != currentGeneration) throw new ArgumentException("設定の世代が一致しません");
                if (!frame && generation <= currentGeneration) throw new ArgumentException("設定の世代が古いです");
                var message = new Dictionary<string, object>(command) { ["gen"] = generation };
                if (frame) message["id"] = id;
                var serialized = JsonSerializer.Serialize(message);
                var waiter = new TaskCompletionSource<Reply>(TaskCreationOptions.RunContinuationsAsynchronously);
                if (!pending.TryAdd((id, generation), waiter)) throw new InvalidOperationException("同じ世代の設定処理が実行中です");
                if (!frame)
                {
                    currentGeneration = generation;
                    foreach (var entry in pending)
                        if (entry.Key.gen < generation && pending.TryRemove(entry.Key, out var stale)) stale.TrySetCanceled();
                }
                // 登録順の送信を予約し、旧世代の応答待ちは書き込み完了を待たずに解除する。
                writeTail = WriteQueuedAsync(writeTail, serialized, waiter);
                return AwaitReplyAsync(id, generation, frame, waiter, cancellation);
            }
        }

        private async Task WriteQueuedAsync(Task previous, string message, TaskCompletionSource<Reply> waiter)
        {
            try
            {
                await previous.ConfigureAwait(false);
                await ready.Task.ConfigureAwait(false);
                await process.StandardInput.WriteLineAsync(message).ConfigureAwait(false);
                await process.StandardInput.FlushAsync().ConfigureAwait(false);
            }
            catch (Exception ex) { waiter?.TrySetException(ex); }
        }

        private async Task<Reply> AwaitReplyAsync(long id, long generation, bool frame, TaskCompletionSource<Reply> waiter, CancellationToken cancellation)
        {
            try { return await waiter.Task.WaitAsync(cancellation).ConfigureAwait(false); }
            finally
            {
                lock (requestSync)
                {
                    pending.TryRemove((id, generation), out _);
                    if (frame && cancellation.IsCancellationRequested)
                        writeTail = WriteQueuedAsync(writeTail, JsonSerializer.Serialize(new { cmd = "cancel", id }), null);
                }
            }
        }

        public bool Stop()
        {
            if (disposed) return true;
            disposed = true;
            try
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
                return process.WaitForExit(5000);
            }
            catch (InvalidOperationException) { return true; }
            catch (System.ComponentModel.Win32Exception) { return false; }
            finally { process.Dispose(); }
        }

        public void Dispose() => Stop();
    }
}

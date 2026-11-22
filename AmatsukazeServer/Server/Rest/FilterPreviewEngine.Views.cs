using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Rest
{
    public sealed partial class FilterPreviewEngine
    {
        public sealed record ViewRecord(string Header, byte[] Metadata, byte[] Payload)
        {
            internal async Task WriteAsync(Stream stream, CancellationToken cancellation)
            {
                await stream.WriteAsync(Encoding.UTF8.GetBytes(Header + "\n"), cancellation).ConfigureAwait(false);
                await stream.WriteAsync(Metadata, cancellation).ConfigureAwait(false);
                await stream.WriteAsync(Payload, cancellation).ConfigureAwait(false);
                await stream.FlushAsync(cancellation).ConfigureAwait(false);
            }
        }

        private sealed class ViewWaiter
        {
            public readonly Channel<ViewRecord> Records = Channel.CreateBounded<ViewRecord>(new BoundedChannelOptions(4)
            {
                SingleReader = true,
                FullMode = BoundedChannelFullMode.Wait,
            });
            public readonly CancellationTokenSource Cancellation = new();
        }

        private readonly ConcurrentDictionary<(long id, long gen), ViewWaiter> pendingViews = new();
        private const int MaxViewMetadata = 64 * 1024;
        private const int MaxViewPayload = 256 * 1024 * 1024;

        public async Task StreamViewAsync(Dictionary<string, object> command, long generation,
            Func<ViewRecord, CancellationToken, Task> deliver, CancellationToken cancellation)
        {
            long id;
            var waiter = new ViewWaiter();
            using (waiter.Cancellation)
            using (var delivery = CancellationTokenSource.CreateLinkedTokenSource(cancellation, waiter.Cancellation.Token))
            {
                lock (requestSync)
                {
                    cancellation.ThrowIfCancellationRequested();
                    if (outputError != null) throw outputError;
                    if (generation != currentGeneration) throw new ArgumentException("設定の世代が一致しません");
                    id = ++nextId;
                    var message = new Dictionary<string, object>(command) { ["cmd"] = "view", ["gen"] = generation, ["id"] = id };
                    var serialized = JsonSerializer.Serialize(message);
                    pendingViews[(id, generation)] = waiter;
                    writeTail = WriteQueuedAsync(writeTail, serialized, ex => waiter.Records.Writer.TryComplete(ex));
                }

                try
                {
                    await foreach (var record in waiter.Records.Reader.ReadAllAsync(delivery.Token).ConfigureAwait(false))
                    {
                        delivery.Token.ThrowIfCancellationRequested();
                        await deliver(record, delivery.Token).ConfigureAwait(false);
                    }
                }
                finally
                {
                    lock (requestSync)
                    {
                        if (pendingViews.TryRemove((id, generation), out _) && generation == currentGeneration)
                            EnqueueCancel(id);
                    }
                }
            }
        }

        // 呼び出し元はrequestSyncの中で送信順を予約する。
        private void EnqueueCancel(long id) =>
            writeTail = WriteQueuedAsync(writeTail, JsonSerializer.Serialize(new { cmd = "cancel", id }), null);

        private async Task ReadViewAsync(Stream stream, string type, string line, Dictionary<string, string> values, long id, long gen)
        {
            if (id <= 0) throw new IOException("表示応答に識別子がありません");
            var meta = Array.Empty<byte>();
            var payload = Array.Empty<byte>();
            var key = (id, gen);
            if (type == "VIEW")
            {
                int metaSize = ReadLength(values, "metaBytes", MaxViewMetadata);
                int imageSize = ReadLength(values, "bytes", MaxViewPayload);
                if (!values.TryGetValue("revision", out var revision) || !long.TryParse(revision, NumberStyles.None, CultureInfo.InvariantCulture, out _))
                    throw new IOException("表示応答に更新番号がありません");
                if (!pendingViews.ContainsKey(key))
                {
                    await DiscardAsync(stream, checked(metaSize + imageSize)).ConfigureAwait(false);
                    return;
                }
                meta = new byte[metaSize];
                await stream.ReadExactlyAsync(meta).ConfigureAwait(false);
                ValidateFrameLengths(meta, imageSize);
                payload = new byte[imageSize];
                await stream.ReadExactlyAsync(payload).ConfigureAwait(false);
            }
            else if (type == "DONE" && !values.ContainsKey("status"))
                throw new IOException("表示応答に終了状態がありません");

            lock (requestSync)
            {
                if (!pendingViews.TryGetValue(key, out var waiter)) return;
                if (!waiter.Records.Writer.TryWrite(new ViewRecord(line, meta, payload)))
                {
                    // 遅いブラウザのHTTP送信が、同じエンジンの他の応答を止めないようにする。
                    pendingViews.TryRemove(key, out _);
                    waiter.Records.Writer.TryComplete(new IOException("表示応答の受信が追いつきません"));
                    if (gen == currentGeneration) EnqueueCancel(id);
                }
                else if (type != "VIEW")
                {
                    pendingViews.TryRemove(key, out _);
                    waiter.Records.Writer.TryComplete();
                }
            }
        }

        private static int ReadLength(Dictionary<string, string> values, string field, int maximum)
        {
            if (!values.TryGetValue(field, out var text) ||
                !int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var size) || size > maximum)
                throw new IOException("表示応答の長さが不正です: " + field);
            return size;
        }

        private static void ValidateFrameLengths(byte[] metadata, int bytes)
        {
            using var document = JsonDocument.Parse(metadata);
            if (document.RootElement.ValueKind != JsonValueKind.Object ||
                !document.RootElement.TryGetProperty("frames", out var frames) ||
                frames.ValueKind != JsonValueKind.Array || frames.GetArrayLength() > 2)
                throw new IOException("表示応答の画像一覧が不正です");
            int sides = 0;
            int remaining = bytes;
            foreach (var frame in frames.EnumerateArray())
            {
                if (frame.ValueKind != JsonValueKind.Object ||
                    !frame.TryGetProperty("side", out var sideValue) || sideValue.ValueKind != JsonValueKind.String ||
                    !frame.TryGetProperty("bytes", out var lengthValue) || lengthValue.ValueKind != JsonValueKind.Number ||
                    !lengthValue.TryGetInt32(out int length))
                    throw new IOException("表示応答の画像境界が不正です");
                int side = sideValue.GetString() switch { "before" => 1, "after" => 2, _ => 0 };
                bool reuse = frame.TryGetProperty("reuse", out var reuseValue);
                if (side == 0 || (sides & side) != 0 || length > remaining ||
                    (reuse ? reuseValue.ValueKind != JsonValueKind.True || length != 0 : length <= 0))
                    throw new IOException("表示応答の画像境界が不正です");
                sides |= side;
                remaining -= length;
            }
            if (remaining != 0) throw new IOException("表示応答の画像長が一致しません");
        }

        private static async Task DiscardAsync(Stream stream, int length)
        {
            var buffer = new byte[Math.Min(length, 8192)];
            while (length > 0)
            {
                int count = Math.Min(length, buffer.Length);
                await stream.ReadExactlyAsync(buffer.AsMemory(0, count)).ConfigureAwait(false);
                length -= count;
            }
        }
    }
}

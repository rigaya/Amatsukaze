using System.Globalization;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.AspNetCore.Components;
using Microsoft.AspNetCore.Components.WebAssembly.Http;
using Microsoft.JSInterop;

namespace AmatsukazeWebUI.Pages;

public partial class FilterAdjust
{
    private const int MaxViewHeaderBytes = 16384, MaxViewMetadataBytes = 65536;
    private const int MaxViewPayloadBytes = 256 * 1024 * 1024;
    private static readonly JsonSerializerOptions ViewRequestJson = new(JsonSerializerDefaults.Web)
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };
    private double _encoderSeconds;
    private double? _encoderDurationSeconds;
    private bool _hasEncoderView;
    private string? _encoderBoundary;
    private string EncoderDisplayMode => _view == "compare" ? "sideBySide" : _view;
    private string EncoderSeekValue => FormatSeconds(_encoderSeconds);
    private string EncoderSeekMax => FormatSeconds(_encoderDurationSeconds ?? 0);
    private string EncoderTimeText => FormatTime(_encoderSeconds);
    private static string FormatSeconds(double seconds) => seconds.ToString("0.######", CultureInfo.InvariantCulture);
    private bool CanStep(int direction) => direction is -1 or 1 && Ready && !_fetchingFrame && (IsEncoder
        ? _encoderBoundary != (direction < 0 ? "start" : "end")
        : direction < 0 ? _position > 0 : _position < MaxFrame);
    private static string FormatTime(double seconds)
    {
        var time = TimeSpan.FromSeconds(seconds < TimeSpan.MaxValue.TotalSeconds ? seconds : 0);
        return $"{(long)time.TotalHours}:{time.Minutes:D2}:{time.Seconds:D2}.{time.Milliseconds:D3}";
    }

    private Task SeekEncoderAsync(ChangeEventArgs args, string phase)
        => double.TryParse(args.Value?.ToString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var seconds)
            ? SeekEncoderToAsync(seconds, phase) : Task.CompletedTask;

    private Task SeekEncoderTimeAsync(string value)
    {
        var text = value.AsSpan().Trim();
        int separator = text.IndexOf(':');
        if (separator < 0)
            return double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var seconds)
                ? SeekEncoderToAsync(seconds, "commit") : Task.CompletedTask;

        if (!int.TryParse(text[..separator], NumberStyles.None, CultureInfo.InvariantCulture, out var hours)
            || !TimeSpan.TryParseExact(text[(separator + 1)..], @"mm\:ss\.fff", CultureInfo.InvariantCulture, out var rest))
            return Task.CompletedTask;
        return SeekEncoderToAsync(hours * 3600d + rest.TotalSeconds, "commit");
    }

    private Task SeekEncoderToAsync(double seconds, string phase)
    {
        if (!CanOperate || !double.IsFinite(seconds) || seconds < 0 || seconds >= TimeSpan.MaxValue.TotalSeconds)
            return Task.CompletedTask;
        _encoderSeconds = Math.Min(seconds, _encoderDurationSeconds ?? seconds);
        return StartEncoderViewAsync(new("seek", _encoderSeconds, phase));
    }

    private async Task RestoreEncoderPresentationAsync()
    {
        var operation = StartEncoderViewAsync(new("seek", _encoderSeconds, "commit"));
        long revision = _frameRevision;
        await operation;
        if (Ready && _frameRevision == revision && _error == null && _view != _displayedView)
            await StartEncoderViewAsync(new("display", Mode: EncoderDisplayMode));
    }

    private Task RefreshEncoderAsync() => _hasEncoderView
        ? StartEncoderViewAsync(new("snapshot"))
        : Task.CompletedTask;

    private sealed record EncoderOperation(string Op, double? Seconds = null, string? Phase = null,
        string? Direction = null, string? Mode = null);

    private Task StartEncoderViewAsync(EncoderOperation action)
    {
        if (!Ready || !IsEncoder) return Task.CompletedTask;
        _frameCancellation?.Cancel();
        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token, _sessionCancellation.Token);
        _frameCancellation = cancellation;
        return TrackAsync(RenderEncoderViewAsync(_generation, ++_frameRevision, action, cancellation));
    }

    private async Task RenderEncoderViewAsync(long gen, long revision, EncoderOperation action, CancellationTokenSource cancellation)
    {
        var token = cancellation.Token;
        bool Current() => !_disposed && gen == _generation && revision == _frameRevision && !token.IsCancellationRequested;
        try
        {
            if (action.Op != "snapshot") _encoderBoundary = null;
            _status = "フレームを取得中…";
            _error = null;
            StateHasChanged();
            await InvalidateImagesAsync();
            if (!Current()) return;
            var options = CaptureFrameOptions();
            string done = await ReceiveAsync(action, options);
            if (!Current()) return;
            if (options.Format == "yuv" && _pngFallback && _hasEncoderView)
            {
                // 背景の比較画像で改訂番号が進む場合も、操作を再適用せず現在の表示をPNGで受け取る。
                await ReceiveAsync(new("snapshot"), options with { Format = "png" });
                if (!Current()) return;
            }
            if (action.Op != "snapshot" && done is "start" or "end") _encoderBoundary = done;
            _status = done switch
            {
                "start" => "先頭です",
                "end" => "末尾です",
                "partial" => "取得できた画像を表示しました",
                "targetNotReached" => "目標時刻に到達できませんでした",
                "ignored" => "直前の操作を処理中です",
                "superseded" => "操作が置き換わりました",
                _ when _pngFallback && _quality == "yuv" => "表示しました(PNG代替)",
                _ => "表示しました"
            };

            async Task<string> ReceiveAsync(EncoderOperation operation, FrameOptions frameOptions)
            {
                using var request = new HttpRequestMessage(HttpMethod.Post, SessionUrl + "/view");
                request.Content = JsonContent.Create(new
                {
                    gen, operation.Op, operation.Seconds, operation.Phase, operation.Direction,
                    operation.Mode, frameOptions.Format, frameOptions.Crop
                }, options: ViewRequestJson);
                request.SetBrowserResponseStreamingEnabled(true);
                using var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token);
                await CheckAsync(response, token);
                using var stream = new BufferedStream(await response.Content.ReadAsStreamAsync(token), MaxViewHeaderBytes);
                // 応答内で転送済みの画像を記録し、実際に描画できたかも区別する。
                var receivedSides = new Dictionary<string, ReceivedEncoderFrame>(StringComparer.Ordinal);
                long? requestId = null;
                long lastRevision = -1;
                while (Current())
                {
                    var header = await ReadViewHeaderAsync(stream, token);
                    var fields = header.Split(' ', 3);
                    if (fields.Length != 3 || fields[0] != "AMTPREVIEW")
                        throw new InvalidDataException("表示応答のヘッダーが不正です");
                    var values = ParseViewHeaderFields(fields[2], out var message);
                    long returnedId = RequiredViewNumber(values, "id");
                    if (RequiredViewNumber(values, "gen") != gen || requestId.HasValue && requestId != returnedId)
                        throw new InvalidDataException("表示応答の要求IDまたは世代が一致しません");
                    requestId = returnedId;
                    if (fields[1] == "ERROR") throw new InvalidOperationException(message ?? "フレームを取得できませんでした");
                    if (fields[1] == "DONE")
                    {
                        var status = values.GetValueOrDefault("status");
                        return status is "ok" or "start" or "end" or "partial" or "targetNotReached" or "ignored" or "superseded"
                            ? status : throw new InvalidDataException("表示応答の完了状態が不正です");
                    }
                    if (fields[1] != "VIEW") throw new InvalidDataException("表示応答の種類が不正です");
                    long viewRevision = RequiredViewNumber(values, "revision");
                    if (viewRevision < lastRevision) throw new InvalidDataException("表示応答の改訂番号が逆順です");
                    lastRevision = viewRevision;
                    int metaSize = checked((int)RequiredViewNumber(values, "metaBytes", MaxViewMetadataBytes));
                    int payloadSize = checked((int)RequiredViewNumber(values, "bytes", MaxViewPayloadBytes));
                    var metaBytes = new byte[metaSize];
                    await stream.ReadExactlyAsync(metaBytes, token);
                    var packet = JsonSerializer.Deserialize<EncoderView>(metaBytes, ViewRequestJson)
                        ?? throw new InvalidDataException("表示応答のメタデータがありません");
                    await DrawEncoderViewAsync(packet, stream, payloadSize, gen, frameOptions, receivedSides,
                        operation.Op != "snapshot", Current, token);
                }
                token.ThrowIfCancellationRequested();
                return "superseded";
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { if (Current()) ShowPreviewError(ex); }
        finally { FinishFrameRequest(cancellation); }
    }

    private sealed record EncoderView(double CurrentSeconds, double? DurationSeconds, string DisplayMode, EncoderViewFrame[] Frames);
    private sealed record EncoderViewFrame(string Side, string Format, int Width, int Height, int Bytes, int[]? Pitches,
        int Sarnum, int Sarden, string? Matrix, string? Range, string? Primaries, string? Transfer, string? Chromaloc, bool Reuse);
    private sealed record ReceivedEncoderFrame(EncoderViewFrame Frame, bool Drawn);

    private async Task DrawEncoderViewAsync(EncoderView packet, Stream stream, int payloadSize, long gen,
        FrameOptions options, Dictionary<string, ReceivedEncoderFrame> receivedSides, bool allowReuse, Func<bool> current, CancellationToken token)
    {
        string view = packet.DisplayMode switch
        {
            "after" => "after", "before" => "before", "split" or "sideBySide" => "compare",
            _ => throw new InvalidDataException("表示方式が不正です")
        };
        if (!double.IsFinite(packet.CurrentSeconds) || packet.CurrentSeconds < 0
            || packet.CurrentSeconds >= TimeSpan.MaxValue.TotalSeconds
            || packet.DurationSeconds is double duration && (!double.IsFinite(duration) || duration < 0 || duration >= TimeSpan.MaxValue.TotalSeconds)
            || packet.Frames is null || packet.Frames.Length > 2
            || packet.Frames.Any(f => f is null || f.Side is not ("before" or "after") || (view != "compare" && f.Side != view)
                || (f.Reuse ? !allowReuse || f.Bytes != 0 || !receivedSides.ContainsKey(f.Side)
                    : f.Format is null || f.Width <= 0 || f.Height <= 0 || f.Bytes <= 0))
            || packet.Frames.Select(f => f.Side).Distinct().Count() != packet.Frames.Length
            || packet.Frames.Sum(f => (long)f.Bytes) != payloadSize)
            throw new InvalidDataException("表示画像のメタデータが不正です");

        var images = new List<(EncoderViewFrame Frame, byte[] Bytes)>(packet.Frames.Length);
        foreach (var frame in packet.Frames)
        {
            if (frame.Reuse) continue;
            var bytes = new byte[frame.Bytes];
            await stream.ReadExactlyAsync(bytes, token);
            images.Add((frame, bytes));
        }
        if (!current()) return;
        _hasEncoderView = true;
        _encoderSeconds = packet.CurrentSeconds;
        _encoderDurationSeconds = packet.DurationSeconds;
        foreach (var (frame, bytes) in images)
        {
            if (_pngFallback && options.Format == "yuv")
            {
                receivedSides[frame.Side] = new(frame, false);
                continue;
            }
            var metadata = new Dictionary<string, string?>
            {
                ["format"] = frame.Format, ["width"] = frame.Width.ToString(CultureInfo.InvariantCulture),
                ["height"] = frame.Height.ToString(CultureInfo.InvariantCulture),
                ["pitches"] = frame.Pitches == null ? "" : string.Join(",", frame.Pitches),
                ["matrix"] = frame.Matrix, ["range"] = frame.Range,
                ["primaries"] = frame.Primaries, ["transfer"] = frame.Transfer, ["chromaloc"] = frame.Chromaloc
            };
            bool success = await _renderer!.InvokeAsync<bool>("draw", frame.Side == "before" ? _beforeCanvas : _afterCanvas,
                bytes, metadata, frame.Format switch { "jpeg" => "image/jpeg", "png" => "image/png", _ => "" });
            if (!current()) return;
            if (!success && options.Format == "yuv") _pngFallback = true;
            else if (!success) throw new InvalidOperationException("画像を描画できません");
            else RecordImageGeneration(frame.Side, gen);
            receivedSides[frame.Side] = new(frame, success);
        }
        if (!current()) return;

        // 表示から消えた側は再利用元からも外し、描画できなかった側に古い画像を残さない。
        if (!packet.Frames.Any(f => f.Side == "after")) receivedSides.Remove("after");
        if (!packet.Frames.Any(f => f.Side == "before")) receivedSides.Remove("before");
        var after = receivedSides.GetValueOrDefault("after");
        var before = receivedSides.GetValueOrDefault("before");
        foreach (var side in new[] { "after", "before" })
        {
            if (receivedSides.GetValueOrDefault(side)?.Drawn == true) continue;
            await _renderer!.InvokeVoidAsync("clear", side == "before" ? _beforeCanvas : _afterCanvas);
            if (!current()) return;
            RecordImageGeneration(side, null);
        }
        if ((after ?? before)?.Frame is { } display)
        {
            double height = options.Crop?[3] ?? display.Height;
            double sar = display.Sarnum > 0 && display.Sarden > 0 ? (double)display.Sarnum / display.Sarden : 1;
            double dar = _inputDar > 0 ? _inputDar : display.Width * sar / display.Height;
            await _viewer!.InvokeVoidAsync("present", view, height * dar, height);
        }
        if (!current()) return;
        PresentPreview(view);
        if (packet.Frames.Length > 0 && (!_pngFallback || options.Format != "yuv"))
            _status = "表示中…";
        StateHasChanged();
    }

    private static async Task<string> ReadViewHeaderAsync(Stream stream, CancellationToken token)
    {
        byte[] buffer = new byte[MaxViewHeaderBytes];
        for (int i = 0; i < buffer.Length; i++)
        {
            if (await stream.ReadAsync(buffer.AsMemory(i, 1), token) == 0)
                throw new EndOfStreamException("表示応答が途中で終了しました");
            if (buffer[i] == (byte)'\n') return Encoding.UTF8.GetString(buffer, 0, i).TrimEnd('\r');
        }
        throw new InvalidDataException("表示応答のヘッダーが長すぎます");
    }

    private static Dictionary<string, string> ParseViewHeaderFields(string fields, out string? message)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        int error = fields.IndexOf(" message=", StringComparison.Ordinal);
        message = error < 0 ? null : fields[(error + " message=".Length)..];
        foreach (var field in (error < 0 ? fields : fields[..error]).Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            int equal = field.IndexOf('=');
            if (equal <= 0 || !values.TryAdd(field[..equal], field[(equal + 1)..]))
                throw new InvalidDataException("表示応答のヘッダー項目が不正です");
        }
        return values;
    }

    private static long RequiredViewNumber(Dictionary<string, string> fields, string key, long max = long.MaxValue)
        => long.TryParse(fields.GetValueOrDefault(key), NumberStyles.None, CultureInfo.InvariantCulture, out var value) && value <= max
            ? value : throw new InvalidDataException($"表示応答の{key}が不正です");
}

using System.Globalization;
using Amatsukaze.Shared;
using Microsoft.AspNetCore.Components;
using Microsoft.JSInterop;

namespace AmatsukazeWebUI.Pages;

public partial class TrimAdjust
{
    private ElementReference _audioRef;
    private CancellationTokenSource? _audioRequest;
    private int _audioGeneration;
    private int _audioStart = -1;
    private int _audioEnd = -1;
    private int _audioSeconds = 5;
    private bool _audioAutoPlay;
    private bool _audioActive;
    private string _audioStatus = "";

    private async Task LoadAudioSettingsAsync()
    {
        try
        {
            var seconds = await JS.InvokeAsync<string?>("localStorage.getItem", "amatsukaze.trimAdjust.audioSeconds");
            if (int.TryParse(seconds, out var value)) _audioSeconds = Math.Clamp(value, 1, 120);
            _audioAutoPlay = await JS.InvokeAsync<string?>("localStorage.getItem", "amatsukaze.trimAdjust.audioAutoPlay") == "1";
        }
        catch (JSException) { }
    }

    private async Task SetAudioSecondsAsync(ChangeEventArgs e)
    {
        if (int.TryParse(e.Value?.ToString(), out var value)) _audioSeconds = Math.Clamp(value, 1, 120);
        await SaveAudioSettingAsync("audioSeconds", _audioSeconds.ToString(CultureInfo.InvariantCulture));
    }

    private async Task SetAudioAutoPlayAsync(ChangeEventArgs e)
    {
        _audioAutoPlay = e.Value is true;
        await SaveAudioSettingAsync("audioAutoPlay", _audioAutoPlay ? "1" : "0");
    }

    private async Task SaveAudioSettingAsync(string name, string value)
    {
        try { await JS.InvokeVoidAsync("localStorage.setItem", "amatsukaze.trimAdjust." + name, value); }
        catch (JSException) { _audioStatus = "設定を保存できませんでした(この画面では有効です)"; }
    }

    private Task PlayAudioHereAsync() => PlayAudioAsync(_currentFrame, _session?.NumFrames - 1 ?? -1, _audioSeconds);

    private Task PlaySelectedAudioAsync()
    {
        var seg = _cmSeg?.Segments.FirstOrDefault(s => s.Idx == _cmSegSelectedIdx);
        return seg == null ? Task.CompletedTask : PlaySegmentAudioAsync(seg);
    }

    private Task PlaySegmentAudioAsync(CmSegAnnotationSegment seg) => PlayAudioAsync(seg.Start, seg.End, 120);

    private async Task PlayAudioAsync(int start, int end, int seconds)
    {
        if (_disposed || _session?.AudioPlaybackEnabled != true || _sessionExpired || string.IsNullOrEmpty(_sessionId) || start < 0 || end < start) return;
        // 次フレームのPTSを区間末尾として、上限を超えない包含端を求める。
        var pts = _session.FramePts;
        if (start >= pts.Count) return;
        end = Math.Min(end, pts.Count - 1);
        double EndTime(int frame) => frame + 1 < pts.Count ? pts[frame + 1]
            : pts[frame] + (frame > 0 ? pts[frame] - pts[frame - 1] : 1.0 / 30.0);
        var originalEnd = end;
        var low = start;
        var high = end;
        while (low <= high)
        {
            var middle = low + (high - low) / 2;
            if (EndTime(middle) - pts[start] <= seconds + 1e-9) low = middle + 1;
            else high = middle - 1;
        }
        end = high;
        if (end < start) return;
        if (_audioActive && _audioStart == start && _audioEnd == end)
        {
            await StopAudioAsync();
            return;
        }
        var stopping = StopAudioAsync();
        var generation = _audioGeneration;
        await stopping;
        if (_disposed || generation != _audioGeneration) return;
        var sessionId = _sessionId;
        var request = new CancellationTokenSource();
        _audioRequest = request;
        _audioStart = start;
        _audioEnd = end;
        _audioActive = true;
        _audioStatus = "音声を読み込み中…";
        try
        {
            var result = await Api.GetTrimAudioAsync(sessionId, start, end, request.Token);
            if (_disposed || generation != _audioGeneration || sessionId != _sessionId) return;
            if (!result.Ok) throw new IOException(result.Error ?? "音声の取得に失敗しました");
            if (result.Data == null || result.Data.Length == 0)
            {
                _audioActive = false;
                _audioStatus = "この範囲には音声がありません";
                return;
            }
            await JS.InvokeVoidAsync("trimAdjustAudio.play", _audioRef, result.Data, generation);
            if (generation != _audioGeneration) return;
            _audioStatus = originalEnd > end && seconds == 120 ? "再生中(区間の先頭120秒まで)" : "再生中";
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            if (generation == _audioGeneration && !_disposed)
            {
                _audioActive = false;
                _audioStatus = $"音声を再生できませんでした: {ex.Message}";
            }
        }
        finally
        {
            if (ReferenceEquals(_audioRequest, request)) _audioRequest = null;
            request.Dispose();
            if (!_disposed) StateHasChanged();
        }
    }

    [JSInvokable]
    public async Task StopAudioAsync()
    {
        _audioGeneration++;
        _audioRequest?.Cancel();
        _audioActive = false;
        _audioStatus = "";
        await JS.InvokeVoidAsync("trimAdjustAudio.stop", _audioRef, _audioGeneration);
        if (!_disposed) StateHasChanged();
    }

    private void OnAudioEnded()
    {
        _audioActive = false;
        _audioStatus = "再生終了";
    }

    private async Task DisposeAudioAsync()
    {
        await StopAudioAsync();
        await JS.InvokeVoidAsync("trimAdjustAudio.dispose", _pageRef, _audioRef);
    }
}

using System.Globalization;
using System.Text.Json;
using Amatsukaze.Shared;
using Microsoft.AspNetCore.Components;
using Microsoft.AspNetCore.Components.Routing;
using Microsoft.JSInterop;

namespace AmatsukazeWebUI.Pages;

public partial class TrimAdjust
{
    private ElementReference _cmSegPanelRef;
    private CmSegAnnotation? _cmSeg;
    private bool _cmSegShortOnly;
    private int _cmSegSelectedIdx = -1;
    private bool _cmSegDirty;
    private bool _cmSegDisposing;
    private int _cmSegRevision;
    private bool _cmSegSaveError;
    private bool _cmSegSaveConflict;
    private string? _cmSegBaseline;
    private string _cmSegStatus = "読み込み中...";
    private CancellationTokenSource? _cmSegDebounce;
    private readonly SemaphoreSlim _cmSegSaveLock = new(1, 1);
    private async Task LoadCmSegAsync()
    {
        _cmSegDebounce?.Cancel();
        _cmSeg = null;
        _cmSegDirty = false;
        _cmSegRevision = 0;
        _cmSegSaveConflict = false;
        _cmSegBaseline = null;
        if (!string.IsNullOrEmpty(_session?.CmSegAnnotationError))
        {
            _cmSegSaveError = true;
            _cmSegStatus = _session.CmSegAnnotationError;
            return;
        }
        if (_session?.CmSegAnnotationEnabled != true || string.IsNullOrEmpty(_sessionId)) return;
        var result = await Api.GetCmSegAnnotationAsync(_sessionId);
        _cmSeg = result.Data;
        if (result.Ok && _cmSeg != null)
            _cmSegBaseline = CmSegLabelSignature(_cmSeg.Reviewed,
                _cmSeg.Segments.Select(seg => new CmSegLabelUpdate { Idx = seg.Idx, Label = seg.Label }));
        _cmSegSaveError = !result.Ok;
        _cmSegStatus = result.Ok ? "変更は自動保存されます" : result.Error ?? "分類の読み込みに失敗しました";
        _cmSegSelectedIdx = VisibleCmSegSegments().FirstOrDefault()?.Idx ?? -1;
    }

    private static string CmSegLabelSignature(bool reviewed, IEnumerable<CmSegLabelUpdate> segments) =>
        JsonSerializer.Serialize(new
        {
            Reviewed = reviewed,
            Segments = segments.OrderBy(seg => seg.Idx).Select(seg => new { seg.Idx, seg.Label }).ToArray()
        });

    private IEnumerable<CmSegAnnotationSegment> VisibleCmSegSegments() =>
        _cmSeg?.Segments.Where(seg => !_cmSegShortOnly || IsCmSegShort(seg))
        ?? Enumerable.Empty<CmSegAnnotationSegment>();

    private (long Num, long Den) CmSegFps()
    {
        var parts = _cmSeg?.Fps.Split('/') ?? Array.Empty<string>();
        return parts.Length == 2 && long.TryParse(parts[0], out var num) &&
            long.TryParse(parts[1], out var den) && num > 0 && den > 0 ? (num, den) : (0, 1);
    }

    private bool IsCmSegShort(CmSegAnnotationSegment seg)
    {
        var fps = CmSegFps();
        return fps.Num > 0 && (decimal)(seg.End - seg.Start + 1) * fps.Den * 2 < 29m * fps.Num;
    }

    private string CmSegDuration(CmSegAnnotationSegment seg)
    {
        var fps = CmSegFps();
        return fps.Num > 0 ? ((double)(seg.End - seg.Start + 1) * fps.Den / fps.Num)
            .ToString("0.00", CultureInfo.InvariantCulture) : "—";
    }

    private async Task SelectCmSegAsync(int idx, bool focus = true)
    {
        var seg = _cmSeg?.Segments.FirstOrDefault(seg => seg.Idx == idx);
        if (seg == null) return;
        _cmSegSelectedIdx = idx;
        OnSeekBarJlsBoundaryClick(seg.Start);
        if (focus) await JS.InvokeVoidAsync("trimAdjustFocusCmSeg", _cmSegPanelRef, idx);
    }

    private async Task OnCmSegFilterChanged(ChangeEventArgs e)
    {
        _cmSegShortOnly = e.Value is true;
        var visible = VisibleCmSegSegments().ToList();
        if (!visible.Any(seg => seg.Idx == _cmSegSelectedIdx))
        {
            _cmSegSelectedIdx = visible.FirstOrDefault()?.Idx ?? -1;
            if (_cmSegSelectedIdx >= 0) await SelectCmSegAsync(_cmSegSelectedIdx, false);
        }
    }

    private void OnCmSegReviewedChanged(ChangeEventArgs e)
    {
        if (_cmSeg == null) return;
        _cmSeg.Reviewed = e.Value is true;
        ScheduleCmSegSave();
    }

    private void OnCmSegLabelChanged(CmSegAnnotationSegment seg, ChangeEventArgs e)
    {
        SetCmSegLabel(seg, e.Value?.ToString() ?? "unknown");
    }

    private void SetCmSegLabel(CmSegAnnotationSegment seg, string label)
    {
        if (seg.Label == label) return;
        seg.Label = label;
        seg.Edited = label != CmSegLabels.InitialLabel(seg.JlsLabel, seg.JlsKeep);
        ScheduleCmSegSave();
    }

    [JSInvokable]
    public async Task OnCmSegKeyDown(int idx, string key, bool shift)
    {
        var visible = VisibleCmSegSegments().ToList();
        var pos = visible.FindIndex(seg => seg.Idx == idx);
        if (pos < 0) return;
        var choice = CmSegLabels.Classes.FirstOrDefault(choice => choice.Key == key);
        if (choice.Id != null) SetCmSegLabel(visible[pos], choice.Id);
        var direction = key == "ArrowUp" || key == "Tab" && shift ? -1 : 1;
        await SelectCmSegAsync(visible[Math.Clamp(pos + direction, 0, visible.Count - 1)].Idx);
        StateHasChanged();
    }

    private void ScheduleCmSegSave()
    {
        _cmSegDirty = true;
        _cmSegRevision++;
        _cmSegSaveError = false;
        _cmSegStatus = "未保存";
        _cmSegDebounce?.Cancel();
        _cmSegDebounce?.Dispose();
        _cmSegDebounce = new CancellationTokenSource();
        _ = SaveCmSegAfterDelayAsync(_cmSegDebounce.Token);
    }

    private async Task SaveCmSegAfterDelayAsync(CancellationToken token)
    {
        try
        {
            await Task.Delay(500, token);
            await InvokeAsync(FlushCmSegAsync);
        }
        catch (OperationCanceledException) { }
    }

    private async Task OnCmSegBeforeNavigationAsync(LocationChangingContext context)
    {
        await FlushCmSegAsync();
        if (_cmSegDirty) context.PreventNavigation();
    }

    private async Task FlushCmSegAsync()
    {
        _cmSegDebounce?.Cancel();
        await _cmSegSaveLock.WaitAsync();
        try
        {
            while (_cmSegDirty && _cmSeg != null && !string.IsNullOrEmpty(_sessionId))
            {
                var revision = _cmSegRevision;
                var sessionId = _sessionId;
                var request = new CmSegSaveRequest
                {
                    Reviewed = _cmSeg.Reviewed,
                    Segments = _cmSeg.Segments.Select(seg => new CmSegLabelUpdate { Idx = seg.Idx, Label = seg.Label }).ToList()
                };
                _cmSegStatus = "保存中...";
                if (!_cmSegDisposing) StateHasChanged();
                var result = await Api.SaveCmSegAnnotationAsync(sessionId, request);
                _cmSegSaveError = !result.Ok;
                if (!result.Ok)
                {
                    // 一度検知した競合は、新しいセッションの読込まで維持する。
                    if (result.StatusCode == 409) _cmSegSaveConflict = true;
                    _cmSegStatus = result.Error ?? "分類の保存に失敗しました";
                    break;
                }
                // 基準は成功した送信内容とし、保存中の追加入力を混ぜない。
                _cmSegBaseline = CmSegLabelSignature(request.Reviewed, request.Segments);
                // 保存中に入力された変更は、同じロック内で続けて保存する。
                if (revision == _cmSegRevision) _cmSegDirty = false;
                _cmSegStatus = _cmSegDirty ? "未保存" : "保存済み";
            }
        }
        catch (Exception ex)
        {
            _cmSegSaveError = true;
            _cmSegStatus = $"分類の保存に失敗しました: {ex.Message}";
        }
        finally
        {
            _cmSegSaveLock.Release();
            if (!_cmSegDisposing) StateHasChanged();
        }
    }
}

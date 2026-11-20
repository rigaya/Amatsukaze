using System.Globalization;
using System.Net;
using System.Net.Http.Json;
using System.Text.Json.Nodes;
using Amatsukaze.Shared;
using Microsoft.AspNetCore.Components;
using Microsoft.AspNetCore.Components.Web;
using Microsoft.JSInterop;

namespace AmatsukazeWebUI.Pages;

public partial class FilterAdjust
{
    [Parameter] public int QueueItemId { get; set; }
    private const string QualityKey = "amatsukaze.filterPreview.quality";
    private const string Root = "api/filter-preview/sessions";
    private readonly CancellationTokenSource _lifetime = new();
    private CancellationTokenSource? _configurationCancellation, _frameCancellation;
    private CancellationTokenSource _sessionCancellation = new();
    private Task _configurationTask = Task.CompletedTask, _initializationTask = Task.CompletedTask;
    private readonly HashSet<Task> _operations = new();
    private JsonObject? _profile;
    private ProfileOptions? _options;
    private IJSObjectReference? _renderer, _lifecycle, _canvasLifetime, _viewerModule, _viewer;
    private DotNetObjectReference<FilterAdjust>? _callback;
    private ElementReference _beforeCanvas, _afterCanvas, _viewport, _imageLayer, _divider, _fitButton, _actualSizeButton, _zoomLabel;
    private string? _sessionId, _error;
    private string _status = "読み込み中…", _view = "after", _scale = "1", _quality = "yuv", _saveName = "";
    private string[] _notices = [], _logs = [];
    private long _generation, _committedGeneration, _frameRevision;
    private int _position, _frames, _width, _height, _cropX, _cropY, _cropWidth = 640, _cropHeight = 360;
    private double _fps = 60000d / 1001, _inputDar;
    private bool _decimate = true, _pngFallback, _expired, _disposed, _saving, _cropEnabled;
    private static readonly (string Value, string Label)[] ViewChoices = [("after", "フィルタ後"), ("before", "フィルタ前"), ("compare", "左右比較")];
    private static readonly (string Value, string Label)[] ScaleChoices = [("1", "1/1"), ("2", "1/2")];
    private string? _sourceName;
    private string FrameTypeLabel = "";
    private bool? FrameKept;
    private bool DecimateInfoMissing, _reopening;
    private bool CustomReplaced;
    private int MaxFrame => Math.Max(0, _frames - 1);
    private bool CanOperate => !_disposed && !_expired && _sessionId != null && _frames > 0;
    private bool _busy => _reopening || _configurationCancellation != null || _frameCancellation != null;
    private bool Ready => !_disposed && !_expired && _sessionId != null && _frames > 0 && _generation == _committedGeneration;
    private string SessionUrl => $"{Root}/{_sessionId}";
    private string FilterKind => Node(_profile, "FilterOption")?.ToString() ?? "None";
    private bool IsEncoder => FilterKind.EndsWith("EncFilter", StringComparison.Ordinal);
    private string FpsLabel => _fps.ToString("0.###", CultureInfo.InvariantCulture) + "fps";
    private static JsonNode? Node(JsonObject? obj, string name) => obj?.FirstOrDefault(x => string.Equals(x.Key, name, StringComparison.OrdinalIgnoreCase)).Value;
    private static void Set(JsonObject obj, string key, JsonNode? value)
    {
        var existing = obj.FirstOrDefault(x => string.Equals(x.Key, key, StringComparison.OrdinalIgnoreCase)).Key;
        obj[existing ?? key] = value;
    }
    private static int Number(JsonObject obj, string name, int fallback = 0) => int.TryParse(Node(obj, name)?.ToString(), CultureInfo.InvariantCulture, out var value) ? value : fallback;

    protected override Task OnAfterRenderAsync(bool firstRender)
    {
        if (!firstRender) return Task.CompletedTask;
        _initializationTask = InitializeAsync();
        return _initializationTask;
    }
    private async Task InitializeAsync()
    {
        try
        {
            _renderer = await JS.InvokeAsync<IJSObjectReference>("import", "./js/filterPreview.js");
            _lifecycle = await JS.InvokeAsync<IJSObjectReference>("import", "./js/filterPreviewSession.js");
            _viewerModule = await JS.InvokeAsync<IJSObjectReference>("import", "./js/filterPreviewViewer.js");
            _callback = DotNetObjectReference.Create(this);
            _viewer = await _viewerModule.InvokeAsync<IJSObjectReference>("bind", _viewport, _imageLayer, _beforeCanvas, _afterCanvas,
                _divider, _fitButton, _actualSizeButton, _zoomLabel, _callback);
            _canvasLifetime = await _renderer.InvokeAsync<IJSObjectReference>("bindCanvases", _beforeCanvas, _afterCanvas, _callback);
            try { _quality = await JS.InvokeAsync<string?>("localStorage.getItem", QualityKey) == "jpeg" ? "jpeg" : "yuv"; }
            catch (JSException) { }
            // 作成結果を回収してから破棄するため、画面終了では作成要求を中断しない。
            using var createCancellation = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            using var create = await Http.PostAsJsonAsync(Root, new { queueItemId = QueueItemId }, createCancellation.Token);
            var session = await ReadJsonAsync(create, createCancellation.Token);
            _sessionId = Node(session, "sessionId")?.ToString() ?? throw new InvalidOperationException("セッションIDがありません");
            if (_disposed) return;
            await _lifecycle.InvokeVoidAsync("attach", new Uri(Http.BaseAddress!, SessionUrl).AbsoluteUri);
            using var configuration = await Http.GetAsync(SessionUrl + "/config", _lifetime.Token);
            _profile = await ReadJsonAsync(configuration, _lifetime.Token);
            _generation = Number(_profile, "gen");
            var options = await Api.GetProfileOptionsAsync().WaitAsync(_lifetime.Token);
            if (!options.Ok || options.Data == null) throw new InvalidOperationException("フィルタ設定項目を取得できません");
            _options = options.Data;
            _sourceName = Node(session, "sourceName")?.ToString();
            CustomReplaced = FilterKind is "Custom" or "2";
            if (CustomReplaced) Set(_profile, "FilterOption", JsonValue.Create("None"));
            else if (int.TryParse(FilterKind, out var filterIndex))
                Set(_profile, "FilterOption", JsonValue.Create(filterIndex switch { 1 => "Setting", 3 => "QSVEncFilter", 4 => "NVEncFilter", 5 => "VCEEncFilter", _ => "None" }));
            await SettingsChangedAsync();
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception ex) { ShowError(ex); }
        if (!_disposed) StateHasChanged();
    }

    private async Task<JsonObject> ReadJsonAsync(HttpResponseMessage response, CancellationToken token)
    {
        await CheckAsync(response, token);
        return await response.Content.ReadFromJsonAsync<JsonObject>(token) ?? throw new InvalidOperationException("応答が空です");
    }
    private async Task CheckAsync(HttpResponseMessage response, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        if (response.StatusCode == HttpStatusCode.NotFound)
        {
            _expired = true;
            throw new InvalidOperationException("セッションが終了しました。画面を開き直してください(無操作で5分経つと終了します)。");
        }
        if (!response.IsSuccessStatusCode)
        {
            var text = await response.Content.ReadAsStringAsync(token);
            token.ThrowIfCancellationRequested();
            try { text = JsonNode.Parse(text)?["message"]?.ToString() ?? text; } catch (System.Text.Json.JsonException) { }
            throw new InvalidOperationException(string.IsNullOrWhiteSpace(text) ? $"操作に失敗しました({(int)response.StatusCode})" : text);
        }
    }
    private void ShowError(Exception ex) { if (!_disposed) { _error = ex.Message; _status = "操作に失敗しました"; } }
    private async Task KindChangedAsync(ChangeEventArgs e)
    {
        if (_profile == null) return;
        CustomReplaced = false;
        Set(_profile, "FilterOption", JsonValue.Create(e.Value?.ToString() ?? "None"));
        await SettingsChangedAsync();
    }
    private Task SettingsChangedAsync()
    {
        if (_disposed || _expired || _profile == null || _sessionId == null) return Task.CompletedTask;
        _configurationCancellation?.Cancel();
        _frameCancellation?.Cancel();
        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token, _sessionCancellation.Token);
        _configurationCancellation = cancellation;
        var gen = ++_generation;
        var configuration = (JsonObject)_profile.DeepClone();
        Set(configuration, "gen", JsonValue.Create(gen));
        _status = "設定を反映中…";
        _error = null;
        _configurationTask = ConfigureAsync(configuration, gen, cancellation);
        return TrackAsync(_configurationTask);
    }
    private async Task TrackAsync(Task operation)
    {
        _operations.Add(operation);
        try { await operation; } finally { _operations.Remove(operation); }
    }
    private async Task ConfigureAsync(JsonObject configuration, long gen, CancellationTokenSource cancellation)
    {
        var token = cancellation.Token;
        try
        {
            if (gen != _generation || token.IsCancellationRequested) return;
            await InvalidateImagesAsync();
            await Task.Delay(150, token);
            using var response = await Http.PutAsJsonAsync(SessionUrl + "/config", configuration, token);
            var result = await ReadJsonAsync(response, token);
            if (gen != _generation || _disposed) return;
            var info = Node(result, "info") as JsonObject ?? throw new InvalidOperationException("映像情報がありません");
            var seconds = _fps > 0 ? _position / _fps : 0;
            int num = Number(info, "fpsnum"), den = Number(info, "fpsden", 1);
            _fps = num > 0 && den > 0 ? (double)num / den : _fps;
            _frames = Number(info, "frames");
            _width = Number(info, "width"); _height = Number(info, "height");
            _position = Math.Clamp((int)Math.Round(seconds * _fps), 0, Math.Max(0, _frames - 1));
            _notices = (Node(result, "notices") as JsonArray)?.Select(x => x?.ToString() ?? "").ToArray() ?? [];
            _committedGeneration = gen;
            await ReadInputAspectAsync(info, gen, token);
            await RefreshAsync();
            await LoadLogsAsync();
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { if (gen == _generation && !token.IsCancellationRequested) ShowError(ex); }
        finally { if (ReferenceEquals(_configurationCancellation, cancellation)) _configurationCancellation = null; cancellation.Dispose(); }
    }
    private async Task ReadInputAspectAsync(JsonObject info, long gen, CancellationToken token)
    {
        if (_inputDar > 0) return;
        var filter = Node(_profile, "FilterSetting") as JsonObject;
        bool resized = FilterKind == "Setting" && bool.TryParse(Node(filter, "EnableResize")?.ToString(), out var enabled) && enabled;
        if (!resized)
        {
            int sn = Number(info, "sarnum", 1), sd = Number(info, "sarden", 1);
            _inputDar = _height > 0 ? (double)_width / _height * (sn > 0 && sd > 0 ? (double)sn / sd : 1) : 0;
            return;
        }
        // リサイズ済みプロファイルで開いた場合だけ、加工前フレームのヘッダから入力DARを求める。
        using var response = await Http.PostAsJsonAsync(SessionUrl + "/frame", new { gen, pos = 0, side = "before", scale = 1, format = "jpeg", decimate = false }, token);
        await CheckAsync(response, token);
        _ = await response.Content.ReadAsByteArrayAsync(token);
        if (gen != _generation || token.IsCancellationRequested) return;
        int Header(string name, int fallback = 0) => response.Headers.TryGetValues("X-Filter-Preview-" + name, out var values)
            && int.TryParse(values.FirstOrDefault(), CultureInfo.InvariantCulture, out var value) ? value : fallback;
        int width = Header("width"), height = Header("height"), numerator = Header("sarnum", 1), denominator = Header("sarden", 1);
        if (width <= 0 || height <= 0) throw new InvalidOperationException("入力映像の表示アスペクト比を取得できません");
        _inputDar = (double)width / height * (numerator > 0 && denominator > 0 ? (double)numerator / denominator : 1);
    }
    private Task RefreshAsync() => StartFrameAsync(null);
    private Task StepAsync(int direction)
    {
        if (CanOperate && !Ready)
        {
            _position = Math.Clamp(_position + direction, 0, MaxFrame);
            return Task.CompletedTask;
        }
        return StartFrameAsync(direction);
    }
    [JSInvokable] public Task StepPreviewAsync(int direction) => InvokeAsync(async () =>
    {
        if (!CanOperate || direction is not (-1 or 1) || _position + direction < 0 || _position + direction > MaxFrame) return;
        await StepAsync(direction);
        if (!_disposed) StateHasChanged();
    });
    private async Task SetViewAsync(string value)
    {
        _view = value;
        await _viewer!.InvokeVoidAsync("setView", value == "compare");
        await RefreshAsync();
    }
    private Task SetScaleAsync(string value) { _scale = value; return RefreshAsync(); }
    private Task SetQualityAsync(string value) { _quality = value; return QualityChangedAsync(); }
    private async Task OnKeyDown(KeyboardEventArgs e)
    {
        if (e.Key is not ("ArrowLeft" or "ArrowRight") || !CanOperate || _lifecycle == null) return;
        if (await _lifecycle.InvokeAsync<bool>("inputFocused")) return;
        await StepAsync(e.Key == "ArrowLeft" ? -1 : 1);
    }
    private async Task SeekAsync(ChangeEventArgs e)
    {
        if (!CanOperate || !int.TryParse(e.Value?.ToString(), out var pos)) return;
        _position = Math.Clamp(pos, 0, Math.Max(0, _frames - 1));
        await RefreshAsync();
    }
    private Task StartFrameAsync(int? step)
    {
        if (!Ready) return Task.CompletedTask;
        _frameCancellation?.Cancel();
        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token, _sessionCancellation.Token);
        _frameCancellation = cancellation;
        return TrackAsync(RenderFramesAsync(_generation, ++_frameRevision, step, cancellation));
    }
    private async Task RenderFramesAsync(long gen, long revision, int? step, CancellationTokenSource cancellation)
    {
        var token = cancellation.Token;
        bool Current() => !_disposed && gen == _generation && revision == _frameRevision && !token.IsCancellationRequested;
        try
        {
            if (!Current()) return;
            await InvalidateImagesAsync();
            if (!Current()) return;
            _status = "フレームを取得中…";
            _error = null;
            var pos = _position;
            var view = _view;
            var options = CaptureFrameOptions();
            PreviewFrame? after = null, before = null;
            if (view == "compare" && !step.HasValue)
            {
                // 取得を同時に開始し、両方がそろってから新しい比較画像を描く。
                var afterTask = FetchFrameAsync("after", pos, null, gen, options, token);
                var beforeTask = FetchFrameAsync("before", pos, null, gen, options, token);
                await Task.WhenAll(afterTask, beforeTask);
                after = await afterTask;
                before = await beforeTask;
            }
            else
            {
                // コマ送り先はフィルタ後の並びで決め、比較画像も同じ位置で取得する。
                if (view != "before" || step.HasValue)
                {
                    after = await FetchFrameAsync("after", pos, step, gen, options, token);
                    pos = after.Position;
                }
                if (view != "after") before = await FetchFrameAsync("before", pos, null, gen, options, token);
            }
            if (!Current()) return;
            await Task.WhenAll(
                after == null ? Task.CompletedTask : DrawFrameAsync(_afterCanvas, "after", after, gen, options, Current, token),
                before == null ? Task.CompletedTask : DrawFrameAsync(_beforeCanvas, "before", before, gen, options, Current, token));
            if (!Current()) return;
            // 比較中はフィルタ後の原寸に合わせ、前後を同じ入力DARで描く。
            var display = view == "before" ? before! : after!;
            var height = options.Crop?[3] ?? display.Height * options.Scale;
            var dar = _inputDar > 0 ? _inputDar : display.Width * display.Sar / display.Height;
            await _viewer!.InvokeVoidAsync("setSize", height * dar, height);
            if (!Current()) return;
            if (after != null) UpdateFrameInfo(after.Metadata);
            pos = after?.Position ?? before!.Position;
            _position = pos;
            _status = _pngFallback && _quality == "yuv" ? "表示しました(PNG代替)" : "表示しました";
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { if (Current()) ShowError(ex); }
        finally { if (ReferenceEquals(_frameCancellation, cancellation)) _frameCancellation = null; cancellation.Dispose(); }
    }
    private sealed record FrameOptions(int Scale, string Format, int[]? Crop, bool Decimate);
    private sealed record PreviewFrame(byte[] Data, Dictionary<string, string> Metadata, string? Mime, int Position)
    {
        public int Width => int.Parse(Metadata["width"], CultureInfo.InvariantCulture);
        public int Height => int.Parse(Metadata["height"], CultureInfo.InvariantCulture);
        public double Sar => Metadata.TryGetValue("sarnum", out var sn) && double.TryParse(sn, CultureInfo.InvariantCulture, out var numerator)
            && Metadata.TryGetValue("sarden", out var sd) && double.TryParse(sd, CultureInfo.InvariantCulture, out var denominator)
            && numerator > 0 && denominator > 0 ? numerator / denominator : 1;
    }
    private FrameOptions CaptureFrameOptions()
    {
        var format = _quality == "jpeg" ? "jpeg" : _pngFallback ? "png" : "yuv";
        int[]? crop = null;
        if (_cropEnabled)
        {
            if (_cropX < 0 || _cropY < 0 || _cropWidth <= 0 || _cropHeight <= 0 || (_cropX | _cropY | _cropWidth | _cropHeight) % 2 != 0)
                throw new InvalidOperationException("切り出し範囲には正の寸法と偶数の開始位置・寸法を指定してください");
            crop = [_cropX, _cropY, _cropWidth, _cropHeight];
        }
        return new FrameOptions(_scale == "2" ? 2 : 1, format, crop, _decimate);
    }
    private async Task<PreviewFrame> FetchFrameAsync(string side, int pos, int? step, long gen, FrameOptions options, CancellationToken token)
    {
        using var response = await Http.PostAsJsonAsync(SessionUrl + "/frame",
            new { gen, pos, side, scale = options.Scale, format = options.Format, crop = options.Crop, step, decimate = options.Decimate }, token);
        await CheckAsync(response, token);
        var metadata = response.Headers.Where(x => x.Key.StartsWith("X-Filter-Preview-", StringComparison.OrdinalIgnoreCase))
            .ToDictionary(x => x.Key["X-Filter-Preview-".Length..].ToLowerInvariant(), x => string.Join(",", x.Value));
        if (!metadata.TryGetValue("gen", out var returnedGen) || returnedGen != gen.ToString(CultureInfo.InvariantCulture)) throw new InvalidOperationException("フレームの世代が一致しません");
        var bytes = await response.Content.ReadAsByteArrayAsync(token);
        var actual = metadata.TryGetValue("pos", out var value) && int.TryParse(value, out var result) ? result : pos;
        return new PreviewFrame(bytes, metadata, response.Content.Headers.ContentType?.MediaType, actual);
    }
    private async Task DrawFrameAsync(ElementReference canvas, string side, PreviewFrame frame, long gen, FrameOptions options, Func<bool> current, CancellationToken token)
    {
        bool success = await _renderer!.InvokeAsync<bool>("draw", canvas, frame.Data, frame.Metadata, frame.Mime);
        if (!current()) return;
        if (!success && options.Format == "yuv")
        {
            _pngFallback = true;
            // 同じ位置のPNGを頼むため、コマ送りは再適用しない。
            var pngOptions = options with { Format = "png" };
            var png = await FetchFrameAsync(side, frame.Position, null, gen, pngOptions, token);
            if (current()) await DrawFrameAsync(canvas, side, png, gen, pngOptions, current, token);
        }
        else if (!success) throw new InvalidOperationException("画像を描画できません");
    }
    private void UpdateFrameInfo(Dictionary<string, string> metadata)
    {
        var type = metadata.GetValueOrDefault("type", "unknown");
        var duration = metadata.GetValueOrDefault("dur", "0");
        FrameTypeLabel = type is not ("unknown" or "-" or "none") ? $"{type} {duration}/60" : "";
        FrameKept = metadata.GetValueOrDefault("kept") switch { "1" => true, "0" => false, _ => null };
        var filter = Node(_profile, "FilterSetting") as JsonObject;
        DecimateInfoMissing = FilterKind == "Setting" && type is "unknown" or "none"
            && bool.TryParse(Node(filter, "EnableDeinterlace")?.ToString(), out var enabled) && enabled
            && Node(filter, "DeinterlaceAlgorithm")?.ToString() == "KFM";
    }
    private async Task InvalidateImagesAsync()
    {
        if (_renderer == null) return;
        await _renderer.InvokeVoidAsync("invalidateAll", _beforeCanvas, _afterCanvas);
    }
    [JSInvokable] public async Task OnPreviewContextLost()
    {
        if (_disposed) return;
        _pngFallback = true;
        await InvokeAsync(async () => { await RefreshAsync(); StateHasChanged(); });
    }
    private async Task QualityChangedAsync()
    {
        try { await JS.InvokeVoidAsync("localStorage.setItem", QualityKey, _quality); } catch (JSException) { }
        await RefreshAsync();
    }
    private async Task LoadLogsAsync()
    {
        if (_sessionId == null || _disposed) return;
        var token = _sessionCancellation.Token;
        try
        {
            using var response = await Http.GetAsync(SessionUrl + "/logs", token);
            await CheckAsync(response, token);
            var logs = await response.Content.ReadFromJsonAsync<string[]>(token) ?? [];
            token.ThrowIfCancellationRequested();
            _logs = logs;
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { if (!token.IsCancellationRequested) ShowError(ex); }
    }
    private async Task SaveAsync(bool saveAs)
    {
        if (!Ready || _saving) return;
        var token = _sessionCancellation.Token;
        if (!await JS.InvokeAsync<bool>("confirm", saveAs ? $"「{_saveName}」として保存しますか？" : "元のプロファイルのフィルタ設定を上書きしますか？")) return;
        _saving = true;
        try
        {
            await _configurationTask;
            if (!Ready) return;
            using var response = await Http.PostAsJsonAsync(SessionUrl + "/save", new { saveAs, name = _saveName, preserveCustomFilter = CustomReplaced }, token);
            await CheckAsync(response, token);
            _error = null; _status = "プロファイルへ保存しました";
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { if (!token.IsCancellationRequested) ShowError(ex); }
        finally { _saving = false; }
    }
    private Task ReopenAsync()
    {
        if (_disposed || _reopening) return Task.CompletedTask;
        _initializationTask = ReopenCoreAsync();
        return _initializationTask;
    }
    private async Task ReopenCoreAsync()
    {
        _reopening = true;
        _sessionCancellation.Cancel();
        _configurationCancellation?.Cancel();
        _frameCancellation?.Cancel();
        try
        {
            await Task.WhenAll(_operations.ToArray());
            _sessionCancellation.Dispose();
            _sessionCancellation = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
            if (_sessionId != null)
            {
                using var close = await Http.DeleteAsync(SessionUrl, _lifetime.Token);
                await _lifecycle!.InvokeVoidAsync("detach");
                _sessionId = null;
            }
            using var creationCancellation = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            using var creation = await Http.PostAsJsonAsync(Root, new { queueItemId = QueueItemId }, creationCancellation.Token);
            var session = await ReadJsonAsync(creation, creationCancellation.Token);
            _sessionId = Node(session, "sessionId")?.ToString() ?? throw new InvalidOperationException("セッションIDがありません");
            if (_disposed) return;
            await _lifecycle!.InvokeVoidAsync("attach", new Uri(Http.BaseAddress!, SessionUrl).AbsoluteUri);
            _expired = false;
            _error = null;
            // 編集中の設定と現在位置をそのまま新しいセッションへ送り直す。
            await SettingsChangedAsync();
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception ex) { ShowError(ex); }
        finally { _reopening = false; }
    }
    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        _lifetime.Cancel();
        _sessionCancellation.Cancel();
        await _initializationTask;
        try { await Task.WhenAll(_operations.ToArray()); } catch (OperationCanceledException) { }
        if (_sessionId != null)
        {
            try { using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5)); using var response = await Http.DeleteAsync(SessionUrl, timeout.Token); } catch (HttpRequestException) { } catch (OperationCanceledException) { }
        }
        try
        {
            if (_lifecycle != null) { await _lifecycle.InvokeVoidAsync("detach"); await _lifecycle.DisposeAsync(); }
            if (_canvasLifetime != null) { await _canvasLifetime.InvokeVoidAsync("dispose"); await _canvasLifetime.DisposeAsync(); }
            if (_viewer != null) { await _viewer.InvokeVoidAsync("dispose"); await _viewer.DisposeAsync(); }
            if (_viewerModule != null) await _viewerModule.DisposeAsync();
            if (_renderer != null) await _renderer.DisposeAsync();
        }
        catch (JSDisconnectedException) { }
        _callback?.Dispose();
        _sessionCancellation.Dispose();
        _lifetime.Dispose();
    }
}

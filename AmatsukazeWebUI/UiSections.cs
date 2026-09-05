namespace AmatsukazeWebUI;

/// <summary>
/// レイアウト直下へ描画したい要素の受け口を識別するためのセクションID。
/// </summary>
public static class UiSections
{
    /// <summary>
    /// モーダルダイアログの描画先。
    /// backdrop-filter や transform を持つ要素の内側では position:fixed が
    /// その要素基準になってしまうため、オーバーレイは必ずここへ流す。
    /// </summary>
    public static readonly object OverlayDialogs = new();
}

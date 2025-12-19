#nullable enable
using System.Windows;
using System.Windows.Controls;

namespace Amatsukaze.Components;

/// <summary>
/// ToolTip の Content が文字列のときだけ特定のテンプレートを適用するためのセレクタ。
/// Fluent(ライト/ダーク)テーマで ToolTip 文字列が折り返されずにクリップされる対策用。
/// </summary>
public sealed class ToolTipContentTemplateSelector : DataTemplateSelector
{
    public DataTemplate? StringTemplate { get; set; }

    public override DataTemplate? SelectTemplate(object item, DependencyObject container)
        => item is string ? StringTemplate : null;
}


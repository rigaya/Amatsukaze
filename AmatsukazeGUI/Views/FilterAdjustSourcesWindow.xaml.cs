using Amatsukaze.Models;
using Amatsukaze.Server;
using Microsoft.Win32;
using System.IO;
using System.Net.Http;
using System.Windows;

namespace Amatsukaze.Views
{
    public partial class FilterAdjustSourcesWindow : Window
    {
        private readonly ClientModel model;

        internal FilterAdjustSourcesWindow(ClientModel model)
        {
            this.model = model;
            InitializeComponent();
            Profiles.ItemsSource = model.ProfileListView;
            Profiles.SelectedItem = model.SelectedProfile;
        }

        private async void LoadSources(object sender, RoutedEventArgs e)
        {
            var baseUrl = WebUILauncher.TryGetBaseUrl(model);
            if (baseUrl == null)
            {
                Status.Text = "WebUIの接続先が取得できません。";
                return;
            }

            Status.Text = "素材を読み込み中…";
            using var client = new HttpClient { BaseAddress = new Uri(baseUrl), Timeout = TimeSpan.FromSeconds(3) };
            var result = await new Shared.AmatsukazeApi(client).GetFilterPreviewSourcesAsync();
            if (!result.Ok || result.Data == null)
            {
                Status.Text = "素材一覧を取得できませんでした。" + (result.Error ?? "");
                return;
            }

            Sources.ItemsSource = result.Data.Select(source =>
                new SourceRow(source.QueueItemId, source.FileName, source.ProfileName, source.GetModeLabel())).ToList();
            Status.Text = result.Data.Count == 0 ? "利用できる素材はありません。" : "";
        }

        private sealed record SourceRow(int QueueItemId, string FileName, string ProfileName, string ModeText);

        private string GetProfileName()
        {
            if (Profiles.SelectedItem is DisplayProfile selected && !selected.HasUnappliedChanges())
                return selected.Name;

            Status.Text = "プロファイルを選び、未適用の変更がある場合は適用してから開いてください。";
            return null;
        }

        private void OpenSource(object sender, RoutedEventArgs e)
        {
            if (Sources.SelectedItem is not SourceRow source)
                return;

            var profile = GetProfileName();
            if (profile == null)
                return;

            if (WebUILauncher.TryOpenPage(model, $"/filter-adjust/{source.QueueItemId}?profile={Uri.EscapeDataString(profile)}"))
                Close();
            else
                Status.Text = "WebUIのフィルタ調整画面を開けませんでした。";
        }

        private void Browse(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "TSファイル (*.ts)|*.ts", CheckFileExists = true };
            if (dialog.ShowDialog(this) == true)
                InputPath.Text = dialog.FileName;
        }

        private static string GetDroppedTs(DragEventArgs e) =>
            e.Data.GetData(DataFormats.FileDrop) is string[] { Length: 1 } files &&
            string.Equals(Path.GetExtension(files[0]), ".ts", StringComparison.OrdinalIgnoreCase) ? files[0] : null;

        private void PreviewInputDragOver(object sender, DragEventArgs e)
        {
            e.Effects = GetDroppedTs(e) == null ? DragDropEffects.None : DragDropEffects.Copy;
            e.Handled = true;
        }

        private void DropInput(object sender, DragEventArgs e)
        {
            var path = GetDroppedTs(e);
            if (path != null)
                InputPath.Text = path;
            e.Handled = true;
        }

        private async void AddSource(object sender, RoutedEventArgs e)
        {
            var profile = GetProfileName();
            if (profile == null)
                return;

            var input = InputPath.Text.Trim();
            if (!Path.IsPathFullyQualified(input) || !string.Equals(Path.GetExtension(input), ".ts", StringComparison.OrdinalIgnoreCase))
            {
                Status.Text = "入力TSファイルのフルパスを指定してください。";
                return;
            }

            var directory = Path.GetDirectoryName(input);
            AddButton.IsEnabled = false;
            try
            {
                var server = model.Server ?? throw new InvalidOperationException("サーバーに接続していません。");
                await server.AddQueue(new AddQueueRequest
                {
                    Mode = ProcMode.FilterAdjust,
                    DirPath = directory,
                    Targets = new() { new AddQueueItem { Path = input } },
                    Outputs = new() { new OutputInfo { DstPath = directory, Profile = profile, Priority = 3 } },
                });
                Close();
            }
            catch (Exception ex)
            {
                Status.Text = "キューに追加できませんでした。" + ex.Message;
            }
            finally
            {
                AddButton.IsEnabled = true;
            }
        }
    }
}

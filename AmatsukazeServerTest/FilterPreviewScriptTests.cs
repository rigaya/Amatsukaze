using System;
using System.IO;
using System.Linq;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

public class FilterPreviewScriptTests
{
    [Fact]
    public void KFMは本番のパス指定を維持しプレビューだけリアルタイムにする()
    {
        var filter = new FilterSetting
        {
            EnableDeinterlace = true,
            DeinterlaceAlgorithm = DeinterlaceAlgorithm.KFM,
            KfmFps = FilterFPS.VFR
        };
        var normal = AvsScriptCreator.FilterToString(filter, new Setting(), false);
        var preview = AvsScriptCreator.FilterToString(filter, new Setting(), false, true);
        Assert.Contains("pass = Select(AMT_PASS, 1, 2, 3)", normal);
        Assert.Contains("AMT_PRE_PROC = (AMT_PASS < 2)", normal);
        Assert.Contains("if(AMT_PASS != 2) { return last }", normal);
        Assert.Contains("pass = 0", preview);
        Assert.DoesNotContain("AMT_PRE_PROC", preview);
        Assert.DoesNotContain("return last", preview);
    }

    [Fact]
    public void AutoVfrはプレビューではインタレ解除せず後処理を残す()
    {
        var filter = new FilterSetting
        {
            EnableDeinterlace = true,
            DeinterlaceAlgorithm = DeinterlaceAlgorithm.AutoVfr,
            EnableResize = true,
            ResizeWidth = 1280,
            ResizeHeight = 720
        };
        var preview = AvsScriptCreator.FilterToString(filter, new Setting(), false, true);
        Assert.DoesNotContain("Auto_Vfr", preview);
        Assert.Contains("BlackmanResize(1280,720)", preview);
        Assert.Single(FilterPreviewService.GetNotices(FilterOption.Setting, filter));
        Assert.Empty(FilterPreviewService.GetNotices(FilterOption.None, filter));
    }
    [Fact]
    public void 自動選択プラグインは対応する最大の命令セットを選ぶ()
    {
        var dir = Path.Combine(Path.GetTempPath(), "filter-preview-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            foreach (var name in new[] { "a.dll", "a_avx.dll", "a_avx2.dll", "b.dll", "c_avx2.dll" })
                File.WriteAllText(Path.Combine(dir, name), "");
            Assert.Equal(new[] { "a.dll", "b.dll" }, Array.ConvertAll(FilterPreviewScripts.SelectPlugins(dir, false, false).ToArray(), Path.GetFileName));
            Assert.Equal(new[] { "a_avx.dll", "b.dll" }, Array.ConvertAll(FilterPreviewScripts.SelectPlugins(dir, true, false).ToArray(), Path.GetFileName));
            Assert.Equal(new[] { "a_avx2.dll", "b.dll", "c_avx2.dll" }, Array.ConvertAll(FilterPreviewScripts.SelectPlugins(dir, true, true).ToArray(), Path.GetFileName));
        }
        finally
        {
            foreach (var file in Directory.GetFiles(dir)) File.Delete(file);
            Directory.Delete(dir);
        }
    }
    [Fact]
    public void スクリプトはBOMを付けずに保存する()
    {
        var path = Path.GetTempFileName();
        try
        {
            FilterPreviewScripts.WriteScript(path, "BlankClip()\n");
            Assert.Equal((byte)'B', File.ReadAllBytes(path)[0]);
        }
        finally { File.Delete(path); }
    }
}

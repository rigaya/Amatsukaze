using System;
using System.Threading.Tasks;
using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public class FilterPreviewProfileTests
{
    [Fact]
    public void 別名保存はフィルタだけを複製して他の項目を保持する()
    {
        var source = new ProfileSetting
        {
            Name = "元", RenameFormat = "出力形式", QSVEncOption = "--quality quality",
            ForceSAR = true, ForceSARWidth = 4, ForceSARHeight = 3,
            FilterOption = FilterOption.None, FilterSetting = new FilterSetting { EnableResize = false },
            EncoderFilterSetting = new EncoderFilterSetting()
        };
        var edited = new ProfileSetting
        {
            FilterOption = FilterOption.Setting,
            FilterSetting = new FilterSetting { EnableResize = true, ResizeWidth = 1280, ResizeHeight = 720 },
            EncoderFilterSetting = new EncoderFilterSetting(),
            QSVEncFilterOption = "--vpp-knn", NVEncFilterOption = "--vpp-deband", VCEEncFilterOption = "--vpp-resize"
        };
        var result = FilterPreviewProfile.Apply(source, edited, "別名");
        Assert.Equal("別名", result.Name);
        Assert.Equal(source.RenameFormat, result.RenameFormat);
        Assert.Equal(source.QSVEncOption, result.QSVEncOption);
        Assert.True(result.ForceSAR);
        Assert.Equal(4, result.ForceSARWidth);
        Assert.Equal(3, result.ForceSARHeight);
        Assert.Equal(FilterOption.Setting, result.FilterOption);
        Assert.True(result.FilterSetting.EnableResize);
        Assert.Equal(1280, result.FilterSetting.ResizeWidth);
        Assert.Equal("--vpp-knn", result.QSVEncFilterOption);
        Assert.Equal("--vpp-deband", result.NVEncFilterOption);
        Assert.Equal("--vpp-resize", result.VCEEncFilterOption);
        result.FilterSetting.ResizeWidth = 1920;
        Assert.Equal(1280, edited.FilterSetting.ResizeWidth);
        Assert.False(source.FilterSetting.EnableResize);
    }
    [Fact]
    public async Task 保存は他の設定を変えず重複名や不正名を拒否する()
    {
        await using var fixture = new RestFileSystemFixture();
        var original = ServerSupport.NormalizeProfile(null!);
        original.Name = "元";
        original.RenameFormat = "";
        original.X264Option = "保持する";
        fixture.Profiles.Add(original.Name, original);
        var edited = ServerSupport.DeepCopy(original);
        edited.FilterOption = FilterOption.Setting;
        edited.FilterSetting.EnableResize = true;
        edited.FilterSetting.ResizeWidth = 1280;
        await fixture.Api.Queue.Server.SaveFilterPreviewProfile(edited, "別名", true);
        Assert.Equal("", fixture.Profiles["別名"].RenameFormat);
        Assert.Equal("保持する", fixture.Profiles["別名"].X264Option);
        Assert.True(fixture.Profiles["別名"].FilterSetting.EnableResize);
        Assert.False(fixture.Profiles["元"].FilterSetting.EnableResize);
        Assert.Throws<InvalidOperationException>(() => { _ = fixture.Api.Queue.Server.SaveFilterPreviewProfile(edited, "別名", true); });
        Assert.Throws<ArgumentException>(() => { _ = fixture.Api.Queue.Server.SaveFilterPreviewProfile(edited, "../不正", true); });
        await fixture.Api.Queue.Server.SaveFilterPreviewProfile(edited, null!, false);
        Assert.True(fixture.Profiles["元"].FilterSetting.EnableResize);
        Assert.Equal("", fixture.Profiles["元"].RenameFormat);
    }
}

using Amatsukaze.Server;
using System.Runtime.Serialization;
using System.Xml.Linq;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class NicoJKReferenceProfileTests
{
    [Fact]
    public void 新規プロファイルでは別局実況を使わない()
    {
        Assert.Equal(0, ServerSupport.NormalizeProfile(null).NicoJKReferenceMode);
    }

    [Fact]
    public void 設定がない旧プロファイルでは別局実況を使わない()
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting { NicoJKReferenceMode = 2 });
        stream.Position = 0;
        var document = XDocument.Load(stream);
        document.Root!.Elements().Single(x => x.Name.LocalName == "NicoJKReferenceMode").Remove();
        using var reader = document.CreateReader();
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(reader));
        Assert.Equal(0, ServerSupport.NormalizeProfile(restored).NicoJKReferenceMode);
    }

    [Theory]
    [InlineData(0, "使用しない")]
    [InlineData(1, "置き換える")]
    [InlineData(2, "統合する")]
    public void 設定を保存復元して一覧に表示する(int mode, string label)
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting { NicoJKReferenceMode = mode });
        stream.Position = 0;
        var restored = ServerSupport.NormalizeProfile(Assert.IsType<ProfileSetting>(serializer.ReadObject(stream)));
        Assert.Equal(mode, restored.NicoJKReferenceMode);
        var description = restored.ToLongString();
        Assert.Contains("アニメの別放送のコメント", description);
        Assert.Contains(label, description);
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(3)]
    [InlineData(int.MinValue)]
    [InlineData(int.MaxValue)]
    public void 範囲外の値は使用しないに戻す(int mode)
    {
        var profile = ServerSupport.NormalizeProfile(new ProfileSetting { NicoJKReferenceMode = mode });
        Assert.Equal(0, profile.NicoJKReferenceMode);
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    public void 実況を無効にしても選んだモードは保持する(int mode)
    {
        var profile = ServerSupport.NormalizeProfile(new ProfileSetting { EnableNicoJK = false, NicoJKReferenceMode = mode });
        Assert.False(profile.EnableNicoJK);
        Assert.Equal(mode, profile.NicoJKReferenceMode);
    }
}

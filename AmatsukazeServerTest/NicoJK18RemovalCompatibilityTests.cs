using System.Runtime.Serialization;
using System.Text.Json;
using System.Xml.Linq;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class NicoJK18RemovalCompatibilityTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void 旧XMLの削除済み設定を無視してコメント設定を保持する(bool oldValue)
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting
        {
            Name = "旧プロファイル",
            EnableNicoJK = true,
            NicoJKLog = true,
            NicoJKFormats = new[] { true, false, true, false }
        });
        stream.Position = 0;
        var document = XDocument.Load(stream);
        Assert.DoesNotContain(document.Descendants(), element => element.Name.LocalName == "NicoJK18");
        var formats = Assert.Single(document.Descendants(), element => element.Name.LocalName == "NicoJKFormats");
        formats.AddBeforeSelf(new XElement(formats.Name.Namespace + "NicoJK18", oldValue));

        using var reader = document.CreateReader();
        var profile = ServerSupport.NormalizeProfile((ProfileSetting)serializer.ReadObject(reader)!);

        Assert.Equal("旧プロファイル", profile.Name);
        Assert.True(profile.EnableNicoJK);
        Assert.True(profile.NicoJKLog);
        Assert.Equal(new[] { true, false, true, false }, profile.NicoJKFormats);
        Assert.Null(typeof(ProfileSetting).GetProperty("NicoJK18"));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void 旧JSONの削除済み設定を無視してコメント設定を保持する(bool oldValue)
    {
        var json = $"{{\"Name\":\"旧プロファイル\",\"EnableNicoJK\":true,\"NicoJK18\":{oldValue.ToString().ToLowerInvariant()},\"NicoJKLog\":true}}";
        var profile = ServerSupport.NormalizeProfile(JsonSerializer.Deserialize<ProfileSetting>(json)!);

        Assert.Equal("旧プロファイル", profile.Name);
        Assert.True(profile.EnableNicoJK);
        Assert.True(profile.NicoJKLog);
        Assert.DoesNotContain("NicoJK18", JsonSerializer.Serialize(profile));
    }
}

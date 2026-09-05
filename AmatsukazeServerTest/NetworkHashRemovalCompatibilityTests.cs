using System.Runtime.Serialization;
using System.Text.Json;
using System.Xml.Linq;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class NetworkHashRemovalCompatibilityTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void 旧プロファイルのDisableHashCheckを無視して読み込める(bool oldValue)
    {
        var document = SerializeToDocument(new ProfileSetting
        {
            Name = "旧プロファイル"
        });
        var disableChapter = Assert.Single(document.Descendants(),
            element => element.Name.LocalName == "DisableChapter");
        disableChapter.AddAfterSelf(new XElement(
            disableChapter.Name.Namespace + "DisableHashCheck", oldValue));

        var profile = DeserializeFromDocument<ProfileSetting>(document);
        var normalized = ServerSupport.NormalizeProfile(profile);

        Assert.Equal("旧プロファイル", normalized.Name);
        Assert.Contains("DisableHashCheck", SerializeToDocument(normalized).ToString());
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void 旧JSONプロファイルのDisableHashCheckを無視して読み込める(bool oldValue)
    {
        var json = $"{{\"Name\":\"旧JSONプロファイル\",\"DisableHashCheck\":{oldValue.ToString().ToLowerInvariant()}}}";

        var profile = JsonSerializer.Deserialize<ProfileSetting>(json);

        Assert.NotNull(profile);
        Assert.Equal("旧JSONプロファイル", profile.Name);
    }

    [Fact]
    public void 旧キューのHashを無視して読み込める()
    {
        var document = SerializeToDocument(new List<QueueItem>
        {
            new QueueItem
            {
                Id = 17,
                SrcPath = @"\\server\share\input.ts",
                DstPath = @"D:\output\input",
                State = QueueState.Queue
            }
        });
        var genre = Assert.Single(document.Descendants(),
            element => element.Name.LocalName == "Genre");
        genre.AddAfterSelf(new XElement(genre.Name.Namespace + "Hash", "AQIDBA=="));

        var queue = DeserializeFromDocument<List<QueueItem>>(document);

        var item = Assert.Single(queue);
        Assert.Equal(17, item.Id);
        Assert.Equal(@"\\server\share\input.ts", item.SrcPath);
        Assert.Equal(QueueState.Queue, item.State);
        Assert.DoesNotContain("<Hash", SerializeToDocument(queue).ToString());
    }

    [Fact]
    public void 旧追加要求のHashを無視して読み込める()
    {
        var document = SerializeToDocument(new AddQueueItem
        {
            Path = @"\\server\share\input.ts"
        });
        var path = Assert.Single(document.Descendants(),
            element => element.Name.LocalName == "Path");
        path.AddBeforeSelf(new XElement(path.Name.Namespace + "Hash", "AQIDBA=="));

        var item = DeserializeFromDocument<AddQueueItem>(document);

        Assert.Equal(@"\\server\share\input.ts", item.Path);
    }

    [Fact]
    public void 旧REST追加要求のHashを無視して読み込める()
    {
        const string json = "{\"Path\":\"\\\\\\\\server\\\\share\\\\input.ts\",\"Hash\":\"AQIDBA==\"}";

        var item = JsonSerializer.Deserialize<Amatsukaze.Shared.AddQueueItem>(json);

        Assert.NotNull(item);
        Assert.Equal(@"\\server\share\input.ts", item.Path);
    }

    private static XDocument SerializeToDocument<T>(T value)
    {
        using var stream = new MemoryStream();
        new DataContractSerializer(typeof(T)).WriteObject(stream, value);
        stream.Position = 0;
        return XDocument.Load(stream);
    }

    private static T DeserializeFromDocument<T>(XDocument document)
    {
        using var stream = new MemoryStream();
        document.Save(stream);
        stream.Position = 0;
        return (T)new DataContractSerializer(typeof(T)).ReadObject(stream)!;
    }
}

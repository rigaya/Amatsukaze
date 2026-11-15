using System.Text.Json;
using Xunit;

namespace AmatsukazeServerTest;

public class FilterPreviewEndpointTests
{
    [Theory]
    [InlineData("extensionData")]
    [InlineData("ExtensionData")]
    public async Task 設定のXML拡張情報を含むJSONを読み込める(string key)
    {
        await using var fixture = new RestEndpointTestFixture();
        var body = JsonSerializer.Serialize(new
        {
            gen = 1, filterOption = "None",
            filterSetting = new Dictionary<string, object> { [key] = new { }, ["enableDeinterlace"] = true },
            encoderFilterSetting = new Dictionary<string, object> { [key] = new { } },
        });
        var result = await fixture.Send("PUT", "/api/filter-preview/sessions/{sessionId}/config", body: body,
            routeValues: new { sessionId = "存在しないセッション" });
        // JSONの読込みを完了してセッション検索へ進むため、JSON例外ではなく404になる。
        Assert.Equal(404, result.Status);
    }
}

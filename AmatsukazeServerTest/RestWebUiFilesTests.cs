using System.IO.Compression;
using System.Net;
using System.Text;
using Amatsukaze.Server.Rest;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class RestWebUiFilesTests
{
    [Theory]
    [InlineData("/js/filterPreview.js", "js/filterPreview.js", null)]
    [InlineData("/js/filterPreview.js?v=2", "js/filterPreview.js", null)]
    [InlineData("/index.html", "index.html", null)]
    [InlineData("/", "index.html", null)]
    [InlineData("/filter-adjust/123", "index.html", null)]
    [InlineData("/_framework/blazor.webassembly.js", "_framework/blazor.webassembly.js", null)]
    [InlineData("/_framework/blazor.webassembly.js", "_framework/blazor.webassembly.js", "br")]
    [InlineData("/_framework/blazor.webassembly.js", "_framework/blazor.webassembly.js", "gzip")]
    [InlineData("/_framework/dotnet.abc123.wasm", "_framework/dotnet.abc123.wasm", "br")]
    public async Task 静的ファイルを再検証し同じ内容には304を返す(string url, string name, string? encoding)
    {
        var root = Directory.CreateTempSubdirectory("amatsukaze-webui-");
        try
        {
            var path = Path.Combine(root.FullName, name);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var bytes = Encoding.UTF8.GetBytes("最初の内容");
            await File.WriteAllBytesAsync(path, bytes);
            var storedPath = path;
            if (encoding is not null)
            {
                storedPath += encoding == "gzip" ? ".gz" : ".br";
                await using var file = File.Create(storedPath);
                await using Stream compressed = encoding == "gzip"
                    ? new GZipStream(file, CompressionLevel.Fastest)
                    : new BrotliStream(file, CompressionLevel.Fastest);
                await compressed.WriteAsync(bytes);
            }
            var modified = new DateTime(2024, 4, 1, 0, 0, 0, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(storedPath, modified);

            var builder = WebApplication.CreateSlimBuilder(new WebApplicationOptions
            {
                ContentRootPath = root.FullName,
                WebRootPath = root.FullName,
            });
            builder.WebHost.ConfigureKestrel(options => options.Listen(IPAddress.Loopback, 0));
            builder.Services.Configure<HostOptions>(options => options.ShutdownTimeout = Timeout.InfiniteTimeSpan);
            await using var host = builder.Build();
            RestApiHost.ConfigureWebUiFiles(host);
            await host.StartAsync();
            var address = host.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.Single();
            using var client = new HttpClient(new SocketsHttpHandler { UseProxy = false })
            { BaseAddress = new Uri(address), Timeout = Timeout.InfiniteTimeSpan };

            using var response = await SendAsync();
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            Assert.True(response.Headers.CacheControl?.NoCache);
            var tag = Assert.IsType<System.Net.Http.Headers.EntityTagHeaderValue>(response.Headers.ETag);
            Assert.Equal(modified, response.Content.Headers.LastModified?.UtcDateTime);
            if (encoding is null) Assert.Empty(response.Content.Headers.ContentEncoding);
            else Assert.Equal(encoding, Assert.Single(response.Content.Headers.ContentEncoding));
            Assert.Equal(await File.ReadAllBytesAsync(storedPath), await response.Content.ReadAsByteArrayAsync());

            using var byTag = await SendAsync(request => request.Headers.IfNoneMatch.Add(tag));
            using var byDate = await SendAsync(request => request.Headers.IfModifiedSince = new DateTimeOffset(modified));
            foreach (var revalidated in new[] { byTag, byDate })
            {
                Assert.Equal(HttpStatusCode.NotModified, revalidated.StatusCode);
                Assert.True(revalidated.Headers.CacheControl?.NoCache);
                Assert.Equal(tag, revalidated.Headers.ETag);
                Assert.Empty(await revalidated.Content.ReadAsByteArrayAsync());
            }

            if (url == "/js/filterPreview.js")
            {
                var updated = Encoding.UTF8.GetBytes("更新した内容");
                await File.WriteAllBytesAsync(path, updated);
                File.SetLastWriteTimeUtc(path, modified.AddDays(1));
                using var changed = await SendAsync(request =>
                {
                    request.Headers.IfNoneMatch.Add(tag);
                    request.Headers.IfModifiedSince = new DateTimeOffset(modified);
                });
                Assert.Equal(HttpStatusCode.OK, changed.StatusCode);
                Assert.True(changed.Headers.CacheControl?.NoCache);
                Assert.NotEqual(tag, changed.Headers.ETag);
                Assert.Equal(updated, await changed.Content.ReadAsByteArrayAsync());
            }

            async Task<HttpResponseMessage> SendAsync(Action<HttpRequestMessage>? customize = null)
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, url);
                if (encoding is not null) request.Headers.AcceptEncoding.ParseAdd(encoding);
                customize?.Invoke(request);
                return await client.SendAsync(request);
            }
        }
        finally
        {
            root.Delete(recursive: true);
        }
    }
}

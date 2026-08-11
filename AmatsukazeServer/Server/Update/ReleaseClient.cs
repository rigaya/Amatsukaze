using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Update
{
    internal sealed class ReleaseAssetInfo
    {
        public string Name { get; init; }
        public string BrowserDownloadUrl { get; init; }
        public long Size { get; init; }
        public string Digest { get; init; }
    }

    internal sealed class ReleaseInfo
    {
        public string Repository { get; init; }
        public string TagName { get; init; }
        public string HtmlUrl { get; init; }
        public DateTimeOffset? PublishedAt { get; init; }
        public IReadOnlyList<ReleaseAssetInfo> Assets { get; init; }
        public string RateLimitRemaining { get; init; }
        public string RateLimitReset { get; init; }
    }

    internal sealed class ReleaseClient : IDisposable
    {
        private const int MaxAttempts = 3;
        private const string DefaultApiBaseUrl = "https://api.github.com/";
        private static readonly TimeSpan CacheDuration = TimeSpan.FromMinutes(30);
        private static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(8);
        private readonly object cacheLock = new object();
        private readonly Dictionary<string, (DateTime CachedAtUtc, IReadOnlyList<ReleaseInfo> Releases)> cache =
            new Dictionary<string, (DateTime, IReadOnlyList<ReleaseInfo>)>(StringComparer.OrdinalIgnoreCase);
        private readonly HttpClient client;
        private readonly Uri apiBaseUri;
        private readonly TimeSpan requestTimeout;
        private readonly string diagnosticApiHost;

        public ReleaseClient(string proxy, string apiBaseUrl = null,
            TimeSpan? requestTimeout = null)
        {
            var effectiveBaseUrl = string.IsNullOrWhiteSpace(apiBaseUrl)
                ? DefaultApiBaseUrl : apiBaseUrl;
            diagnosticApiHost = string.IsNullOrWhiteSpace(apiBaseUrl) ? null :
                new Uri(effectiveBaseUrl, UriKind.Absolute).Host;
            apiBaseUri = new Uri(EnsureTrailingSlash(effectiveBaseUrl),
                UriKind.Absolute);
            this.requestTimeout = requestTimeout ?? DefaultRequestTimeout;
            client = UpdateHttpClientFactory.Create(proxy, githubApi: true);
        }

        public void ClearCache()
        {
            lock (cacheLock)
            {
                cache.Clear();
            }
        }

        public async Task<IReadOnlyList<ReleaseInfo>> GetReleasesAsync(string repository, string target,
            ReleaseSelectMode selectMode, UpdateLog log, CancellationToken cancellationToken)
        {
            var listReleases = selectMode == ReleaseSelectMode.LatestWithAsset;
            var cacheKey = repository + (listReleases ? "|recent" : "|latest");
            lock (cacheLock)
            {
                if (cache.TryGetValue(cacheKey, out var cached) &&
                    DateTime.UtcNow - cached.CachedAtUtc < CacheDuration)
                {
                    var first = cached.Releases.FirstOrDefault();
                    log.Write(target, "S03_CONNECT", "OK",
                        ("host", apiBaseUri.Host),
                        ("status", 200),
                        ("elapsed", "0ms"),
                        ("cache", "yes"),
                        ("ratelimit", first?.RateLimitRemaining),
                        ("reset", first?.RateLimitReset));
                    return cached.Releases;
                }
            }

            Exception lastException = null;
            for (var attempt = 1; attempt <= MaxAttempts; attempt++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                IReadOnlyList<IPAddress> addresses;
                try
                {
                    using var dnsTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                    dnsTimeout.CancelAfter(requestTimeout);
                    addresses = await Dns.GetHostAddressesAsync(apiBaseUri.Host, dnsTimeout.Token)
                        .ConfigureAwait(false);
                }
                catch (Exception ex) when (ex is SocketException || ex is OperationCanceledException)
                {
                    lastException = ex;
                    if (attempt < MaxAttempts && !cancellationToken.IsCancellationRequested)
                    {
                        await Task.Delay(TimeSpan.FromMilliseconds(250 * attempt), cancellationToken)
                            .ConfigureAwait(false);
                        continue;
                    }
                    log.Write(target, "S03_CONNECT", "NG",
                        ("code", "DNS_FAILED"),
                        ("type", ex.GetType().Name),
                        ("attempt", attempt + "/" + MaxAttempts));
                    await UpdateDiagnostics.LogDnsFailureAsync(log, target, ex,
                        diagnosticApiHost).ConfigureAwait(false);
                    return null;
                }

                var stopwatch = Stopwatch.StartNew();
                using var request = new HttpRequestMessage(HttpMethod.Get,
                    new Uri(apiBaseUri, "repos/" + repository +
                    (listReleases ? "/releases?per_page=30" : "/releases/latest")));
                try
                {
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                    timeout.CancelAfter(requestTimeout);
                    using var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead,
                        timeout.Token).ConfigureAwait(false);
                    var headers = CollectHeaders(response);
                    var responseBody = await response.Content.ReadAsStringAsync(timeout.Token).ConfigureAwait(false);
                    stopwatch.Stop();
                    var rateRemaining = GetHeader(headers, "X-RateLimit-Remaining");
                    var rateReset = FormatRateLimitReset(GetHeader(headers, "X-RateLimit-Reset"));
                    if (!response.IsSuccessStatusCode)
                    {
                        var retryable = (int)response.StatusCode >= 500 ||
                            response.StatusCode == HttpStatusCode.TooManyRequests;
                        if (retryable && attempt < MaxAttempts)
                        {
                            await Task.Delay(TimeSpan.FromMilliseconds(250 * attempt), cancellationToken)
                                .ConfigureAwait(false);
                            continue;
                        }
                        log.Write(target, "S03_CONNECT", "NG",
                            ("host", apiBaseUri.Host),
                            ("dns", string.Join(",", addresses)),
                            ("status", (int)response.StatusCode),
                            ("elapsed", stopwatch.ElapsedMilliseconds + "ms"),
                            ("ratelimit", rateRemaining),
                            ("reset", rateReset),
                            ("attempt", attempt + "/" + MaxAttempts));
                        if (response.StatusCode == HttpStatusCode.Forbidden ||
                            response.StatusCode == HttpStatusCode.NotFound)
                        {
                            UpdateDiagnostics.LogHttpFailure(log, target, response.StatusCode,
                                headers, responseBody, repository);
                        }
                        return null;
                    }

                    var releases = ParseReleases(repository, responseBody, rateRemaining, rateReset,
                        listReleases);
                    log.Write(target, "S03_CONNECT", "OK",
                        ("host", apiBaseUri.Host),
                        ("dns", string.Join(",", addresses)),
                        ("tls", "ok"),
                        ("status", (int)response.StatusCode),
                        ("elapsed", stopwatch.ElapsedMilliseconds + "ms"),
                        ("ratelimit", rateRemaining),
                        ("reset", rateReset),
                        ("attempt", attempt + "/" + MaxAttempts));
                    lock (cacheLock)
                    {
                        cache[cacheKey] = (DateTime.UtcNow, releases);
                    }
                    return releases;
                }
                catch (OperationCanceledException ex) when (!cancellationToken.IsCancellationRequested)
                {
                    lastException = ex;
                    if (attempt < MaxAttempts)
                    {
                        continue;
                    }
                    log.Write(target, "S03_CONNECT", "NG",
                        ("code", "CONNECT_TIMEOUT"),
                        ("timeout", requestTimeout.TotalSeconds + "s"),
                        ("elapsed", stopwatch.ElapsedMilliseconds + "ms"),
                        ("attempt", attempt + "/" + MaxAttempts));
                    await UpdateDiagnostics.LogConnectionFailureAsync(log, target, ex,
                        diagnosticApiHost).ConfigureAwait(false);
                    return null;
                }
                catch (HttpRequestException ex) when (FindInner<AuthenticationException>(ex) != null)
                {
                    lastException = ex;
                    log.Write(target, "S03_CONNECT", "NG",
                        ("code", "TLS_FAILED"),
                        ("type", ex.GetType().Name),
                        ("msg", ex.Message),
                        ("attempt", attempt + "/" + MaxAttempts));
                    await UpdateDiagnostics.LogTlsFailureAsync(log, target, ex,
                        diagnosticApiHost).ConfigureAwait(false);
                    return null;
                }
                catch (HttpRequestException ex)
                {
                    lastException = ex;
                    if (attempt < MaxAttempts)
                    {
                        continue;
                    }
                    log.Write(target, "S03_CONNECT", "NG",
                        ("code", "CONNECT_FAILED"),
                        ("type", ex.GetType().Name),
                        ("msg", ex.Message),
                        ("attempt", attempt + "/" + MaxAttempts));
                    await UpdateDiagnostics.LogConnectionFailureAsync(log, target, ex,
                        diagnosticApiHost).ConfigureAwait(false);
                    return null;
                }
                catch (JsonException ex)
                {
                    log.Write(target, "S03_CONNECT", "NG",
                        ("code", "INVALID_RESPONSE"),
                        ("type", ex.GetType().Name),
                        ("msg", ex.Message));
                    return null;
                }
            }
            log.Write(target, "S03_CONNECT", "NG",
                ("code", "CONNECT_FAILED"),
                ("type", lastException?.GetType().Name));
            return null;
        }

        private static IReadOnlyList<ReleaseInfo> ParseReleases(string repository, string json,
            string rateRemaining, string rateReset, bool listReleases)
        {
            using var document = JsonDocument.Parse(json);
            if (listReleases)
            {
                if (document.RootElement.ValueKind != JsonValueKind.Array)
                {
                    throw new JsonException("GitHub Releases API の応答が配列ではありません");
                }
                return document.RootElement.EnumerateArray()
                    .Select(root => ParseRelease(repository, root, rateRemaining, rateReset))
                    .ToArray();
            }
            return new[] { ParseRelease(repository, document.RootElement, rateRemaining, rateReset) };
        }

        private static ReleaseInfo ParseRelease(string repository, JsonElement root,
            string rateRemaining, string rateReset)
        {
            var assets = new List<ReleaseAssetInfo>();
            if (root.TryGetProperty("assets", out var assetsElement))
            {
                foreach (var asset in assetsElement.EnumerateArray())
                {
                    assets.Add(new ReleaseAssetInfo
                    {
                        Name = GetString(asset, "name"),
                        BrowserDownloadUrl = GetString(asset, "browser_download_url"),
                        Size = asset.TryGetProperty("size", out var size) && size.TryGetInt64(out var parsedSize)
                            ? parsedSize : -1,
                        Digest = GetString(asset, "digest"),
                    });
                }
            }
            DateTimeOffset? publishedAt = null;
            var publishedText = GetString(root, "published_at");
            if (DateTimeOffset.TryParse(publishedText, CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var parsedPublished))
            {
                publishedAt = parsedPublished;
            }
            return new ReleaseInfo
            {
                Repository = repository,
                TagName = GetString(root, "tag_name"),
                HtmlUrl = GetString(root, "html_url"),
                PublishedAt = publishedAt,
                Assets = assets,
                RateLimitRemaining = rateRemaining,
                RateLimitReset = rateReset,
            };
        }

        private static string GetString(JsonElement element, string property)
        {
            return element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.String
                ? value.GetString() : null;
        }

        private static string EnsureTrailingSlash(string value) =>
            value.EndsWith("/", StringComparison.Ordinal) ? value : value + "/";

        private static Dictionary<string, string> CollectHeaders(HttpResponseMessage response)
        {
            var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var header in response.Headers)
            {
                headers[header.Key] = string.Join(",", header.Value);
            }
            foreach (var header in response.Content.Headers)
            {
                headers[header.Key] = string.Join(",", header.Value);
            }
            return headers;
        }

        private static string GetHeader(IReadOnlyDictionary<string, string> headers, string name)
        {
            return headers.TryGetValue(name, out var value) ? value : "?";
        }

        private static string FormatRateLimitReset(string value)
        {
            if (long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var unixSeconds))
            {
                try
                {
                    return DateTimeOffset.FromUnixTimeSeconds(unixSeconds).ToUniversalTime()
                        .ToString("O", CultureInfo.InvariantCulture);
                }
                catch
                {
                }
            }
            return value;
        }

        private static TException FindInner<TException>(Exception exception) where TException : Exception
        {
            while (exception != null)
            {
                if (exception is TException found)
                {
                    return found;
                }
                exception = exception.InnerException;
            }
            return null;
        }

        public void Dispose()
        {
            client.Dispose();
        }
    }
}

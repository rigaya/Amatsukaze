using System;
using System.Net;
using System.Net.Http;

namespace Amatsukaze.Server.Update
{
    internal static class UpdateHttpClientFactory
    {
        public static HttpClient Create(string proxy, bool githubApi = false)
        {
            var client = new HttpClient(CreateHandler(proxy))
            {
                Timeout = System.Threading.Timeout.InfiniteTimeSpan,
            };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("AmatsukazeServer-Update/1.0");
            if (githubApi)
            {
                client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            }
            return client;
        }

        internal static HttpClientHandler CreateHandler(string proxy)
        {
            var handler = new HttpClientHandler();
            if (!string.IsNullOrWhiteSpace(proxy))
            {
                handler.Proxy = new WebProxy(proxy);
                handler.UseProxy = true;
            }
            return handler;
        }
    }
}

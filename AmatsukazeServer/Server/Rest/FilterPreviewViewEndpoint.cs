using System;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;

namespace Amatsukaze.Server.Rest
{
    internal static class FilterPreviewViewEndpoint
    {
        internal static Task WriteAsync(HttpContext context,
            Func<Func<FilterPreviewEngine.ViewRecord, CancellationToken, Task>, CancellationToken, Task> stream)
        {
            var response = context.Response;
            response.ContentType = "application/octet-stream";
            response.Headers.CacheControl = "no-store, no-transform";
            context.Features.Get<IHttpResponseBodyFeature>()?.DisableBuffering();
            return stream((record, token) => record.WriteAsync(response.Body, token), context.RequestAborted);
        }
    }
}

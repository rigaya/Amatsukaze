using System;

namespace AmatsukazeWebUI.Api
{
    public sealed class ApiBaseAddress
    {
        public ApiBaseAddress(Uri uri)
        {
            Uri = uri ?? throw new ArgumentNullException(nameof(uri));
        }

        public Uri Uri { get; }
    }
}

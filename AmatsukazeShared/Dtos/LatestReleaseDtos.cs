using System;

namespace Amatsukaze.Shared
{
    public sealed class LatestReleaseInfo
    {
        public string Tag { get; set; } = "";
        public string Url { get; set; } = "";
        public DateTime? PublishedAt { get; set; }
    }
}

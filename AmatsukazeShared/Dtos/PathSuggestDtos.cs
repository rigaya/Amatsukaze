using System.Collections.Generic;

namespace Amatsukaze.Shared
{
    public sealed class PathSuggestResponse
    {
        public string Input { get; set; } = "";
        public string BaseDir { get; set; } = "";
        public List<PathCandidate> Dirs { get; set; } = new();
        public List<PathCandidate> Files { get; set; } = new();
        public bool HasMoreDirs { get; set; }
        public bool HasMoreFiles { get; set; }
        public int NextDirOffset { get; set; }
        public int NextFileOffset { get; set; }
    }

    public sealed class PathSuggestRequest
    {
        public string Input { get; set; } = "";
        public string? Extensions { get; set; }
        public int MaxDirs { get; set; } = 10;
        public int MaxFiles { get; set; } = 20;
        public bool AllowFiles { get; set; } = true;
        public bool AllowDirs { get; set; } = true;
        public bool CheckAccess { get; set; } = false;
        public int DirOffset { get; set; }
        public int FileOffset { get; set; }
    }

    public sealed class PathCandidate
    {
        public string Name { get; set; } = "";
        public string FullPath { get; set; } = "";
        public bool StartsWith { get; set; }
        public int MatchIndex { get; set; }
    }
}

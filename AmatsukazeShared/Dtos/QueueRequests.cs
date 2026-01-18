using System.Collections.Generic;

namespace Amatsukaze.Shared
{
    public enum ProcMode
    {
        Batch,
        AutoBatch,
        Test,
        DrcsCheck,
        CMCheck
    }

    public class AddQueueItem
    {
        public string? Path { get; set; }
        public byte[]? Hash { get; set; }
    }

    public class OutputInfo
    {
        public string? DstPath { get; set; }
        public string? Profile { get; set; }
        public int Priority { get; set; }
    }

    public class AddQueueRequest
    {
        public string? DirPath { get; set; }
        public List<AddQueueItem>? Targets { get; set; }
        public ProcMode Mode { get; set; }
        public List<OutputInfo>? Outputs { get; set; }
        public string? RequestId { get; set; }
        public string? AddQueueBat { get; set; }
    }

    public enum ChangeItemType
    {
        ResetState,
        UpdateProfile,
        Duplicate,
        Cancel,
        Priority,
        Profile,
        RemoveItem,
        RemoveCompleted,
        ForceStart,
        RemoveSourceFile,
        Move
    }

    public class ChangeItemData
    {
        public int ItemId { get; set; }
        public int WorkerId { get; set; }
        public ChangeItemType ChangeType { get; set; }
        public int Priority { get; set; }
        public string? Profile { get; set; }
        public int Position { get; set; }
        public string? RequestId { get; set; }
    }

    public class PauseRequest
    {
        public bool IsQueue { get; set; }
        public int Index { get; set; }
        public bool Pause { get; set; }
    }
}

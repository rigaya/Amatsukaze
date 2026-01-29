using System.Collections.Generic;

namespace Amatsukaze.Shared
{
    public class QueueMoveManyRequest
    {
        public List<int> ItemIds { get; set; } = new List<int>();
        public int DropIndex { get; set; }
        public string? RequestId { get; set; }
    }
}

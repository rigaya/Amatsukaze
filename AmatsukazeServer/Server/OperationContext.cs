using System;
using System.Threading;

namespace Amatsukaze.Server
{
    public sealed class OperationContext
    {
        public string RequestId { get; set; }
        public string Page { get; set; }
        public string Action { get; set; }
        public string Source { get; set; }
    }

    public static class OperationContextScope
    {
        private static readonly AsyncLocal<OperationContext> current = new AsyncLocal<OperationContext>();

        public static OperationContext Current => current.Value;

        public static IDisposable Use(OperationContext context)
        {
            var prev = current.Value;
            current.Value = context;
            return new ResetScope(() => current.Value = prev);
        }

        private sealed class ResetScope : IDisposable
        {
            private Action reset;

            public ResetScope(Action reset)
            {
                this.reset = reset;
            }

            public void Dispose()
            {
                if (reset != null)
                {
                    reset();
                    reset = null;
                }
            }
        }
    }
}

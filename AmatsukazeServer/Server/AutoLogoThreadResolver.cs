using System;

namespace Amatsukaze.Server
{
    internal static class AutoLogoThreadResolver
    {
        public static int Resolve(int configured)
        {
            if (configured > 0)
            {
                return configured;
            }

            var logical = Math.Max(1, Environment.ProcessorCount);
            return Math.Clamp(logical - 4, 1, 16);
        }
    }
}

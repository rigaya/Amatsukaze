using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Principal;
using Amatsukaze.Lib;

namespace Amatsukaze.Server
{
    public static partial class ServerSupport
    {
        public static string GetAccountType()
        {
            if (!OperatingSystem.IsWindows()) return "通常ユーザー";
            using var identity = WindowsIdentity.GetCurrent();
            var sid = identity.User;
            return sid.IsWellKnown(WellKnownSidType.LocalSystemSid) ? "LocalSystem" :
                sid.IsWellKnown(WellKnownSidType.LocalServiceSid) ? "LocalService" :
                sid.IsWellKnown(WellKnownSidType.NetworkServiceSid) ? "NetworkService" : "通常ユーザー";
        }

        public static bool IsServiceAccount() => OperatingSystem.IsWindows() && GetAccountType() != "通常ユーザー";

    }
}

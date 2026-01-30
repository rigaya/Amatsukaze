using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Amatsukaze.Lib
{
    public class DiskUsageInfo
    {
        public string Path { get; set; }
        public long CapacityBytes { get; set; }
        public long FreeBytes { get; set; }
    }

    public static class DiskUtility
    {
        private static readonly HashSet<string> LinuxExcludeFsTypes = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "overlay",
            "squashfs", "autofs", "mqueue", "hugetlbfs", "tracefs", "ramfs", "pstore",
            "securityfs", "fusectl", "swap"
        };

        private static readonly string[] LinuxExcludeMountPrefixes = new[]
        {
            "/boot",
            "/boot/efi",
            "/run",
            "/var/run",
            "/dev",
            "/proc",
            "/sys"
        };

        public static List<DiskUsageInfo> GetMainDisks()
        {
            if (Amatsukaze.Server.Util.IsServerWindows())
            {
                return GetWindowsDisks();
            }
            if (Amatsukaze.Server.Util.IsServerLinux())
            {
                return GetLinuxDisks();
            }
            return new List<DiskUsageInfo>();
        }

        private static List<DiskUsageInfo> GetWindowsDisks()
        {
            var list = new List<DiskUsageInfo>();
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (!drive.IsReady)
                {
                    continue;
                }
                if (drive.DriveType != DriveType.Fixed && drive.DriveType != DriveType.Network)
                {
                    continue;
                }
                if (drive.TotalSize <= 0)
                {
                    continue;
                }
                list.Add(new DiskUsageInfo
                {
                    Path = drive.RootDirectory.FullName,
                    CapacityBytes = drive.TotalSize,
                    FreeBytes = drive.AvailableFreeSpace
                });
            }
            return list.OrderBy(item => item.Path, StringComparer.OrdinalIgnoreCase).ToList();
        }

        private static List<DiskUsageInfo> GetLinuxDisks()
        {
            var list = new List<DiskUsageInfo>();
            var mountPath = File.Exists("/proc/self/mounts") ? "/proc/self/mounts" : "/proc/mounts";
            if (File.Exists(mountPath) == false)
            {
                return list;
            }

            var mountPoints = new HashSet<string>(StringComparer.Ordinal);
            foreach (var line in File.ReadLines(mountPath))
            {
                var parts = line.Split(' ');
                if (parts.Length < 3)
                {
                    continue;
                }
                var device = DecodeMountField(parts[0]);
                var mountPoint = DecodeMountField(parts[1]);
                var fsType = DecodeMountField(parts[2]);

                if (string.IsNullOrEmpty(mountPoint))
                {
                    continue;
                }
                if (LinuxExcludeFsTypes.Contains(fsType))
                {
                    continue;
                }
                if (device.StartsWith("/dev/loop", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                if (LinuxExcludeMountPrefixes.Any(prefix => mountPoint.Equals(prefix, StringComparison.Ordinal) ||
                    mountPoint.StartsWith(prefix + "/", StringComparison.Ordinal)))
                {
                    continue;
                }
                if (mountPoints.Add(mountPoint) == false)
                {
                    continue;
                }

                DriveInfo driveInfo;
                try
                {
                    driveInfo = new DriveInfo(mountPoint);
                }
                catch
                {
                    continue;
                }
                if (!driveInfo.IsReady || driveInfo.TotalSize <= 0)
                {
                    continue;
                }
                list.Add(new DiskUsageInfo
                {
                    Path = mountPoint,
                    CapacityBytes = driveInfo.TotalSize,
                    FreeBytes = driveInfo.AvailableFreeSpace
                });
            }

            return list.OrderBy(item => item.Path, StringComparer.Ordinal).ToList();
        }

        private static string DecodeMountField(string value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return value;
            }
            return value.Replace("\\040", " ")
                .Replace("\\011", "\t")
                .Replace("\\012", "\n")
                .Replace("\\134", "\\");
        }
    }
}

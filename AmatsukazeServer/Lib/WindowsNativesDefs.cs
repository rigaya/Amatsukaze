using System;
using System.Runtime.InteropServices;

namespace Amatsukaze.Lib
{

    [Flags]
    public enum FileDesiredAccess : uint
    {
        GenericRead = 0x80000000,
        GenericWrite = 0x40000000,
        GenericExecute = 0x20000000,
        GenericAll = 0x10000000
    }

    [Flags]
    public enum FileShareMode : uint
    {
        Zero = 0x00000000,
        FileShareDelete = 0x00000004,
        FileShareRead = 0x00000001,
        FileShareWrite = 0x00000002
    }

    public enum FileCreationDisposition : uint
    {
        CreateNew = 1,
        CreateAlways = 2,
        OpenExisting = 3,
        OpenAlways = 4,
        TruncateExisting = 5
    }

    [Serializable]
    [StructLayout(LayoutKind.Sequential)]
    public struct WINDOWPLACEMENT
    {
        public int length;
        public int flags;
        public SW showCmd;
        public POINT minPosition;
        public POINT maxPosition;
        public RECT normalPosition;
    }

    [Serializable]
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT
    {
        public int X;
        public int Y;

        public POINT(int x, int y)
        {
            this.X = x;
            this.Y = y;
        }
    }

    [Serializable]
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;

        public RECT(int left, int top, int right, int bottom)
        {
            this.Left = left;
            this.Top = top;
            this.Right = right;
            this.Bottom = bottom;
        }
    }

    public enum SW
    {
        HIDE = 0,
        SHOWNORMAL = 1,
        SHOWMINIMIZED = 2,
        SHOWMAXIMIZED = 3,
        SHOWNOACTIVATE = 4,
        SHOW = 5,
        MINIMIZE = 6,
        SHOWMINNOACTIVE = 7,
        SHOWNA = 8,
        RESTORE = 9,
        SHOWDEFAULT = 10,
    }
}

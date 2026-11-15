using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Amatsukaze.Lib
{
    [StructLayout(LayoutKind.Sequential)]
    public struct FilterPreviewSourceFormat
    {
        public int Progressive, SarWidth, SarHeight, ColorPrimaries, TransferCharacteristics, ColorSpace;
    }

    public static class FilterPreviewSourceReader
    {
        [DllImport(AmatsukazeNatives.AmatsukazeLibName, CharSet = AmatsukazeNatives.AmatsukazeLibCharSet)]
        private static extern int FilterPreview_GetSourceFormat(IntPtr context, string path, out FilterPreviewSourceFormat format);

        public static FilterPreviewSourceFormat Read(string path)
        {
            using var context = new AMTContext();
            if (FilterPreview_GetSourceFormat(context.Ptr, path, out var format) == 0)
                throw new IOException(context.GetError());
            return format;
        }
    }
}

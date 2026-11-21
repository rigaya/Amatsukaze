using Xunit;

namespace AmatsukazeServerTest;

internal sealed class LinuxFactAttribute : FactAttribute
{
    public LinuxFactAttribute()
    {
        if (!OperatingSystem.IsLinux()) Skip = "Linuxのシェルとプロセス動作を対象とする試験です。";
    }
}

internal sealed class LinuxTheoryAttribute : TheoryAttribute
{
    public LinuxTheoryAttribute()
    {
        if (!OperatingSystem.IsLinux()) Skip = "Linuxのシェルとプロセス動作を対象とする試験です。";
    }
}

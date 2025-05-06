@echo off
echo Building solution with MSBuild...
msbuild Amatsukaze.sln /p:Configuration=Release /p:Platform=x64

echo Publishing .NET project AmatsukazeAddTask...
dotnet publish AmatsukazeAddTask/AmatsukazeAddTask.csproj -r win-x64 -c Release --self-contained=true -o ./publish/win-x64

echo Publishing .NET project AmatsukazeServerCLI...
dotnet publish AmatsukazeServerCLI/AmatsukazeServerCLI.csproj -r win-x64 -c Release --self-contained=true -o ./publish/win-x64

echo Publishing .NET project AmatsukazeGUI...
dotnet publish AmatsukazeGUI/AmatsukazeGUI.csproj -r win-x64 -c Release --self-contained=true -o ./publish/win-x64

pause
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'test-7zip.conf') -Raw | ConvertFrom-StringData
$archiveName = "$($pin.SEVENZIP_PKG)-extra.7z"
$url = "https://github.com/ip7z/7zip/releases/download/$($pin.SEVENZIP_VER)/$archiveName"
$runId = "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$results = Join-Path $repoRoot "build/csharp-test-results/$runId"
$work = Join-Path ([IO.Path]::GetTempPath()) "amatsukaze-csharp-$runId"
$tar = Join-Path ([Environment]::SystemDirectory) 'tar.exe'
$previousExtractor = $env:AMT_TEST_7ZIP
New-Item -ItemType Directory -Path $work | Out-Null

try {
    $archive = Join-Path $work $archiveName
    Write-Host "試験用の7-Zipを取得します $url"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
        $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($hash -ne $pin.SEVENZIP_WINDOWS_SHA256) {
            throw "SHA-256が一致しません。期待値 $($pin.SEVENZIP_WINDOWS_SHA256)、実測値 $hash"
        }

        # 照合済みアーカイブのx64版だけを展開する。
        & $tar -xf $archive -C $work 'x64/7za.exe'
        $extractor = Join-Path $work 'x64/7za.exe'
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $extractor -PathType Leaf)) {
            throw "x64/7za.exeを展開できません。tarの終了コード $LASTEXITCODE"
        }
    }
    catch {
        throw "試験用の展開器の準備に失敗しました。取得元 $url、配置先 $work。$($_.Exception.Message)"
    }

    $env:AMT_TEST_7ZIP = $extractor
    Write-Host "試験結果の保存先 $results"
    $project = Join-Path $repoRoot 'AmatsukazeServerTest/AmatsukazeServerTest.csproj'
    & dotnet test $project -c $Configuration --filter 'Category!=Network' --blame-hang-timeout 5m `
        --results-directory $results --logger 'trx;LogFileName=offline.trx'
    exit $LASTEXITCODE
}
finally {
    $env:AMT_TEST_7ZIP = $previousExtractor
    if (Test-Path -LiteralPath $work) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction Continue
    }
}

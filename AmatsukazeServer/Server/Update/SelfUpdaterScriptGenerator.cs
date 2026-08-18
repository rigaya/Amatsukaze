using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Amatsukaze.Server.Update
{
    // updater の待機条件。失敗注入は使い捨て環境でロールバックを検証するためにだけ使う。
    internal sealed record SelfUpdaterScriptOptions(int WaitTimeoutSeconds = 120,
        int PollIntervalSeconds = 1, int FailAfterPlacedItems = 0);

    // 生成したスクリプトと、サーバー側が P4c で監視するファイルのパス。
    internal sealed record GeneratedSelfUpdater(string ScriptPath, string LogPath,
        string ReadyPath, string ResultPath, string BackupDirectory);

    // サーバー終了後に単独で動作する本体 updater スクリプトを生成する。
    internal static class SelfUpdaterScriptGenerator
    {
        private const int RobocopySuccessMax = 7;
        private const int RobocopyFailureMin = RobocopySuccessMax + 1;
        // 起動直後の依存ライブラリ不足や abort を検出するための生存確認時間。
        private const int RestartProbeSeconds = 1;
        private const string TimestampFormat = "yyyyMMdd-HHmmss";
        private static readonly Regex TransactionIdRegex = new Regex("^[0-9a-f]{8}$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

        public static GeneratedSelfUpdater Generate(string appRoot, long serverPid,
            PreparedSelfUpdate prepared, string transactionId, UpdateOSKind os,
            DateTime generatedAtUtc, SelfUpdaterScriptOptions options = null)
        {
            ArgumentNullException.ThrowIfNull(prepared);
            options ??= new SelfUpdaterScriptOptions();
            ValidateInputs(appRoot, serverPid, prepared, transactionId, options);
            var fullAppRoot = Path.GetFullPath(appRoot);
            var workRoot = SelfUpdateWorkspace.ValidateChild(fullAppRoot,
                Path.Combine(fullAppRoot, ".amatsukaze_update"));
            var staging = SelfUpdateWorkspace.ValidateChild(workRoot,
                prepared.StagingDirectory);
            var timestamp = generatedAtUtc.ToUniversalTime().ToString(TimestampFormat,
                CultureInfo.InvariantCulture);
            var scriptPath = SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, os == UpdateOSKind.Windows ? "updater.bat" : "updater.sh"));
            var logPath = SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, "update_" + timestamp + ".log"));
            var readyPath = SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, "updater_ready"));
            var resultPath = SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, "result.txt"));
            var backupDirectory = SelfUpdateWorkspace.ValidateChild(workRoot,
                Path.Combine(workRoot, "backup", timestamp));
            SelfUpdatePolicy.ValidateTopLevel(staging, os);
            var plan = BuildPlan(staging, os);
            var content = os == UpdateOSKind.Windows
                ? BuildBatch(fullAppRoot, serverPid, prepared, transactionId, logPath,
                    readyPath, resultPath, backupDirectory, plan, options)
                : BuildShell(fullAppRoot, serverPid, prepared, transactionId, logPath,
                    readyPath, resultPath, backupDirectory, plan, options);
            WriteScript(scriptPath, content, os);
            return new GeneratedSelfUpdater(scriptPath, logPath, readyPath, resultPath,
                backupDirectory);
        }

        private static UpdatePlan BuildPlan(string staging, UpdateOSKind os)
        {
            var overwrite = new List<string>();
            var addOnly = new List<string>();
            var replace = new HashSet<string>(StringComparer.Ordinal);
            foreach (var path in Directory.EnumerateFiles(staging, "*", SearchOption.AllDirectories))
            {
                var relative = Path.GetRelativePath(staging, path).Replace('\\', '/');
                switch (SelfUpdatePolicy.GetAction(relative, os))
                {
                    case SelfUpdatePathAction.OverwriteCopy:
                        overwrite.Add(relative);
                        break;
                    case SelfUpdatePathAction.AddOnly:
                        addOnly.Add(relative);
                        break;
                    case SelfUpdatePathAction.ReplaceDirectory:
                        replace.Add("exe_files/wwwroot");
                        break;
                    case SelfUpdatePathAction.Ignore:
                        break;
                    default:
                        throw new UpdatePreparationException("UNEXPECTED_ARCHIVE_LAYOUT",
                            "S09_STAGE", "updater の許可リスト外パスです: " + relative);
                }
            }
            overwrite.Sort(StringComparer.Ordinal);
            addOnly.Sort(StringComparer.Ordinal);
            return new UpdatePlan(overwrite, addOnly,
                replace.OrderBy(item => item, StringComparer.Ordinal).ToArray());
        }

        private static string BuildShell(string appRoot, long serverPid,
            PreparedSelfUpdate prepared, string transactionId, string logPath, string readyPath,
            string resultPath, string backupDirectory, UpdatePlan plan,
            SelfUpdaterScriptOptions options)
        {
            var builder = new StringBuilder();
            void L(string line = "") => builder.Append(line).Append('\n');
            L("#!/usr/bin/env bash");
            L("# Amatsukaze 本体更新用スクリプト。サーバーが起動できない場合は引数なしで手動実行できます。");
            L("set -u");
            L("DEFAULT_SERVER_PID=" + serverPid.ToString(CultureInfo.InvariantCulture));
            L("DEFAULT_STAGING=" + QuoteShell(prepared.StagingDirectory));
            L("DEFAULT_APP_ROOT=" + QuoteShell(appRoot));
            L("DEFAULT_RESTART_EXE=" + QuoteShell(prepared.RestartCommandLine.ProcessPath));
            L("DEFAULT_LOG_PATH=" + QuoteShell(logPath));
            L("DEFAULT_TXID=" + QuoteShell(transactionId));
            L("DEFAULT_RESTART_ARGS=(" + string.Join(" ", prepared.RestartCommandLine.Arguments
                .Select(QuoteShell)) + ")");
            L("WAIT_TIMEOUT_SECONDS=" + options.WaitTimeoutSeconds.ToString(
                CultureInfo.InvariantCulture));
            L("POLL_INTERVAL_SECONDS=" + options.PollIntervalSeconds.ToString(
                CultureInfo.InvariantCulture));
            L("RESTART_PROBE_SECONDS=" + RestartProbeSeconds.ToString(
                CultureInfo.InvariantCulture));
            L("FAIL_AFTER_PLACED_ITEMS=" + options.FailAfterPlacedItems.ToString(
                CultureInfo.InvariantCulture));
            L("SERVER_PID=\"${1:-$DEFAULT_SERVER_PID}\"");
            L("STAGING=\"${2:-$DEFAULT_STAGING}\"");
            L("APP_ROOT=\"${3:-$DEFAULT_APP_ROOT}\"");
            L("RESTART_EXE=\"${4:-$DEFAULT_RESTART_EXE}\"");
            L("LOG_PATH=\"${5:-$DEFAULT_LOG_PATH}\"");
            L("TXID=\"${6:-$DEFAULT_TXID}\"");
            L("RESTART_ARGS=(\"${DEFAULT_RESTART_ARGS[@]}\")");
            L("if (( $# >= 7 )); then shift 6; RESTART_ARGS=(\"$@\"); fi");
            L("WORK_ROOT=\"$APP_ROOT/.amatsukaze_update\"");
            L("READY_PATH=\"$WORK_ROOT/updater_ready\"");
            L("RESULT_PATH=\"$WORK_ROOT/result.txt\"");
            L("BACKUP_NAME=" + QuoteShell(Path.GetFileName(backupDirectory)));
            L("BACKUP_DIR=\"$WORK_ROOT/backup/$BACKUP_NAME\"");
            L("CREATED_LIST=\"$BACKUP_DIR/created_paths.txt\"");
            L("WWWROOT_NEW=\"$APP_ROOT/exe_files/wwwroot.new.$TXID\"");
            L("WWWROOT_OLD=\"$APP_ROOT/exe_files/wwwroot.old.$TXID\"");
            L("STARTED=\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\"");
            L("PLACED_ITEMS=0");
            L("STATUS=success");
            L("ERROR_CODE=");
            L();
            L("quote_value() {");
            L("  local value=${1//\\/\\\\}");
            L("""  value=${value//\"/\\\"}""");
            L("  printf '\"%s\"' \"$value\"");
            L("}");
            L("log_update() {");
            L("  local stage=$1 result=$2; shift 2");
            L("  printf '[Update][%s][main][%s] %s%s\\n' \"$TXID\" \"$stage\" \"$result\" \"${*:+ $*}\" >> \"$LOG_PATH\"");
            L("}");
            L("write_result() {");
            L("  local finished tmp=\"$RESULT_PATH.tmp\"");
            L("  finished=\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\"");
            L("  { printf 'txid=%s\\n' \"$TXID\"; printf 'status=%s\\n' \"$STATUS\";");
            L("    printf 'version=%s\\n' " + QuoteShell(prepared.Version) + ";");
            L("    printf 'started=%s\\n' \"$STARTED\"; printf 'finished=%s\\n' \"$finished\";");
            L("    printf 'error_code=%s\\n' \"$ERROR_CODE\"; printf 'log=%s\\n' \"$LOG_PATH\"; } > \"$tmp\"");
            L("  mv -f -- \"$tmp\" \"$RESULT_PATH\"");
            L("  log_update S29_RESULT \"$([ \"$STATUS\" = success ] && printf OK || printf NG)\" \"status=$STATUS error_code=${ERROR_CODE:-none}\"");
            L("}");
            L("server_running() {");
            L("  case \"$SERVER_PID\" in ''|*[!0-9]*) ;; *) kill -0 \"$SERVER_PID\" 2>/dev/null && return 0 ;; esac");
            L("  command -v pgrep >/dev/null 2>&1 || return 1");
            L("  pgrep -f '(^|/)([A]matsukazeServerCLI|[A]matsukazeGUI|[A]matsukazeCLI)([[:space:]]|$)' >/dev/null 2>&1");
            L("}");
            L("spawn_server() {");
            L("  if [ -z \"$RESTART_EXE\" ]; then log_update S24_RESTART NG 'code=RESTART_COMMAND_MISSING'; return 1; fi");
            L("  if [ ! -x \"$RESTART_EXE\" ]; then log_update S24_RESTART NG \"code=RESTART_FAILED reason=not_executable process=$(quote_value \"$RESTART_EXE\")\"; return 1; fi");
            L("  nohup \"$RESTART_EXE\" \"${RESTART_ARGS[@]}\" >/dev/null 2>&1 &");
            L("  restart_pid=$!");
            L("  log_update S24_RESTART OK \"process=$(quote_value \"$RESTART_EXE\") pid=$restart_pid\"; return 0");
            L("}");
            L("probe_server() {");
            L("  sleep \"$RESTART_PROBE_SECONDS\"");
            L("  if ! kill -0 \"$restart_pid\" 2>/dev/null; then log_update S24_RESTART NG \"code=RESTART_FAILED reason=exited_immediately process=$(quote_value \"$RESTART_EXE\")\"; return 1; fi");
            // 新サーバーが結果取込後にログを削除している可能性があるため、probe 成功時は追記しない。
            L("  return 0");
            L("}");
            L("backup_file() {");
            L("  local rel=$1 source=\"$APP_ROOT/$1\" destination=\"$BACKUP_DIR/$1\"");
            L("  [ -e \"$source\" ] || [ -L \"$source\" ] || return 0");
            L("  [ ! -d \"$source\" ] || [ -L \"$source\" ] || return 1");
            L("  mkdir -p -- \"$(dirname \"$destination\")\" && cp -a -- \"$source\" \"$destination\"");
            L("}");
            L("remember_addition() {");
            L("  local rel=$1 destination=\"$APP_ROOT/$1\"");
            L("  if [ ! -e \"$destination\" ] && [ ! -L \"$destination\" ]; then printf '%s\\n' \"$rel\" >> \"$CREATED_LIST\"; fi");
            L("}");
            L("place_file() {");
            L("  local rel=$1 source=\"$STAGING/$1\" destination=\"$APP_ROOT/$1\"");
            L("  mkdir -p -- \"$(dirname \"$destination\")\" || return 1");
            L("  rm -f -- \"$destination\" || return 1");
            L("  cp -a -- \"$source\" \"$destination\" || return 1");
            L("  PLACED_ITEMS=$((PLACED_ITEMS + 1))");
            L("  [ \"$FAIL_AFTER_PLACED_ITEMS\" -le 0 ] || [ \"$PLACED_ITEMS\" -lt \"$FAIL_AFTER_PLACED_ITEMS\" ]");
            L("}");
            L("place_addition() {");
            L("  local rel=$1 source=\"$STAGING/$1\" destination=\"$APP_ROOT/$1\"");
            L("  if [ -e \"$destination\" ] || [ -L \"$destination\" ]; then return 0; fi");
            L("  mkdir -p -- \"$(dirname \"$destination\")\" && cp -a -- \"$source\" \"$destination\" || return 1");
            L("  PLACED_ITEMS=$((PLACED_ITEMS + 1))");
            L("  [ \"$FAIL_AFTER_PLACED_ITEMS\" -le 0 ] || [ \"$PLACED_ITEMS\" -lt \"$FAIL_AFTER_PLACED_ITEMS\" ]");
            L("}");
            L("backup_wwwroot() {");
            L("  local source=\"$APP_ROOT/exe_files/wwwroot\" destination=\"$BACKUP_DIR/exe_files/wwwroot\"");
            L("  [ -e \"$source\" ] || return 0");
            L("  mkdir -p -- \"$(dirname \"$destination\")\" && cp -a -- \"$source\" \"$destination\"");
            L("}");
            L("validate_wwwroot() {");
            L("  [ ! -e \"$WWWROOT_NEW\" ] && [ ! -e \"$WWWROOT_OLD\" ]");
            L("}");
            L("place_wwwroot() {");
            L("  local source=\"$STAGING/exe_files/wwwroot\" destination=\"$APP_ROOT/exe_files/wwwroot\"");
            L("  [ -d \"$source\" ] || return 0");
            L("  [ ! -e \"$WWWROOT_NEW\" ] && [ ! -e \"$WWWROOT_OLD\" ] || return 1");
            L("  cp -a -- \"$source\" \"$WWWROOT_NEW\" || return 1");
            L("  if [ -e \"$destination\" ]; then mv -- \"$destination\" \"$WWWROOT_OLD\" || return 1; fi");
            L("  mv -- \"$WWWROOT_NEW\" \"$destination\" || return 1");
            L("  PLACED_ITEMS=$((PLACED_ITEMS + 1))");
            L("  [ \"$FAIL_AFTER_PLACED_ITEMS\" -le 0 ] || [ \"$PLACED_ITEMS\" -lt \"$FAIL_AFTER_PLACED_ITEMS\" ]");
            L("}");
            L("rollback_all() {");
            L("  local failed=0 rel destination backup");
            L("  if [ -e \"$WWWROOT_OLD\" ]; then rm -rf -- \"$APP_ROOT/exe_files/wwwroot\"; mv -- \"$WWWROOT_OLD\" \"$APP_ROOT/exe_files/wwwroot\" || failed=1;");
            L("  elif [ -e \"$BACKUP_DIR/exe_files/wwwroot\" ]; then rm -rf -- \"$APP_ROOT/exe_files/wwwroot\"; mkdir -p -- \"$APP_ROOT/exe_files\"; cp -a -- \"$BACKUP_DIR/exe_files/wwwroot\" \"$APP_ROOT/exe_files/wwwroot\" || failed=1;");
            L("  else rm -rf -- \"$APP_ROOT/exe_files/wwwroot\" || failed=1; fi");
            L("  rm -rf -- \"$WWWROOT_NEW\"");
            foreach (var relative in plan.OverwriteFiles)
            {
                L("  rel=" + QuoteShell(relative) + "; destination=\"$APP_ROOT/$rel\"; backup=\"$BACKUP_DIR/$rel\"; rm -f -- \"$destination\"; if [ -e \"$backup\" ] || [ -L \"$backup\" ]; then mkdir -p -- \"$(dirname \"$destination\")\"; cp -a -- \"$backup\" \"$destination\" || failed=1; fi");
            }
            L("  if [ -f \"$CREATED_LIST\" ]; then while IFS= read -r rel; do rm -f -- \"$APP_ROOT/$rel\" || failed=1; done < \"$CREATED_LIST\"; fi");
            L("  [ \"$failed\" -eq 0 ]");
            L("}");
            L();
            L("mkdir -p -- \"$(dirname \"$LOG_PATH\")\"");
            L(": > \"$LOG_PATH\"");
            L("# サーバーへ起動完了を通知してから、対象プロセスがすべて終了するまで待ちます。");
            L("rm -f -- \"$READY_PATH\"");
            L("printf '%s\\n' \"$TXID\" > \"$READY_PATH\"");
            L("log_update S20_WAIT_EXIT OK \"ready=$(quote_value \"$READY_PATH\")\"");
            L("elapsed=0");
            L("while server_running; do");
            L("  if [ \"$elapsed\" -ge \"$WAIT_TIMEOUT_SECONDS\" ]; then STATUS=timeout; ERROR_CODE=RESTART_TIMEOUT; log_update S20_WAIT_EXIT NG \"code=$ERROR_CODE timeout=${WAIT_TIMEOUT_SECONDS}s\"; write_result; exit 2; fi");
            L("  sleep \"$POLL_INTERVAL_SECONDS\"; elapsed=$((elapsed + POLL_INTERVAL_SECONDS))");
            L("done");
            L("log_update S20_WAIT_EXIT OK \"pid=$SERVER_PID elapsed=${elapsed}s\"");
            L("# 既存ファイルへ触る前に、必要な全バックアップを確定します。");
            L("rm -rf -- \"$BACKUP_DIR\" && mkdir -p -- \"$BACKUP_DIR\" || { STATUS=failed; ERROR_CODE=BACKUP_FAILED; log_update S21_BACKUP NG \"code=$ERROR_CODE\"; spawn_server || true; write_result; exit 3; }");
            L(": > \"$CREATED_LIST\"");
            if (plan.ReplaceDirectories.Count > 0)
            {
                L("validate_wwwroot || { STATUS=failed; ERROR_CODE=WWWROOT_RESIDUE; log_update S21_BACKUP NG \"code=$ERROR_CODE\"; spawn_server || true; write_result; exit 3; }");
                L("backup_wwwroot || { STATUS=failed; ERROR_CODE=BACKUP_FAILED; log_update S21_BACKUP NG \"code=$ERROR_CODE path=exe_files/wwwroot\"; spawn_server || true; write_result; exit 3; }");
            }
            foreach (var relative in plan.OverwriteFiles)
                L("backup_file " + QuoteShell(relative) + " || { STATUS=failed; ERROR_CODE=BACKUP_FAILED; log_update S21_BACKUP NG \"code=$ERROR_CODE path=$(quote_value " + QuoteShell(relative) + ")\"; spawn_server || true; write_result; exit 3; }");
            foreach (var relative in plan.AddOnlyFiles)
                L("remember_addition " + QuoteShell(relative) + " || { STATUS=failed; ERROR_CODE=BACKUP_FAILED; log_update S21_BACKUP NG \"code=$ERROR_CODE path=$(quote_value " + QuoteShell(relative) + ")\"; spawn_server || true; write_result; exit 3; }");
            L("log_update S21_BACKUP OK \"path=$(quote_value \"$BACKUP_DIR\") files=" + plan.OverwriteFiles.Count.ToString(CultureInfo.InvariantCulture) + "\"");
            L("# 許可リストから生成した明示的な配置計画だけを適用します。");
            L("placement_failed=0");
            if (plan.ReplaceDirectories.Count > 0) L("place_wwwroot || placement_failed=1");
            foreach (var relative in plan.OverwriteFiles)
                L("[ \"$placement_failed\" -ne 0 ] || place_file " + QuoteShell(relative) + " || placement_failed=1");
            foreach (var relative in plan.AddOnlyFiles)
                L("[ \"$placement_failed\" -ne 0 ] || place_addition " + QuoteShell(relative) + " || placement_failed=1");
            L("if [ \"$placement_failed\" -ne 0 ]; then");
            L("  log_update S22_PLACE NG 'code=PLACE_FAILED'");
            L("  if rollback_all; then STATUS=rolled_back; ERROR_CODE=PLACE_FAILED; log_update S23_ROLLBACK OK 'code=PLACE_FAILED'; else STATUS=failed; ERROR_CODE=ROLLBACK_FAILED; log_update S23_ROLLBACK NG 'code=ROLLBACK_FAILED'; fi");
            L("else");
            L("  rm -rf -- \"$WWWROOT_OLD\" \"$WWWROOT_NEW\"");
            L("  if rm -rf -- \"$STAGING\" \"$BACKUP_DIR\"; then log_update S22_PLACE OK \"items=$PLACED_ITEMS staging=removed backup=removed\"; log_update S23_ROLLBACK SKIP 'reason=placement_succeeded';");
            L("  else STATUS=failed; ERROR_CODE=WORKSPACE_CLEANUP_FAILED; log_update S22_PLACE NG \"code=$ERROR_CODE staging=$(quote_value \"$STAGING\") backup=$(quote_value \"$BACKUP_DIR\")\"; log_update S23_ROLLBACK SKIP 'reason=placement_succeeded'; fi");
            L("fi");
            L("# 新サーバーが起動処理を終える前に、再起動ログと結果ファイルを確定します。");
            L("restart_failed=0");
            L("spawn_server || restart_failed=1");
            L("write_result");
            L("[ \"$restart_failed\" -ne 0 ] || probe_server || restart_failed=1");
            L("if [ \"$restart_failed\" -ne 0 ] && [ \"$STATUS\" = success ]; then");
            L("  STATUS=failed; ERROR_CODE=RESTART_FAILED; write_result");
            L("fi");
            L("[ \"$STATUS\" = success ]");
            return builder.ToString();
        }

        private static string BuildBatch(string appRoot, long serverPid,
            PreparedSelfUpdate prepared, string transactionId, string logPath, string readyPath,
            string resultPath, string backupDirectory, UpdatePlan plan,
            SelfUpdaterScriptOptions options)
        {
            var builder = new StringBuilder();
            void L(string line = "") => builder.Append(line).Append("\r\n");
            L("@echo off");
            L("rem Amatsukaze 本体更新用スクリプト。サーバーが起動できない場合は引数なしで手動実行できます。");
            L("setlocal EnableExtensions DisableDelayedExpansion");
            L("set \"SERVER_PID=" + serverPid.ToString(CultureInfo.InvariantCulture) + "\"");
            L("set \"STAGING=" + QuoteBatchValue(prepared.StagingDirectory) + "\"");
            L("set \"APP_ROOT=" + QuoteBatchValue(appRoot) + "\"");
            L("set \"RESTART_EXE=" + QuoteBatchValue(prepared.RestartCommandLine.ProcessPath) + "\"");
            L("set \"RESTART_ARGS=" + string.Join(" ", prepared.RestartCommandLine.Arguments
                .Select(QuoteBatchArgument)) + "\"");
            L("set \"LOG_PATH=" + QuoteBatchValue(logPath) + "\"");
            L("set \"TXID=" + QuoteBatchValue(transactionId) + "\"");
            L("set \"WAIT_TIMEOUT_SECONDS=" + options.WaitTimeoutSeconds.ToString(CultureInfo.InvariantCulture) + "\"");
            L("set \"POLL_INTERVAL_SECONDS=" + options.PollIntervalSeconds.ToString(CultureInfo.InvariantCulture) + "\"");
            L("set \"FAIL_AFTER_PLACED_ITEMS=" + options.FailAfterPlacedItems.ToString(CultureInfo.InvariantCulture) + "\"");
            L("if not \"%~1\"==\"\" set \"SERVER_PID=%~1\"");
            L("if not \"%~2\"==\"\" set \"STAGING=%~2\"");
            L("if not \"%~3\"==\"\" set \"APP_ROOT=%~3\"");
            L("if not \"%~4\"==\"\" set \"RESTART_EXE=%~4\"");
            L("if not \"%~5\"==\"\" set \"LOG_PATH=%~5\"");
            L("if not \"%~6\"==\"\" set \"TXID=%~6\"");
            L("if \"%~7\"==\"\" goto args_ready");
            L("shift&shift&shift&shift&shift&shift");
            L("set \"RESTART_ARGS=\"");
            L(":collect_restart_args");
            L("if \"%~1\"==\"\" goto args_ready");
            L("set \"RESTART_ARGS=%RESTART_ARGS% \"%~1\"\"");
            L("shift");
            L("goto collect_restart_args");
            L(":args_ready");
            L("set \"WORK_ROOT=%APP_ROOT%\\.amatsukaze_update\"");
            L("set \"READY_PATH=%WORK_ROOT%\\updater_ready\"");
            L("set \"RESULT_PATH=%WORK_ROOT%\\result.txt\"");
            L("set \"BACKUP_NAME=" + QuoteBatchValue(Path.GetFileName(backupDirectory)) + "\"");
            L("set \"BACKUP_DIR=%WORK_ROOT%\\backup\\%BACKUP_NAME%\"");
            L("set \"CREATED_LIST=%BACKUP_DIR%\\created_paths.txt\"");
            L("set \"WWWROOT_NEW=%APP_ROOT%\\exe_files\\wwwroot.new.%TXID%\"");
            L("set \"WWWROOT_OLD=%APP_ROOT%\\exe_files\\wwwroot.old.%TXID%\"");
            L("set \"ROBOCOPY_SUCCESS_MAX=" + RobocopySuccessMax.ToString(CultureInfo.InvariantCulture) + "\"");
            L("set \"ROBOCOPY_FAILURE_MIN=" + RobocopyFailureMin.ToString(CultureInfo.InvariantCulture) + "\"");
            L("set \"STATUS=success\"");
            L("set \"ERROR_CODE=\"");
            L("set \"PLACED_ITEMS=0\"");
            L("for /f %%I in ('powershell -NoProfile -Command \"[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ',[Globalization.CultureInfo]::InvariantCulture)\"') do set \"STARTED=%%I\"");
            L("if not exist \"%WORK_ROOT%\" mkdir \"%WORK_ROOT%\"");
            L(">\"%LOG_PATH%\" rem updater log");
            L("rem サーバーへ起動完了を通知してから、対象プロセスがすべて終了するまで待ちます。");
            L(">\"%READY_PATH%\" echo %TXID%");
            L("call :log S20_WAIT_EXIT OK ready=\"%READY_PATH%\"");
            L("set \"ELAPSED=0\"");
            L(":wait_exit");
            L("call :server_running");
            L("if errorlevel 1 goto wait_complete");
            L("if %ELAPSED% GEQ %WAIT_TIMEOUT_SECONDS% goto wait_timeout");
            L("timeout /t %POLL_INTERVAL_SECONDS% /nobreak >nul");
            L("set /a ELAPSED+=POLL_INTERVAL_SECONDS");
            L("goto wait_exit");
            L(":wait_timeout");
            L("set \"STATUS=timeout\"");
            L("set \"ERROR_CODE=RESTART_TIMEOUT\"");
            L("call :log S20_WAIT_EXIT NG code=RESTART_TIMEOUT timeout=%WAIT_TIMEOUT_SECONDS%s");
            L("call :write_result");
            L("exit /b 2");
            L(":wait_complete");
            L("call :log S20_WAIT_EXIT OK pid=%SERVER_PID% elapsed=%ELAPSED%s");
            L("rem 既存ファイルへ触る前に、必要な全バックアップを確定します。");
            L("if not exist \"%BACKUP_DIR%\" goto backup_directory_ready");
            L("rmdir /s /q \"%BACKUP_DIR%\"");
            L("if errorlevel 1 goto backup_failed");
            L(":backup_directory_ready");
            L("mkdir \"%BACKUP_DIR%\"");
            L("if errorlevel 1 goto backup_failed");
            L(">\"%CREATED_LIST%\" rem created paths");
            if (plan.ReplaceDirectories.Count > 0)
            {
                L("call :validate_wwwroot");
                L("if errorlevel 1 goto wwwroot_residue");
                L("call :backup_wwwroot");
                L("if errorlevel 1 goto backup_failed");
            }
            foreach (var relative in plan.OverwriteFiles)
            {
                L("call :backup_file " + QuoteBatchArgument(ToBatchPath(relative)));
                L("if errorlevel 1 goto backup_failed");
            }
            foreach (var relative in plan.AddOnlyFiles)
            {
                L("call :remember_addition " + QuoteBatchArgument(ToBatchPath(relative)));
                L("if errorlevel 1 goto backup_failed");
            }
            L("call :log S21_BACKUP OK path=\"%BACKUP_DIR%\" files=" + plan.OverwriteFiles.Count.ToString(CultureInfo.InvariantCulture));
            L("rem 許可リストから生成した明示的な配置計画だけを適用します。");
            if (plan.ReplaceDirectories.Count > 0)
            {
                L("call :place_wwwroot");
                L("if errorlevel 1 goto place_failed");
            }
            foreach (var relative in plan.OverwriteFiles)
            {
                L("call :place_file " + QuoteBatchArgument(ToBatchPath(relative)));
                L("if errorlevel 1 goto place_failed");
            }
            foreach (var relative in plan.AddOnlyFiles)
            {
                L("call :place_addition " + QuoteBatchArgument(ToBatchPath(relative)));
                L("if errorlevel 1 goto place_failed");
            }
            L("if exist \"%WWWROOT_OLD%\" rmdir /s /q \"%WWWROOT_OLD%\"");
            L("if exist \"%WWWROOT_NEW%\" rmdir /s /q \"%WWWROOT_NEW%\"");
            L("rmdir /s /q \"%STAGING%\"");
            L("if errorlevel 1 goto workspace_cleanup_failed");
            L("rmdir /s /q \"%BACKUP_DIR%\"");
            L("if errorlevel 1 goto workspace_cleanup_failed");
            L("call :log S22_PLACE OK items=%PLACED_ITEMS% staging=removed backup=removed");
            L("call :log S23_ROLLBACK SKIP reason=placement_succeeded");
            L("goto finish");
            L(":workspace_cleanup_failed");
            L("set \"STATUS=failed\"");
            L("set \"ERROR_CODE=WORKSPACE_CLEANUP_FAILED\"");
            L("call :log S22_PLACE NG code=WORKSPACE_CLEANUP_FAILED");
            L("call :log S23_ROLLBACK SKIP reason=placement_succeeded");
            L("goto finish");
            L(":wwwroot_residue");
            L("set \"STATUS=failed\"");
            L("set \"ERROR_CODE=WWWROOT_RESIDUE\"");
            L("call :log S21_BACKUP NG code=WWWROOT_RESIDUE");
            L("goto finish");
            L(":backup_failed");
            L("set \"STATUS=failed\"");
            L("set \"ERROR_CODE=BACKUP_FAILED\"");
            L("call :log S21_BACKUP NG code=BACKUP_FAILED");
            L("goto finish");
            L(":place_failed");
            L("call :log S22_PLACE NG code=PLACE_FAILED");
            L("call :rollback_all");
            L("if errorlevel 1 (set \"STATUS=failed\"&set \"ERROR_CODE=ROLLBACK_FAILED\"&call :log S23_ROLLBACK NG code=ROLLBACK_FAILED) else (set \"STATUS=rolled_back\"&set \"ERROR_CODE=PLACE_FAILED\"&call :log S23_ROLLBACK OK code=PLACE_FAILED)");
            L(":finish");
            L("rem ロールバックを含む配置結果を確定してから、記録済みコマンドで再起動します。");
            L("call :restart_server");
            L("if errorlevel 1 if \"%STATUS%\"==\"success\" (set \"STATUS=failed\"&set \"ERROR_CODE=RESTART_FAILED\")");
            L("call :write_result");
            L("if \"%STATUS%\"==\"success\" exit /b 0");
            L("exit /b 3");
            AppendBatchFunctions(L, prepared.Version, plan);
            return builder.ToString();
        }

        private static void AppendBatchFunctions(Action<string> line, string version,
            UpdatePlan plan)
        {
            line(":log");
            line(">>\"%LOG_PATH%\" echo [Update][%TXID%][main][%~1] %~2 %~3 %~4 %~5");
            line("exit /b 0");
            line(":server_running");
            line("powershell -NoProfile -Command \"$p=Get-Process -ErrorAction SilentlyContinue; if ($p | Where-Object { $_.Id -eq %SERVER_PID% -or $_.ProcessName -in @('AmatsukazeServerCLI','AmatsukazeGUI','AmatsukazeCLI') }) { exit 0 } else { exit 1 }\"");
            line("exit /b %errorlevel%");
            line(":backup_file");
            line("if not exist \"%APP_ROOT%\\%~1\" exit /b 0");
            line("if exist \"%APP_ROOT%\\%~1\\NUL\" exit /b 1");
            line("for %%D in (\"%BACKUP_DIR%\\%~1\") do if not exist \"%%~dpD\" mkdir \"%%~dpD\"");
            line("copy /b /y \"%APP_ROOT%\\%~1\" \"%BACKUP_DIR%\\%~1\" >nul");
            line("exit /b %errorlevel%");
            line(":remember_addition");
            line("if not exist \"%APP_ROOT%\\%~1\" >>\"%CREATED_LIST%\" echo %~1");
            line("exit /b 0");
            line(":place_file");
            line("for %%D in (\"%APP_ROOT%\\%~1\") do if not exist \"%%~dpD\" mkdir \"%%~dpD\"");
            line("copy /b /y \"%STAGING%\\%~1\" \"%APP_ROOT%\\%~1\" >nul || exit /b 1");
            line("set /a PLACED_ITEMS+=1");
            line("if %FAIL_AFTER_PLACED_ITEMS% GTR 0 if %PLACED_ITEMS% GEQ %FAIL_AFTER_PLACED_ITEMS% exit /b 1");
            line("exit /b 0");
            line(":place_addition");
            line("if exist \"%APP_ROOT%\\%~1\" exit /b 0");
            line("call :place_file %1");
            line("exit /b %errorlevel%");
            line(":restore_file");
            line("if exist \"%APP_ROOT%\\%~1\" del /f /q \"%APP_ROOT%\\%~1\"");
            line("if not exist \"%BACKUP_DIR%\\%~1\" exit /b 0");
            line("for %%D in (\"%APP_ROOT%\\%~1\") do if not exist \"%%~dpD\" mkdir \"%%~dpD\"");
            line("copy /b /y \"%BACKUP_DIR%\\%~1\" \"%APP_ROOT%\\%~1\" >nul");
            line("exit /b %errorlevel%");
            line(":backup_wwwroot");
            line("if not exist \"%APP_ROOT%\\exe_files\\wwwroot\" exit /b 0");
            line("robocopy \"%APP_ROOT%\\exe_files\\wwwroot\" \"%BACKUP_DIR%\\exe_files\\wwwroot\" /MIR /R:1 /W:1 >nul");
            line("if errorlevel %ROBOCOPY_FAILURE_MIN% exit /b 1");
            line("exit /b 0");
            line(":validate_wwwroot");
            line("if exist \"%WWWROOT_NEW%\" exit /b 1");
            line("if exist \"%WWWROOT_OLD%\" exit /b 1");
            line("exit /b 0");
            line(":place_wwwroot");
            line("if exist \"%WWWROOT_NEW%\" exit /b 1");
            line("if exist \"%WWWROOT_OLD%\" exit /b 1");
            line("robocopy \"%STAGING%\\exe_files\\wwwroot\" \"%WWWROOT_NEW%\" /MIR /R:1 /W:1 >nul");
            line("if errorlevel %ROBOCOPY_FAILURE_MIN% exit /b 1");
            line("if not exist \"%APP_ROOT%\\exe_files\\wwwroot\" goto place_wwwroot_new");
            line("move /y \"%APP_ROOT%\\exe_files\\wwwroot\" \"%WWWROOT_OLD%\" >nul || exit /b 1");
            line(":place_wwwroot_new");
            line("move /y \"%WWWROOT_NEW%\" \"%APP_ROOT%\\exe_files\\wwwroot\" >nul || exit /b 1");
            line("set /a PLACED_ITEMS+=1");
            line("if %FAIL_AFTER_PLACED_ITEMS% GTR 0 if %PLACED_ITEMS% GEQ %FAIL_AFTER_PLACED_ITEMS% exit /b 1");
            line("exit /b 0");
            line(":rollback_all");
            line("set \"ROLLBACK_FAILED=0\"");
            line("if exist \"%APP_ROOT%\\exe_files\\wwwroot\" rmdir /s /q \"%APP_ROOT%\\exe_files\\wwwroot\"");
            line("if exist \"%WWWROOT_OLD%\" goto rollback_wwwroot_old");
            line("if not exist \"%BACKUP_DIR%\\exe_files\\wwwroot\" goto rollback_wwwroot_done");
            line("robocopy \"%BACKUP_DIR%\\exe_files\\wwwroot\" \"%APP_ROOT%\\exe_files\\wwwroot\" /MIR /R:1 /W:1 >nul");
            line("if errorlevel %ROBOCOPY_FAILURE_MIN% set \"ROLLBACK_FAILED=1\"");
            line("goto rollback_wwwroot_done");
            line(":rollback_wwwroot_old");
            line("move /y \"%WWWROOT_OLD%\" \"%APP_ROOT%\\exe_files\\wwwroot\" >nul || set \"ROLLBACK_FAILED=1\"");
            line(":rollback_wwwroot_done");
            foreach (var relative in plan.OverwriteFiles)
            {
                var path = QuoteBatchValue(relative.Replace('/', '\\'));
                line("call :restore_file \"" + path + "\"");
                line("if errorlevel 1 set \"ROLLBACK_FAILED=1\"");
            }
            line("if exist \"%CREATED_LIST%\" for /f \"usebackq delims=\" %%P in (\"%CREATED_LIST%\") do if exist \"%APP_ROOT%\\%%P\" del /f /q \"%APP_ROOT%\\%%P\"");
            line("if \"%ROLLBACK_FAILED%\"==\"1\" exit /b 1");
            line("exit /b 0");
            line(":restart_server");
            line("if \"%RESTART_EXE%\"==\"\" (call :log S24_RESTART NG code=RESTART_COMMAND_MISSING&exit /b 1)");
            line("rem start は子 PID を返さないため、Windows では実行ファイルの存在と start の終了コードを確認します。");
            line("if not exist \"%RESTART_EXE%\" (call :log S24_RESTART NG code=RESTART_FAILED reason=not_found&exit /b 1)");
            line("start \"\" \"%RESTART_EXE%\" %RESTART_ARGS%");
            line("if errorlevel 1 (call :log S24_RESTART NG code=RESTART_FAILED&exit /b 1)");
            line("call :log S24_RESTART OK process=\"%RESTART_EXE%\"");
            line("exit /b 0");
            line(":write_result");
            line("for /f %%I in ('powershell -NoProfile -Command \"[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ',[Globalization.CultureInfo]::InvariantCulture)\"') do set \"FINISHED=%%I\"");
            line(">\"%RESULT_PATH%.tmp\" echo txid=%TXID%");
            line(">>\"%RESULT_PATH%.tmp\" echo status=%STATUS%");
            line(">>\"%RESULT_PATH%.tmp\" echo version=" + QuoteBatchValue(version));
            line(">>\"%RESULT_PATH%.tmp\" echo started=%STARTED%");
            line(">>\"%RESULT_PATH%.tmp\" echo finished=%FINISHED%");
            line(">>\"%RESULT_PATH%.tmp\" echo error_code=%ERROR_CODE%");
            line(">>\"%RESULT_PATH%.tmp\" <nul set /p \"=log=%LOG_PATH%\"");
            line(">>\"%RESULT_PATH%.tmp\" echo(");
            line("move /y \"%RESULT_PATH%.tmp\" \"%RESULT_PATH%\" >nul");
            line("if \"%STATUS%\"==\"success\" (call :log S29_RESULT OK status=%STATUS% error_code=none) else (call :log S29_RESULT NG status=%STATUS% error_code=%ERROR_CODE%)");
            line("exit /b 0");
        }

        private static void ValidateInputs(string appRoot, long serverPid,
            PreparedSelfUpdate prepared, string transactionId, SelfUpdaterScriptOptions options)
        {
            if (string.IsNullOrWhiteSpace(appRoot)) throw new ArgumentException(
                "アプリルートが空です", nameof(appRoot));
            if (serverPid < 0) throw new ArgumentOutOfRangeException(nameof(serverPid));
            if (!TransactionIdRegex.IsMatch(transactionId ?? string.Empty))
                throw new UpdatePreparationException("INVALID_TRANSACTION_ID", "S01_PRECHECK",
                    "更新トランザクションIDが不正です");
            if (string.IsNullOrWhiteSpace(prepared.StagingDirectory) ||
                !Directory.Exists(prepared.StagingDirectory))
                throw new UpdatePreparationException("INVALID_STAGING_PATH", "S01_PRECHECK",
                    "本体更新の staging がありません");
            if (prepared.RestartCommandLine == null)
                throw new UpdatePreparationException("RESTART_COMMAND_MISSING", "S09_STAGE",
                    "再起動コマンドラインがありません");
            if (options.WaitTimeoutSeconds <= 0 || options.PollIntervalSeconds <= 0 ||
                options.FailAfterPlacedItems < 0)
                throw new ArgumentOutOfRangeException(nameof(options),
                    "待機時間と失敗注入位置が不正です");
        }

        private static void WriteScript(string path, string content, UpdateOSKind os)
        {
            var temporary = path + ".tmp";
            File.WriteAllText(temporary, content, new UTF8Encoding(false));
            File.Move(temporary, path, true);
            if (os == UpdateOSKind.Linux && !OperatingSystem.IsWindows())
            {
                File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite |
                    UnixFileMode.UserExecute | UnixFileMode.GroupRead |
                    UnixFileMode.GroupExecute | UnixFileMode.OtherRead |
                    UnixFileMode.OtherExecute);
            }
        }

        private static string QuoteShell(string value)
        {
            value ??= string.Empty;
            if (value.IndexOfAny(new[] { '\'', '\r', '\n' }) >= 0)
                throw new UpdatePreparationException("UNSAFE_SCRIPT_ARGUMENT", "S09_STAGE",
                    "シェルスクリプトへ安全に埋め込めない引用符または改行があります");
            return "'" + value + "'";
        }

        private static string QuoteBatchValue(string value)
        {
            value ??= string.Empty;
            if (value.IndexOfAny(new[] { '"', '%', '!', '\r', '\n' }) >= 0)
                throw new UpdatePreparationException("UNSAFE_SCRIPT_ARGUMENT", "S09_STAGE",
                    "バッチファイルへ安全に埋め込めない文字があります");
            return value;
        }

        private static string QuoteBatchArgument(string value) =>
            "\"" + QuoteBatchValue(value) + "\"";

        private static string ToBatchPath(string value) => value.Replace('/', '\\');

        private sealed record UpdatePlan(IReadOnlyList<string> OverwriteFiles,
            IReadOnlyList<string> AddOnlyFiles, IReadOnlyList<string> ReplaceDirectories);
    }
}

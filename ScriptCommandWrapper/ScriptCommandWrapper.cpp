#include "rgy_osdep.h"
#include "rgy_tchar.h"
#include "rgy_pipe.h"
#include "rgy_filesystem.h"
#include "rgy_util.h"
#include <thread>
#include <io.h>
#include <fcntl.h>

static void writeStderrText(const tstring& message) {
    const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD consoleMode = 0;
    if (stderrHandle != INVALID_HANDLE_VALUE && stderrHandle != nullptr
        && GetConsoleMode(stderrHandle, &consoleMode)) {
        DWORD written = 0;
        if (WriteConsoleW(stderrHandle, message.data(), (DWORD)message.size(), &written, nullptr)) {
            return;
        }
    }

    // パイプやファイルへのリダイレクト時は、呼び出し元が期待するANSIバイト列で出力する。
    const std::string ansiMessage = tchar_to_string(message, CP_ACP);
    fwrite(ansiMessage.data(), 1, ansiMessage.size(), stderr);
    fflush(stderr);
}

int _tmain(int argc, TCHAR **argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    try {
        // 自身のフルパスを取得
        const tstring modulePath = getExePathW();

        // 自身のディレクトリ
        const tstring exeDir = PathRemoveBackslashS(PathRemoveFileSpecFixed(modulePath).second);

        // 一階層上のディレクトリ（exe_files を想定）
        const tstring parentDir = PathRemoveBackslashS(PathRemoveFileSpecFixed(exeDir).second);

        // 呼び出す ScriptCommand.exe のフルパス
        const tstring scriptExe = PathCombineS(parentDir, _T("ScriptCommand.exe"));

        // 自分のファイル名（拡張子なし）をコマンド名として渡す
        const tstring commandName = PathRemoveExtensionS(PathGetFilename(modulePath));

        // 子プロセス起動設定（非表示、親の環境・ハンドル継承）
        std::unique_ptr<RGYPipeProcess> process = createRGYPipeProcess();
        process->init(PIPE_MODE_DISABLE, PIPE_MODE_ENABLE, PIPE_MODE_ENABLE);

        // 引数: [ScriptCommand.exe] [自分のファイル名] [以降は渡された引数]
        std::vector<tstring> args;
        args.push_back(scriptExe);
        args.push_back(commandName);
        for (int i = 1; i < argc; i++) {
            args.push_back(argv[i]);
        }

        tstring cmd;
        for (auto arg : args) {
            cmd += arg + _T(" ");
        }
        // stderr は子プロセスの出力をそのまま中継するためバイナリモードにしている。
        // コンソールにはUnicode、パイプやファイルにはANSIで診断行を出力する。
        writeStderrText(_T("ScriptCommandWrapper: ") + cmd + _T("\n"));

        // 作業ディレクトリは ScriptCommand.exe のある親ディレクトリに設定
        int runResult = process->run(args, parentDir.c_str(), 0, true, false, false);
        if (runResult != 0) {
            return -1;
        }

        // 標準出力/標準エラーを並行で読み出して自身に出力
        std::thread thOut([&]() {
            std::vector<uint8_t> buffer;
            while (process->stdOutRead(buffer) >= 0) {
                if (buffer.size() > 0) {
                    fwrite(buffer.data(), 1, buffer.size(), stdout);
                    fflush(stdout);
                    buffer.clear();
                }
            }
            process->stdOutRead(buffer);
            if (buffer.size() > 0) {
                fwrite(buffer.data(), 1, buffer.size(), stdout);
                fflush(stdout);
                buffer.clear();
            }
        });

        std::thread thErr([&]() {
            std::vector<uint8_t> buffer;
            while (process->stdErrRead(buffer) >= 0) {
                if (buffer.size() > 0) {
                    fwrite(buffer.data(), 1, buffer.size(), stderr);
                    fflush(stderr);
                    buffer.clear();
                }
            }
            process->stdErrRead(buffer);
            if (buffer.size() > 0) {
                fwrite(buffer.data(), 1, buffer.size(), stderr);
                fflush(stderr);
                buffer.clear();
            }
        });

        // 子プロセスの終了コードを取得
        int exitCode = process->waitAndGetExitCode();

        // 読み取りスレッドの終了待ち
        if (thOut.joinable()) thOut.join();
        if (thErr.joinable()) thErr.join();

        return exitCode;
    } catch (...) {
        return -1;
    }
}

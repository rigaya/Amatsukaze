/**
* Amtasukaze CLI Entry point
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <windows.h>
#include <iostream>

typedef int (*AmatsukazeCLIFunc)(int argc, const wchar_t* argv[]);

int wmain(int argc, const wchar_t* argv[]) {
    int loadTarget = 0;
    for (int iarg = 1; iarg < argc; iarg++) {
        if (wcscmp(argv[iarg], L"--loadv2") == 0) {
            loadTarget = 1;
        }
    }

    static const wchar_t* dllnames[] = { L"Amatsukaze.dll", L"Amatsukaze2.dll" };
    HMODULE hModule = LoadLibrary(dllnames[loadTarget]);
    if (hModule == NULL) {
        std::wcerr << L"Failed to load " << dllnames[loadTarget] << std::endl;
        return -1;
    }

    AmatsukazeCLIFunc AmatsukazeCLI = (AmatsukazeCLIFunc)GetProcAddress(hModule, "AmatsukazeCLI");
    if (AmatsukazeCLI == NULL) {
        std::wcerr << L"Failed to find AmatsukazeCLI function" << std::endl;
        FreeLibrary(hModule);
        return -1;
    }

    int result = AmatsukazeCLI(argc, argv);

    FreeLibrary(hModule);
    return result;
}

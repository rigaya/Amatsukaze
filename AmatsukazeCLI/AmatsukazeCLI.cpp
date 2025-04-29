/**
* Amtasukaze CLI Entry point
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "rgy_osdep.h"
#include "rgy_tchar.h"
#include <iostream>

typedef int (*AmatsukazeCLIFunc)(int argc, const TCHAR* argv[]);

int _tmain(int argc, const TCHAR* argv[]) {
    int loadTarget = 0;
#if defined(_WIN32) || defined(_WIN64)
    for (int iarg = 1; iarg < argc; iarg++) {
        if (wcscmp(argv[iarg], L"--loadv2") == 0) {
            loadTarget = 1;
        }
    }

    static const wchar_t* dllnames[] = { L"Amatsukaze.dll", L"Amatsukaze2.dll" };
#else
    static const TCHAR* dllnames[] = { _T("libAmatsukaze.so") };
#endif
    auto hModule = RGY_LOAD_LIBRARY(dllnames[loadTarget]);
    if (hModule == NULL) {
        std::wcerr << L"Failed to load " << dllnames[loadTarget] << std::endl;
        return -1;
    }

    AmatsukazeCLIFunc AmatsukazeCLI = (AmatsukazeCLIFunc)RGY_GET_PROC_ADDRESS(hModule, "AmatsukazeCLI");
    if (AmatsukazeCLI == NULL) {
        std::wcerr << L"Failed to find AmatsukazeCLI function" << std::endl;
        RGY_FREE_LIBRARY(hModule);
        return -1;
    }

    int result = AmatsukazeCLI(argc, argv);

    RGY_FREE_LIBRARY(hModule);
    return result;
}

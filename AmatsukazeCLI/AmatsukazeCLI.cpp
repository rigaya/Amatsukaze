/**
* Amtasukaze CLI Entry point
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "rgy_osdep.h"
#include "rgy_tchar.h"
#include "rgy_filesystem.h"
#include <iostream>

typedef int (*AmatsukazeCLIFunc)(int argc, const TCHAR* argv[]);

int _tmain(int argc, const TCHAR* argv[]) {
    int loadTarget = 0;
    for (int iarg = 1; iarg < argc; iarg++) {
        if (_tcscmp(argv[iarg], _T("--loadv2")) == 0) {
            loadTarget = 1;
        }
    }
#if defined(_WIN32) || defined(_WIN64)
    static const wchar_t* dllnames[] = { L"Amatsukaze.dll", L"Amatsukaze2.dll" };
    auto hModule = RGY_LOAD_LIBRARY(dllnames[loadTarget]);
    if (hModule == NULL) {
        std::wcerr << L"Failed to load " << dllnames[loadTarget] << std::endl;
        return -1;
    }
#else
    std::vector<std::string> dllnames = { PathCombineS(getExeDir(), _T("libAmatsukaze.so")), PathCombineS(getExeDir(), _T("libAmatsukaze2.so")) };
    auto hModule = RGY_LOAD_LIBRARY(dllnames[loadTarget].c_str());
    if (hModule == NULL) {
        std::cerr << "Failed to load " << dllnames[loadTarget] << std::endl;
        return -1;
    }
#endif
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

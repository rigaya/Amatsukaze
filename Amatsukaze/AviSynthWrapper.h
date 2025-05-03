#pragma once

/**
* AviSynth Wrapper
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#ifdef _stricmp
#undef _stricmp
#endif
RGY_DISABLE_WARNING_PUSH
RGY_DISABLE_WARNING_STR("-Wsign-compare")
#include "avisynth.h"
#pragma comment(lib, "avisynth.lib")
RGY_DISABLE_WARNING_POP


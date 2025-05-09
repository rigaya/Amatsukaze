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

#define AVISYNTH_NEO (1)
#define AVISYNTH_PLUS (2) 

#if defined(_WIN32) || defined(_WIN64)
#define AVISYNTH_MODE AVISYNTH_NEO
#else
#define AVISYNTH_MODE AVISYNTH_PLUS
#endif

#if AVISYNTH_MODE == AVISYNTH_PLUS
#define GetProperty GetEnvProperty
#define CopyFrameProps copyFrameProps
#endif

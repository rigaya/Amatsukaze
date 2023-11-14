
#ifndef _AMATSUKAZE_VERSION_H_
#define _AMATSUKAZE_VERSION_H_

#include "Version.h"

#ifdef DEBUG
#define VER_DEBUG   VS_FF_DEBUG
#define VER_PRIVATE VS_FF_PRIVATEBUILD
#else
#define VER_DEBUG   0
#define VER_PRIVATE 0
#endif

#define VER_STR_COMMENTS         "Amatsukaze mod by rigaya"
#define VER_STR_COMPANYNAME      ""
#define VER_STR_FILEDESCRIPTION  "Amatsukaze"
#define VER_STR_INTERNALNAME     "Amatsukaze"
#define VER_STR_ORIGINALFILENAME "Amatsukaze.exe"
#define VER_STR_LEGALCOPYRIGHT   "Copyright (c)  2017-2019 Nekopanda"
#define VER_STR_PRODUCTNAME      "Amatsukaze"
#define VER_PRODUCTVERSION       AMATSUKAZE_PRODUCTVERSION
#define VER_STR_PRODUCTVERSION   AMATSUKAZE_VERSION
#define VER_FILEVERSION          AMATSUKAZE_PRODUCTVERSION
#define VER_STR_FILEVERSION      AMATSUKAZE_VERSION

#endif //_AMATSUKAZE_VERSION_H_

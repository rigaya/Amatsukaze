#pragma once
#include <string>
#include "AviSynthWrapper.h"

void DrawText(const PVideoFrame &dst, bool isYV12, int x1, int y1, const std::string& s);

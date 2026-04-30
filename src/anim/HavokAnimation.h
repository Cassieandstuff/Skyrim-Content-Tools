#pragma once
#include "anim/AnimClip.h"

// Parse the first animation from a Havok packfile XML.
// Supports hkaInterleavedUncompressedAnimation and hkaSplineCompressedAnimation.
// Returns true on success; writes a message into errOut on failure.
bool LoadHavokAnimationXml(const char* path, AnimClip& out, char* errOut, int errLen);

// Same as above but reads from an in-memory XML buffer (e.g. from DotNetHost::HkxToXml).
// `name` is used as the clip name since there is no file path to derive it from.
bool LoadHavokAnimationXmlFromBuffer(const char* xmlData, int xmlLen,
                                     const char* name,
                                     AnimClip& out, char* errOut, int errLen);

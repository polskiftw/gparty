#pragma once

// vcpkg installs libwebm's public parser headers below include/webm/ while
// upstream source-tree examples include them from the repository root. Keep
// media_decode.cpp on the upstream spelling and bridge only the packaged
// include layout here.
#include <webm/mkvparser/mkvparser.h>

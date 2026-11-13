#pragma once

class SharedFrameWriter;

// Installs the D3D8 Present hook. Kept in its own translation unit because the Wine d3d8 headers
// define types (D3DPRESENT_PARAMETERS and friends) that conflict with the Windows SDK's d3d9.h.
bool InstallD3D8Hooks(SharedFrameWriter* writer, bool (*shouldCapture)(), void (*log)(const char*));

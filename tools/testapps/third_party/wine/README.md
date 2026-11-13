# Wine d3d8 headers

`d3d8.h`, `d3d8types.h`, and `d3d8caps.h` are copied unmodified from the Wine project
(https://github.com/wine-mirror/wine, `include/`). They provide the Direct3D 8 interfaces that the
modern Windows SDK no longer ships.

These headers are used only by the capture test apps in `tools/testapps`; the shipping application
does not include them. Wine headers are LGPL-2.1-or-later; the full license text is in
`COPYING.LIB`.

`d3d8.cpp` defines `__MSABI_LONG(x)` to `x` before including them because that macro is normally
provided by Wine's own `windef.h`.

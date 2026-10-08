// SPDX-License-Identifier: GPL-3.0-only

// Included into every test, by CMake, only where this Qt's QTRY_ macros
// narrow a timeout to int (Qt 6.8, which Debian trixie and so CI has; not
// Qt 6.12): under -Werror=conversion each test using them would not build.
// A pragma, so no flag after it turns the warning back on.
#pragma GCC diagnostic ignored "-Wconversion"

#pragma once

// Stand-in for the header CMake's generate_export_header() produces inside the
// GWToolboxdll build tree. We compile a few Toolbox source files (ToolboxIni,
// SettingsDoc) straight into our DLL but deliberately do not link GWToolboxdll
// itself, so its export attributes have to collapse to nothing and the handful
// of symbols those files reference are provided by compat/logger_stub.cpp.

#define GWTOOLBOXDLL_EXPORT
#define GWTOOLBOXDLL_NO_EXPORT
#define GWTOOLBOXDLL_DEPRECATED __declspec(deprecated)
#define GWTOOLBOXDLL_DEPRECATED_EXPORT GWTOOLBOXDLL_DEPRECATED
#define GWTOOLBOXDLL_DEPRECATED_NO_EXPORT GWTOOLBOXDLL_DEPRECATED

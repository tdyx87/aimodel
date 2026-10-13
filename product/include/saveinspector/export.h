// 导出宏：仅共享库启用 dllexport/dllimport；静态库为空。
// 命名沿用 `<LIB>_API` 约定（roles.md §3.7 统一导出宏）。
#ifndef SAVEINSPECTOR_EXPORT_H_
#define SAVEINSPECTOR_EXPORT_H_

#if defined(_WIN32) || defined(_WIN64)
#  if defined(SAVEINSPECTOR_SHARED)
#    if defined(SAVEINSPECTOR_BUILD)
#      define SAVEINSPECTOR_API __declspec(dllexport)
#    else
#      define SAVEINSPECTOR_API __declspec(dllimport)
#    endif
#  else
#    define SAVEINSPECTOR_API
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define SAVEINSPECTOR_API __attribute__((visibility("default")))
#else
#  define SAVEINSPECTOR_API
#endif

#endif  // SAVEINSPECTOR_EXPORT_H_

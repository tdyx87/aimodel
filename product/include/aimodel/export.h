// 导出宏：仅共享库启用 dllexport/dllimport；静态库为空（agents.md 第 9 节：
// 禁止裸 __declspec，统一经 AIMODEL_API；AIMODEL_SHARED 由 CMake 对目标 PUBLIC 传递）。
#ifndef AIMODEL_EXPORT_H_
#define AIMODEL_EXPORT_H_

#if defined(_WIN32) || defined(_WIN64)
#  if defined(AIMODEL_SHARED)
#    if defined(AIMODEL_BUILD)
#      define AIMODEL_API __declspec(dllexport)
#    else
#      define AIMODEL_API __declspec(dllimport)
#    endif
#  else
#    define AIMODEL_API
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define AIMODEL_API __attribute__((visibility("default")))
#else
#  define AIMODEL_API
#endif

#endif  // AIMODEL_EXPORT_H_

// 垃圾清理工具 —— 程序入口
//
// 走 NIM Duilib 的标准启动路径：
//   1. 在 UI 线程上建一个消息循环（nbase::FrameworkThread）
//   2. GlobalManager::Startup 加载全局皮肤资源
//   3. 创建主窗口，进入消息循环
//
// 命令行：garbage_cleaner_gui.exe [扫描目录]
//   给了目录就以它作为初始扫描目录（便于脚本化 / 自动化冒烟测试），
//   不给就用 exe 所在目录。

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "duilib/UIlib.h"

#include "base/base.h"
#include "base/framework/message_loop.h"
#include "base/thread/framework_thread.h"

#include "gc_log.h"
#include "gc_main_form.h"

namespace {

class MainThread : public nbase::FrameworkThread {
public:
    MainThread() : nbase::FrameworkThread("GarbageCleanerMainThread") {}
    ~MainThread() override = default;

private:
    void Init() override;
    void Cleanup() override;
};

void MainThread::Init() {
    nbase::ThreadManager::RegisterThread(kThreadMain);

    // 资源目录 = exe 同级 resources\；
    // 皮肤取自 resources\themes\default，语言表取自 resources\lang\zh_CN。
    const std::wstring resource_dir = nbase::win32::GetCurrentModuleDirectory() + L"resources\\";
    ui::GlobalManager::Startup(resource_dir, ui::CreateControlCallback(), false);

    // WindowImplBase 会在收到 WM_NCDESTROY 后自行 delete，这里不需要手动释放
    GcMainForm* window = new GcMainForm();
    window->Create(nullptr, GcMainForm::kClassName.c_str(), UI_WNDSTYLE_FRAME, 0);
    window->CenterWindow();
    window->ShowWindow();
}

void MainThread::Cleanup() {
    ui::GlobalManager::Shutdown();
    nbase::FrameworkThread::SetThreadWasQuitProperly(true);
    nbase::ThreadManager::UnregisterThread();
}

} // namespace

namespace {

// 取命令行里的第一个非选项参数当初始扫描目录。
// 用 CommandLineToArgvW 而不是手动拆 lpCmdLine：路径里有空格时手动拆一定会错。
std::wstring ParseInitialScanRoot() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) return std::wstring();

    std::wstring root;
    for (int i = 1; i < argc; ++i) { // argv[0] 是 exe 路径
        const std::wstring arg(argv[i]);
        if (arg.empty() || arg[0] == L'-' || arg[0] == L'/') continue;
        root = arg;
        break;
    }
    ::LocalFree(argv);
    return root;
}

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 先装日志：后面任何一步崩了，日志里至少能留下异常码与地址
    gc::InstallCrashLogger();

    // 必须在建窗口之前写入：窗口的 InitWindow 会读它
    gc_ui::SetInitialScanRoot(ParseInitialScanRoot());
    gc::LogLine(L"启动：日志文件 " + gc::LogFilePath());
    if (!gc_ui::GetInitialScanRoot().empty()) {
        gc::LogLine(L"命令行指定扫描目录：" + gc_ui::GetInitialScanRoot());
    }

    // 目录选择对话框（BIF_NEWDIALOGSTYLE）需要 COM；GDI+ 等组件也依赖它
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    MainThread thread;
    thread.RunOnCurrentThreadWithLoop(nbase::MessageLoop::kUIMessageLoop);

    ::CoUninitialize();
    return 0;
}

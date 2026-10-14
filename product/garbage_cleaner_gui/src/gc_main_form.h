#pragma once

// 主窗口：选择目录 → 扫描 → 展示结果 → 清理。
// 界面线程只跟 duilib 打交道，扫描在工作线程里跑，两边通过 PostMessage + 原子量通信。

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "duilib/UIlib.h"

#include "gc_scanner.h"

namespace gc_ui {

// 启动时的默认扫描目录，由命令行参数给出（wWinMain 里设置）。
// 空字符串表示「没给参数」，此时窗口用 exe 所在目录。
void SetInitialScanRoot(const std::wstring& path);
std::wstring GetInitialScanRoot();

// 工作线程 → 界面线程
constexpr UINT kMsgScanProgress = WM_APP + 101;
constexpr UINT kMsgScanDone = WM_APP + 102;

// 工作线程写入、界面线程读取的进度数据（纯原子量，不加锁）
struct ScanProgressState {
    std::atomic<std::uint64_t> dirs{0};
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> matched{0};
    std::atomic<std::uint64_t> bytes{0};
};

// 工作线程把结果交给界面线程时的交接点
struct ScanOutcome {
    std::mutex mutex;
    std::shared_ptr<gc::ScanResult> result;
};

} // namespace gc_ui

class GcMainForm : public ui::WindowImplBase {
public:
    GcMainForm();
    ~GcMainForm() override;

    static const std::wstring kClassName;

    std::wstring GetSkinFolder() override;
    std::wstring GetSkinFile() override;
    std::wstring GetWindowClassName() const override;

    void InitWindow() override;
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    LRESULT OnClose(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) override;

private:
    bool OnChooseDirClick(ui::EventArgs* msg);
    bool OnScanClick(ui::EventArgs* msg);
    bool OnCleanClick(ui::EventArgs* msg);

    void BeginScan();
    void FinishScan();
    void FillFileList();
    void ClearResults();
    void StopWorkerAndWait();
    void UpdateButtons();

    void SetStatus(const std::wstring& text);
    void SetSummary(const std::wstring& text);
    void SetRootPath(const std::wstring& path);
    void ChooseRootFromDialog();

    ui::Label* status_label_ = nullptr;
    ui::Label* root_label_ = nullptr;
    ui::Label* summary_label_ = nullptr;
    ui::Button* choose_btn_ = nullptr;
    ui::Button* scan_btn_ = nullptr;
    ui::Button* clean_btn_ = nullptr;
    ui::ListBox* file_list_ = nullptr;

    std::wstring root_;        // 当前扫描根目录（已去掉结尾分隔符）
    std::wstring root_lower_;  // 删除时做前缀校验用

    std::shared_ptr<gc::Scanner> scanner_;              // 与工作线程共享，用于中途取消
    std::shared_ptr<gc_ui::ScanProgressState> progress_; // 与工作线程共享的进度原子量
    std::shared_ptr<gc_ui::ScanOutcome> outcome_;        // 与工作线程共享的结果交接点
    std::thread worker_;
    std::atomic<bool> worker_running_{false};

    gc::ScanResult result_;  // 界面线程持有的最近一次完整结果
    bool has_result_ = false;
};

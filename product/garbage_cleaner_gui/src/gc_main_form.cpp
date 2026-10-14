#include "gc_main_form.h"

#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <vector>

#include "base/base.h"

#include "gc_log.h"

namespace {

const wchar_t* const kSkinFolder = L"garbage_cleaner";
const wchar_t* const kSkinFile = L"main_layout.xml";
// 列表行模板：相对路径，duilib 会自动加上全局资源路径前缀
const wchar_t* const kItemTemplate = L"garbage_cleaner/list_item.xml";

std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(static_cast<wchar_t>(::towlower(c)));
    return out;
}

std::wstring RemoveTrailingSeparators(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

// 行模板里子控件的查找：不用 Box::FindSubControl，因为它要先把控件绑定到窗口
// （内部走 GetWindow()->FindSubControlByName），而这里只需在子树里按名字找。
// 注意必须**递归**：名字挂在行内盒（HBox）的子控件上，只查一层是找不到的。
ui::Control* FindChildByName(ui::Box* box, const wchar_t* name) {
    if (!box) return nullptr;
    const std::wstring wanted(name);
    for (int i = 0; i < box->GetCount(); ++i) {
        ui::Control* child = box->GetItemAt(static_cast<std::size_t>(i));
        if (child == nullptr) continue;
        if (child->GetName() == wanted) return child;
        if (ui::Box* sub = dynamic_cast<ui::Box*>(child)) {
            if (ui::Control* found = FindChildByName(sub, name)) return found;
        }
    }
    return nullptr;
}

} // namespace

const std::wstring GcMainForm::kClassName = L"GarbageCleanerMainWnd";

GcMainForm::GcMainForm() = default;

GcMainForm::~GcMainForm() {
    StopWorkerAndWait();
}

std::wstring GcMainForm::GetSkinFolder() {
    return kSkinFolder;
}

std::wstring GcMainForm::GetSkinFile() {
    return kSkinFile;
}

namespace gc_ui {
namespace {

// 只在 UI 线程上读写：wWinMain 建线程前写入，InitWindow 读取
std::wstring g_initial_scan_root;

} // namespace

void SetInitialScanRoot(const std::wstring& path) {
    // 只接受确实存在的目录：传进来的可能是文件、盘符拼错或已被删除的路径，
    // 那种情况下宁可变回默认目录，也不要让用户面对一个空列表而不知为何。
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return;
    g_initial_scan_root = RemoveTrailingSeparators(path);
}

std::wstring GetInitialScanRoot() {
    return g_initial_scan_root;
}

} // namespace gc_ui

std::wstring GcMainForm::GetWindowClassName() const {
    return kClassName;
}

void GcMainForm::InitWindow() {
    // duilib 的窗口标题默认是窗口类名，这里换成用户看得懂的名字（任务栏 / Alt-Tab 会用到）
    ::SetWindowTextW(GetHWND(), L"垃圾清理工具");

    status_label_ = dynamic_cast<ui::Label*>(FindControl(L"status_label"));
    root_label_ = dynamic_cast<ui::Label*>(FindControl(L"root_label"));
    summary_label_ = dynamic_cast<ui::Label*>(FindControl(L"summary_label"));
    choose_btn_ = dynamic_cast<ui::Button*>(FindControl(L"choose_btn"));
    scan_btn_ = dynamic_cast<ui::Button*>(FindControl(L"scan_btn"));
    clean_btn_ = dynamic_cast<ui::Button*>(FindControl(L"clean_btn"));
    file_list_ = dynamic_cast<ui::ListBox*>(FindControl(L"file_list"));

    // nbase::Bind 返回弱引用回调：窗口先于控件销毁时不会回调到悬空对象
    if (choose_btn_) {
        choose_btn_->AttachClick(nbase::Bind(&GcMainForm::OnChooseDirClick, this, std::placeholders::_1));
    }
    if (scan_btn_) {
        scan_btn_->AttachClick(nbase::Bind(&GcMainForm::OnScanClick, this, std::placeholders::_1));
    }
    if (clean_btn_) {
        clean_btn_->AttachClick(nbase::Bind(&GcMainForm::OnCleanClick, this, std::placeholders::_1));
    }

    // 扫描目录优先用命令行参数（便于脚本化与自动化测试），没有就用程序自身所在目录：
    // 可执行文件旁边通常就是构建产物，这样打开即用。
    const std::wstring initial = gc_ui::GetInitialScanRoot();
    SetRootPath(initial.empty() ? nbase::win32::GetCurrentModuleDirectory() : initial);
    SetStatus(L"就绪：确认扫描目录后点击「开始扫描」。");
    gc::LogLine(L"主窗口已创建，当前扫描目录：" + root_);
    UpdateButtons();
}

LRESULT GcMainForm::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case gc_ui::kMsgScanProgress: {
        // 工作线程只通知「有新进展」；具体数字由界面线程自己读原子量，
        // 不跨线程传递容器，省掉一整类数据竞争。
        const std::uint64_t files = progress_ ? progress_->files.load() : 0;
        const std::uint64_t matched = progress_ ? progress_->matched.load() : 0;
        const std::uint64_t bytes = progress_ ? progress_->bytes.load() : 0;
        wchar_t buf[192] = {0};
        ::swprintf_s(buf, L"正在扫描…已检查 %llu 个文件，命中 %llu 个（%s）",
                     static_cast<unsigned long long>(files), static_cast<unsigned long long>(matched),
                     gc::Scanner::FormatSize(bytes).c_str());
        SetStatus(buf);
        return 0;
    }
    case gc_ui::kMsgScanDone:
        FinishScan();
        return 0;
    default:
        break;
    }
    return __super::HandleMessage(uMsg, wParam, lParam);
}

LRESULT GcMainForm::OnClose(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    StopWorkerAndWait();
    ::PostQuitMessage(0L);
    return __super::OnClose(uMsg, wParam, lParam, bHandled);
}

// ---------------------------------------------------------------------------
// 用户操作
// ---------------------------------------------------------------------------

bool GcMainForm::OnChooseDirClick(ui::EventArgs* msg) {
    (void)msg;
    if (worker_running_.load()) {
        SetStatus(L"扫描进行中，请稍候。");
        return true;
    }
    ChooseRootFromDialog();
    return true;
}

bool GcMainForm::OnScanClick(ui::EventArgs* msg) {
    (void)msg;
    if (worker_running_.load()) return true;
    if (root_.empty()) {
        SetStatus(L"请先选择要扫描的目录。");
        return true;
    }
    BeginScan();
    return true;
}

bool GcMainForm::OnCleanClick(ui::EventArgs* msg) {
    (void)msg;
    if (worker_running_.load()) {
        SetStatus(L"扫描进行中，请稍候。");
        return true;
    }
    if (!has_result_ || result_.items.empty()) {
        SetStatus(L"没有待清理的条目，请先扫描。");
        return true;
    }

    wchar_t prompt[512] = {0};
    ::swprintf_s(prompt, L"将删除 %llu 个文件，共 %s。\n\n删除后不可撤销，是否继续？",
                 static_cast<unsigned long long>(result_.items.size()),
                 gc::Scanner::FormatSize(result_.total_bytes).c_str());
    if (::MessageBoxW(GetHWND(), prompt, L"确认清理", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        SetStatus(L"已取消清理。");
        return true;
    }

    std::uint64_t deleted = 0;
    std::uint64_t freed = 0;
    std::uint64_t failed = 0;
    for (const gc::ScanItem& item : result_.items) {
        // 每个文件都重新做一次保护目录 / 归属根目录校验
        if (gc::Scanner::DeleteFileSafely(item.path, root_lower_)) {
            ++deleted;
            freed += item.size;
        } else {
            ++failed;
        }
    }

    ClearResults();
    wchar_t summary[256] = {0};
    ::swprintf_s(summary, L"已删除 %llu 个文件，释放 %s，失败 %llu 个。",
                 static_cast<unsigned long long>(deleted), gc::Scanner::FormatSize(freed).c_str(),
                 static_cast<unsigned long long>(failed));
    gc::LogLine(std::wstring(L"清理结果：") + summary);
    SetStatus(summary);
    SetSummary(L"清理完成，可重新扫描。");
    UpdateButtons();
    return true;
}

// ---------------------------------------------------------------------------
// 扫描流程
// ---------------------------------------------------------------------------

void GcMainForm::BeginScan() {
    ClearResults();
    gc::LogLine(L"开始扫描：" + root_);

    progress_ = std::make_shared<gc_ui::ScanProgressState>();
    outcome_ = std::make_shared<gc_ui::ScanOutcome>();

    gc::ScanOptions options;
    options.root = root_;
    scanner_ = std::make_shared<gc::Scanner>(std::move(options));

    // 工作线程里绝不触碰界面对象：只往这几个共享结构里写数据 + 投递消息
    const HWND hwnd = GetHWND();
    std::shared_ptr<gc_ui::ScanProgressState> progress = progress_;
    scanner_->SetProgress([progress, hwnd](const gc::ScanProgress& p) {
        progress->dirs.store(p.dirs);
        progress->files.store(p.files);
        progress->matched.store(p.matched);
        progress->bytes.store(p.bytes);
        ::PostMessageW(hwnd, gc_ui::kMsgScanProgress, 0, 0);
    });

    std::shared_ptr<gc::Scanner> scanner = scanner_;
    std::shared_ptr<gc_ui::ScanOutcome> outcome = outcome_;
    worker_running_.store(true);

    SetStatus(L"正在扫描…");
    SetSummary(L"扫描进行中，结果将在结束后一次列出。");
    UpdateButtons();

    worker_ = std::thread([scanner, outcome, hwnd]() {
        std::shared_ptr<gc::ScanResult> scan_result = std::make_shared<gc::ScanResult>(scanner->Scan());
        {
            wchar_t buf[256] = {0};
            ::swprintf_s(buf, L"扫描结束：目录 %llu 个 / 文件 %llu 个，命中 %llu 个（%s）",
                         static_cast<unsigned long long>(scan_result->dirs),
                         static_cast<unsigned long long>(scan_result->files),
                         static_cast<unsigned long long>(scan_result->items.size()),
                         gc::Scanner::FormatSize(scan_result->total_bytes).c_str());
            gc::LogLine(buf);
        }
        {
            std::lock_guard<std::mutex> lock(outcome->mutex);
            outcome->result = scan_result;
        }
        ::PostMessageW(hwnd, gc_ui::kMsgScanDone, 0, 0);
    });
}

void GcMainForm::FinishScan() {
    std::shared_ptr<gc::ScanResult> scan_result;
    if (outcome_) {
        std::lock_guard<std::mutex> lock(outcome_->mutex);
        scan_result = outcome_->result;
    }

    // 工作线程已经投递完消息，此处基本立即返回；join 保证线程对象可复用
    if (worker_.joinable()) worker_.join();
    worker_running_.store(false);

    if (!scan_result) {
        SetStatus(L"扫描没有产生结果。");
        UpdateButtons();
        return;
    }

    result_ = *scan_result;
    has_result_ = !result_.items.empty();
    FillFileList();

    wchar_t buf[512] = {0};
    ::swprintf_s(buf, L"扫描完成：命中 %llu 个文件，共 %s。",
                 static_cast<unsigned long long>(result_.items.size()),
                 gc::Scanner::FormatSize(result_.total_bytes).c_str());
    SetStatus(buf);

    ::swprintf_s(buf, L"已检查 %llu 个目录 / %llu 个文件，跳过受保护路径 %llu 处%s%s",
                 static_cast<unsigned long long>(result_.dirs),
                 static_cast<unsigned long long>(result_.files),
                 static_cast<unsigned long long>(result_.skipped_protected),
                 result_.truncated ? L"；结果已达显示上限" : L"",
                 result_.aborted ? L"；扫描被中止" : L"");
    SetSummary(buf);

    UpdateButtons();
}

void GcMainForm::FillFileList() {
    if (!file_list_) {
        gc::LogLine(L"填充列表失败：找不到 file_list 控件");
        return;
    }

    file_list_->RemoveAll();
    if (result_.items.empty()) return;

    std::size_t added = 0;
    bool template_missing = false;

    for (const gc::ScanItem& item : result_.items) {
        // 行控件必须自己 new 出来：模板 XML 只提供属性与子控件，
        // 由 duilib 把这些填进我们给出的容器里。
        ui::ListContainerElement* row = new ui::ListContainerElement();

        // ⚠ builder 必须每行一个，不能提到循环外面复用。
        // Create() 内部每次都会 CMarkup::LoadFromFile()，而 LoadFromFile 先调 Release()；
        // 上游的 Release() 只把 m_pstrXML / m_pElements 置空，却漏了 m_nElements 的归零
        // （duilib/Core/Markup.cpp 里那行就是个空语句 "m_nElements;"）。
        // 于是第二次解析时 _ReserveElement() 认为"还有余额"、不再 realloc，
        // 直接返回 NULL + nElements 的野指针，写进去就是 0xC0000005。
        // 复用一个 builder 列表就会在第二行崩溃——这个坑已用 cdb 实测确认（Markup.cpp:430）。
        ui::WindowBuilder builder;
        ui::Box* filled = builder.Create(kItemTemplate, ui::CreateControlCallback(), this, nullptr, row);
        if (!filled) {
            delete row;
            template_missing = true;
            break;
        }

        if (auto* path = dynamic_cast<ui::Label*>(FindChildByName(row, L"item_path"))) {
            path->SetText(item.path);
        }
        if (auto* category = dynamic_cast<ui::Label*>(FindChildByName(row, L"item_category"))) {
            category->SetText(item.category);
        }
        if (auto* size = dynamic_cast<ui::Label*>(FindChildByName(row, L"item_size"))) {
            size->SetText(gc::Scanner::FormatSize(item.size));
        }

        // 这里不需要手动 InitControls：ListBox::Add → Box::Add 内部会调
        // m_pWindow->InitControls(pControl, this)，把整棵新控件树挂到窗口上。
        if (!file_list_->Add(row)) {
            delete row;
            break;
        }
        ++added;
    }

    gc::LogLine(L"列表已填充：" + std::to_wstring(added) + L" 行" +
                (template_missing ? L"（行模板加载失败）" : L""));

    if (template_missing) {
        SetStatus(L"列表行模板加载失败，未能列出明细（请检查 resources/themes/default/garbage_cleaner/list_item.xml）。");
    } else if (added < result_.items.size()) {
        SetStatus(L"命中条目较多，列表中只显示了一部分。");
    }
}

void GcMainForm::ClearResults() {
    has_result_ = false;
    result_ = gc::ScanResult();
    if (file_list_) file_list_->RemoveAll();
}

void GcMainForm::StopWorkerAndWait() {
    if (scanner_) scanner_->RequestAbort();
    if (worker_.joinable()) worker_.join();
    worker_running_.store(false);
}

void GcMainForm::UpdateButtons() {
    const bool running = worker_running_.load();
    const bool has_items = has_result_ && !result_.items.empty();

    if (choose_btn_) choose_btn_->SetEnabled(!running);
    if (scan_btn_) scan_btn_->SetEnabled(!running && !root_.empty());
    if (clean_btn_) clean_btn_->SetEnabled(!running && has_items);
}

// ---------------------------------------------------------------------------
// 状态与路径
// ---------------------------------------------------------------------------

void GcMainForm::SetStatus(const std::wstring& text) {
    if (status_label_) status_label_->SetText(text);
}

void GcMainForm::SetSummary(const std::wstring& text) {
    if (summary_label_) summary_label_->SetText(text);
}

void GcMainForm::SetRootPath(const std::wstring& path) {
    root_ = RemoveTrailingSeparators(path);
    root_lower_ = ToLowerCopy(root_);
    if (root_label_) root_label_->SetText(root_.empty() ? L"（未选择）" : root_);
    ClearResults();
    SetSummary(L"尚未扫描。");
    UpdateButtons();
}

void GcMainForm::ChooseRootFromDialog() {
    wchar_t display[MAX_PATH] = {0};
    BROWSEINFOW info = {};
    info.hwndOwner = GetHWND();
    info.pszDisplayName = display;
    info.lpszTitle = L"选择要扫描的目录";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST id_list = ::SHBrowseForFolderW(&info);
    if (id_list == nullptr) return;

    wchar_t path[MAX_PATH] = {0};
    const BOOL ok = ::SHGetPathFromIDListW(id_list, path);
    ::CoTaskMemFree(id_list);
    if (!ok) return;

    SetRootPath(path);
    SetStatus(L"已选择目录：" + root_);
}

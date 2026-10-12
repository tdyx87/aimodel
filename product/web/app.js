/*
 * aimodel 硬件报告界面（原生实现，无构建、无外部依赖）
 * 契约：docs/design/http-ui.md —— 同源 GET api/system-info，单请求整体加载。
 * 设计：design/design-system.md、design/screens/system-report.md —— 四态齐备、隐私徽标常驻。
 */
(function () {
    'use strict';

    var ENDPOINT = 'api/system-info';
    var TIMEOUT_MS = 10000;
    var SLOW_HINT_MS = 3000;

    var content = document.getElementById('content');
    var live = document.getElementById('live');
    var osLine = document.getElementById('os-line');
    var refreshBtn = document.getElementById('refresh');
    var refreshFooterBtn = document.getElementById('refresh-footer');
    var buttons = [refreshBtn, refreshFooterBtn];

    var state = { data: null, phase: 'loading', refreshing: false };
    var slowTimer = null;
    var controller = null;

    // ---- 工具 ----
    var GIB = 1024 * 1024 * 1024;
    var MIB = 1024 * 1024;

    function formatBytes(bytes) {
        if (!isFinite(bytes) || bytes <= 0) return '0 MB';
        if (bytes >= GIB) return (bytes / GIB).toFixed(1) + ' GB';
        return Math.round(bytes / MIB) + ' MB';
    }

    function usedPercent(total, free) {
        if (!total || total <= 0) return null;
        return Math.min(100, Math.max(0, ((total - free) / total) * 100));
    }

    function formatFrequency(mhz) {
        if (!isFinite(mhz) || mhz <= 0) return '';
        return (mhz / 1000).toFixed(1) + ' GHz';
    }

    function printerStatus(status) {
        var s = (status || '').toLowerCase();
        if (s.indexOf('print') >= 0) return { label: '打印中', tone: 'is-busy' };
        if (s.indexOf('error') >= 0 || s.indexOf('jam') >= 0) return { label: '异常', tone: 'is-bad' };
        if (s.indexOf('offline') >= 0) return { label: '离线', tone: '' };
        if (!s) return { label: '状态未知', tone: '' };
        return { label: '空闲', tone: 'is-ok' };
    }

    function el(tag, className, text) {
        var node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined && text !== null) node.textContent = String(text);
        return node;
    }

    // ---- 骨架屏 / 空态 ----
    function skeletonBlock() {
        var block = el('div', 'skeleton-block');
        block.appendChild(el('div', 'skeleton-line is-short'));
        block.appendChild(el('div', 'skeleton-line'));
        block.appendChild(el('div', 'skeleton-line is-short'));
        return block;
    }

    function emptyState(icon, text) {
        var box = el('div', 'state');
        var mark = el('span', 'state-icon', icon);
        mark.setAttribute('aria-hidden', 'true');
        box.appendChild(mark);
        box.appendChild(el('span', null, text));
        return box;
    }

    function card(title, icon, bodyNode, dimmed) {
        var section = el('section', dimmed ? 'card is-refreshing' : 'card');
        var heading = el('h2', 'card-title');
        var id = 'card-' + title;
        heading.id = id;
        section.setAttribute('aria-labelledby', id);
        var mark = el('span', 'card-icon', icon);
        mark.setAttribute('aria-hidden', 'true');
        heading.appendChild(mark);
        heading.appendChild(document.createTextNode(title));
        section.appendChild(heading);
        section.appendChild(bodyNode);
        return section;
    }

    function progressBar(percent, label) {
        var bar = el('div', 'bar');
        bar.setAttribute('role', 'progressbar');
        bar.setAttribute('aria-valuemin', '0');
        bar.setAttribute('aria-valuemax', '100');
        bar.setAttribute('aria-valuenow', String(Math.round(percent)));
        bar.setAttribute('aria-label', label);
        var fill = el('div', 'bar-fill' + (percent > 95 ? ' is-danger' : percent > 85 ? ' is-warn' : ''));
        fill.style.width = percent + '%';
        bar.appendChild(fill);
        return bar;
    }

    // ---- 各卡片渲染 ----
    function cpuCard(info, dimmed) {
        var cpu = info.cpus && info.cpus.length > 0 ? info.cpus[0] : null;
        if (!cpu) return card('处理器', '🖥', emptyState('🖥', '未检测到处理器'), dimmed);

        var box = el('div');
        box.appendChild(el('div', 'card-main', cpu.brand || '未知型号'));
        var parts = [];
        if (cpu.core_count > 0) parts.push(cpu.core_count + ' 逻辑核');
        if (cpu.physical_core_count > 0) parts.push(cpu.physical_core_count + ' 物理核');
        var freq = formatFrequency(cpu.frequency_mhz);
        if (freq) parts.push(freq);
        box.appendChild(el('div', 'card-detail', parts.join(' · ')));
        return card('处理器', '🖥', box, dimmed);
    }

    function gpuCard(info, dimmed) {
        var gpus = info.gpus || [];
        if (gpus.length === 0) return card('显卡', '🎮', emptyState('🎮', '未检测到显卡'), dimmed);

        var list = el('div', 'list');
        gpus.forEach(function (gpu) {
            var row = el('div', 'row');
            row.appendChild(el('span', 'row-label', gpu.name || '未知显卡'));
            row.appendChild(
                el('span', 'row-value', gpu.video_memory_bytes > 0 ? formatBytes(gpu.video_memory_bytes) + ' 显存' : '共享内存')
            );
            list.appendChild(row);
        });
        return card('显卡', '🎮', list, dimmed);
    }

    function memoryCard(info, dimmed) {
        var mem = info.memory;
        if (!mem || !mem.total_bytes) return card('内存', '💾', emptyState('💾', '未检测到内存信息'), dimmed);

        var total = mem.total_bytes;
        var available = mem.available_bytes;
        var percent = usedPercent(total, available);
        var box = el('div');
        box.appendChild(el('div', 'card-main', formatBytes(total - available) + ' / ' + formatBytes(total) + ' 已用'));
        var row = el('div', 'row');
        row.appendChild(el('span', null, '可用 ' + formatBytes(available)));
        row.appendChild(el('span', 'row-value', Math.round(percent || 0) + '%'));
        box.appendChild(row);
        if (percent !== null) box.appendChild(progressBar(percent, '内存使用率'));
        return card('内存', '💾', box, dimmed);
    }

    function diskCard(info, dimmed) {
        var disks = info.disks || [];
        if (disks.length === 0) return card('磁盘', '🗄', emptyState('🗄', '未检测到磁盘'), dimmed);

        var scroll = disks.length > 5;
        var list = el('div', scroll ? 'list scroll-area' : 'list');
        disks.forEach(function (disk) {
            var item = el('div');
            var row = el('div', 'row');
            row.appendChild(el('span', 'row-label', disk.mount_point || '未知分区'));
            row.appendChild(el('span', 'row-value', formatBytes(disk.total_bytes) + ' · ' + (disk.file_system || '')));
            item.appendChild(row);
            var percent = usedPercent(disk.total_bytes, disk.free_bytes);
            if (percent !== null) {
                item.appendChild(progressBar(percent, (disk.mount_point || '分区') + ' 已用空间'));
                var note = el('div', 'row');
                note.appendChild(el('span', 'card-detail', '剩余 ' + formatBytes(disk.free_bytes)));
                if (percent > 85) note.appendChild(el('span', 'row-note', '空间紧张'));
                item.appendChild(note);
            }
            list.appendChild(item);
        });
        return card('磁盘', '🗄', list, dimmed);
    }

    function printerCard(info, dimmed) {
        var printers = info.printers || [];
        if (printers.length === 0) return card('打印机', '🖨', emptyState('🖨', '未检测到打印机'), dimmed);

        var box = el('div');
        var scroll = printers.length > 5;
        var list = el('div', scroll ? 'list scroll-area' : 'list');
        printers.forEach(function (printer) {
            var status = printerStatus(printer.status);
            var row = el('div', 'row');
            var label = el('span', 'row-label');
            var dot = el('span', 'dot ' + status.tone);
            dot.setAttribute('aria-hidden', 'true');
            label.appendChild(dot);
            label.appendChild(document.createTextNode(printer.name || '未命名打印机'));
            if (printer.is_default) label.appendChild(document.createTextNode('（默认）'));
            label.appendChild(document.createTextNode(' · ' + status.label));
            row.appendChild(label);
            list.appendChild(row);
        });
        box.appendChild(list);
        box.appendChild(el('div', 'card-detail', '共 ' + printers.length + ' 台'));
        return card('打印机', '🖨', box, dimmed);
    }

    // ---- 渲染入口 ----
    function renderSkeletons() {
        content.setAttribute('aria-busy', 'true');
        content.textContent = '';
        content.appendChild(skeletonBlock());
        content.appendChild(skeletonBlock());
        content.appendChild(skeletonBlock());
        content.appendChild(skeletonBlock());
        content.appendChild(skeletonBlock());
    }

    function renderError() {
        content.setAttribute('aria-busy', 'false');
        content.textContent = '';
        var box = el('div', 'global-error');
        box.setAttribute('role', 'alert');
        var icon = el('span', 'state-icon', '😢');
        icon.setAttribute('aria-hidden', 'true');
        box.appendChild(icon);
        box.appendChild(el('h2', 'global-error-title', '无法获取硬件信息'));
        box.appendChild(el('p', 'card-detail', '请确认 aimodel 正在运行，然后重试。'));
        var retry = el('button', 'btn');
        retry.type = 'button';
        retry.appendChild(el('span', 'btn-icon', '⟳')).setAttribute('aria-hidden', 'true');
        retry.appendChild(el('span', 'btn-text', '重试'));
        retry.addEventListener('click', function () {
            load(true);
        });
        box.appendChild(retry);
        content.appendChild(box);
    }

    function renderReport(info, dimmed) {
        content.setAttribute('aria-busy', 'false');
        content.textContent = '';
        content.appendChild(cpuCard(info, dimmed));
        content.appendChild(gpuCard(info, dimmed));
        content.appendChild(memoryCard(info, dimmed));
        content.appendChild(diskCard(info, dimmed));
        content.appendChild(printerCard(info, dimmed));
    }

    function renderFooter(info) {
        if (!info) return;
        var os = [info.os_name, info.os_version].filter(Boolean).join(' ');
        var parts = [];
        if (os) parts.push(os);
        if (info.architecture) parts.push(info.architecture);
        if (info.hostname) parts.push(info.hostname);
        var line = parts.join(' · ');
        osLine.textContent = line
            ? line + ' · 所有检测均在本机完成，数据不会离开你的设备'
            : '所有检测均在本机完成，数据不会离开你的设备';
    }

    function setButtons(refreshing) {
        buttons.forEach(function (btn) {
            btn.disabled = refreshing;
            var icon = btn.querySelector('.btn-icon');
            var text = btn.querySelector('.btn-text');
            var spinner = btn.querySelector('.spinner');
            if (refreshing) {
                if (icon) icon.style.display = 'none';
                if (!spinner) {
                    spinner = el('span', 'spinner');
                    spinner.setAttribute('aria-hidden', 'true');
                    btn.insertBefore(spinner, btn.firstChild);
                }
                if (text) text.textContent = '检测中…';
            } else {
                if (icon) icon.style.display = '';
                if (spinner) spinner.parentNode.removeChild(spinner);
                if (text) text.textContent = btn === refreshFooterBtn ? '刷新' : '刷新';
            }
        });
    }

    // ---- 数据加载 ----
    function load(isRefresh) {
        if (state.refreshing) return;
        state.refreshing = true;
        setButtons(true);

        if (!isRefresh) {
            state.phase = 'loading';
            renderSkeletons();
            announce('正在检测你的电脑配置…');
        } else {
            announce('正在重新检测…');
            if (state.data) renderReport(state.data, true);
        }

        window.clearTimeout(slowTimer);
        slowTimer = window.setTimeout(function () {
            announce('仍在检测…');
        }, SLOW_HINT_MS);

        controller = new AbortController();
        var timer = window.setTimeout(function () {
            controller.abort();
        }, TIMEOUT_MS);

        fetch(ENDPOINT, { method: 'GET', cache: 'no-store', signal: controller.signal })
            .then(function (response) {
                if (!response.ok) throw new Error('HTTP ' + response.status);
                return response.json();
            })
            .then(function (info) {
                state.data = info;
                state.phase = 'ready';
                renderReport(info, false);
                renderFooter(info);
                announce('已更新 · ' + new Date().toLocaleTimeString('zh-CN', { hour12: false }));
            })
            .catch(function () {
                state.phase = 'error';
                renderError();
                announce('检测失败，请确认 aimodel 正在运行');
            })
            .then(function () {
                window.clearTimeout(timer);
                window.clearTimeout(slowTimer);
                state.refreshing = false;
                setButtons(false);
            });
    }

    function announce(text) {
        live.textContent = text;
    }

    refreshBtn.addEventListener('click', function () {
        load(true);
    });
    refreshFooterBtn.addEventListener('click', function () {
        load(true);
    });

    load(false);
})();

/*
 * aimodel 文件传输界面（原生实现，无构建、无外部依赖）
 * 契约：docs/design/file-transfer-contract.md —— POST /api/file/upload、
 *       GET /api/file/download；失败返回 {ok,code,message}。
 * 界面：design/file-transfer-ui.md —— 状态齐备；只展示授权目录名，
 *       不暴露服务端绝对路径与内部结构；权限与路径判定全部由后端承担。
 */
(function () {
    'use strict';

    var MAX_BYTES = 10 * 1024 * 1024;  // 与后端契约 6.4 一致
    var UPLOAD_URL = 'api/file/upload';
    var DOWNLOAD_URL = 'api/file/download';
    var SAVED_DIR_NAME = 'data/uploads';  // 仅展示目录名，不展示绝对路径

    var transferBody = document.getElementById('transfer-body');
    if (!transferBody) return;  // 界面未包含文件传输区时静默跳过

    var state = {
        file: null,       // 当前选择的 File
        phase: 'idle',    // idle | uploading | done | error
        message: '',
        items: []         // 已上传文件名列表（本会话内累积）
    };
    var busy = false;

    // ---- 工具 ----
    function el(tag, className, text) {
        var node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined && text !== null) node.textContent = String(text);
        return node;
    }

    function formatBytes(bytes) {
        if (!isFinite(bytes) || bytes <= 0) return '0 MB';
        if (bytes >= 1024 * 1024 * 1024) return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
        return Math.round(bytes / (1024 * 1024)) + ' MB';
    }

    function announce(text) {
        var live = document.getElementById('live');
        if (live) live.textContent = text;
    }

    // 错误码 → 用户语言文案。只呈现语义，不回显服务端内部细节（界面稿 §6）。
    function messageForCode(code, fallback) {
        switch (code) {
            case 'TOO_LARGE':
                return '文件过大，请选择小于 10 MB 的文件。';
            case 'PATH_NOT_ALLOWED':
                return '路径不合法，请选择“允许保存目录”内的文件名。';
            case 'NOT_FOUND':
                return '文件不存在，可能已被移除。';
            case 'WRITE_FAILED':
                return '保存文件失败，请检查目录是否可写入。';
            default:
                return fallback || '操作失败，请重试。';
        }
    }

    // ---- 片段 ----
    function dropzone() {
        var zone = el('div', 'dropzone');
        zone.setAttribute('role', 'button');
        zone.setAttribute('tabindex', '0');
        zone.setAttribute('aria-label', '选择或拖放文件上传');

        var icon = el('span', 'dropzone-icon', '⬆');
        icon.setAttribute('aria-hidden', 'true');
        zone.appendChild(icon);
        zone.appendChild(el('div', null, '选择文件上传，或把文件拖到这里'));
        zone.appendChild(el('div', 'dropzone-hint', '单个文件上限 ' + formatBytes(MAX_BYTES) + ' · 文件只保存在本机'));

        // 隐藏的 file input：同时服务鼠标点击与键盘操作（界面稿交互规则 1）。
        var input = el('input');
        input.type = 'file';
        input.hidden = true;
        input.addEventListener('change', function () {
            if (input.files && input.files.length > 0) selectFile(input.files[0]);
            input.value = '';  // 清空，允许再次选择同一文件
        });
        zone.appendChild(input);

        zone.addEventListener('click', function () { input.click(); });
        zone.addEventListener('keydown', function (event) {
            if (event.key === 'Enter' || event.key === ' ') {
                event.preventDefault();
                input.click();
            }
        });

        ['dragenter', 'dragover'].forEach(function (type) {
            zone.addEventListener(type, function (event) {
                event.preventDefault();
                zone.classList.add('is-over');
            });
        });
        ['dragleave', 'drop'].forEach(function (type) {
            zone.addEventListener(type, function (event) {
                event.preventDefault();
                zone.classList.remove('is-over');
            });
        });
        zone.addEventListener('drop', function (event) {
            var files = event.dataTransfer && event.dataTransfer.files;
            if (files && files.length > 0) selectFile(files[0]);
        });

        return zone;
    }

    function statusNode() {
        var box = el('div', 'upload-status');
        if (state.phase === 'uploading') {
            box.classList.add('is-busy');
            box.appendChild(el('span', null, '正在上传…'));
            return box;
        }
        if (state.phase === 'done') {
            box.classList.add('is-ok');
            box.appendChild(el('span', null, state.message || '上传成功'));
            return box;
        }
        if (state.phase === 'error') {
            box.classList.add('is-bad');
            box.appendChild(el('span', null, state.message || '上传失败'));
            var retry = el('button', 'btn btn-quiet btn-sm');
            retry.type = 'button';
            retry.appendChild(el('span', 'btn-text', '重试'));
            retry.addEventListener('click', function () {
                if (state.file) uploadFile(state.file);
            });
            box.appendChild(retry);
            return box;
        }
        return box;
    }

    function selectedFileBox() {
        var box = el('div');

        var row = el('div', 'file-row');
        var name = el('span', 'file-name', state.file.name);
        name.title = state.file.name;  // 名称过长时悬停可见全名
        row.appendChild(name);
        row.appendChild(el('span', 'row-value', formatBytes(state.file.size)));
        box.appendChild(row);

        box.appendChild(statusNode());

        var actions = el('div', 'file-row');
        var upload = el('button', 'btn');
        upload.type = 'button';
        upload.disabled = state.phase === 'uploading';
        upload.appendChild(el('span', 'btn-text', '上传'));
        upload.addEventListener('click', function () { uploadFile(state.file); });
        var cancel = el('button', 'btn btn-quiet btn-sm');
        cancel.type = 'button';
        cancel.disabled = state.phase === 'uploading';
        cancel.appendChild(el('span', 'btn-text', '取消选择'));
        cancel.addEventListener('click', function () {
            state.file = null;
            state.phase = 'idle';
            state.message = '';
            render();
        });
        actions.appendChild(upload);
        actions.appendChild(cancel);
        box.appendChild(actions);

        return box;
    }

    function savedPlace() {
        var box = el('div', 'saved-place');
        box.appendChild(el('span', 'saved-place-label', '允许保存目录：'));
        box.appendChild(el('span', 'saved-place-value', SAVED_DIR_NAME));
        return box;
    }

    function downloadList() {
        var box = el('div');
        if (state.items.length === 0) {
            var empty = el('div', 'state');
            var mark = el('span', 'state-icon', '📥');
            mark.setAttribute('aria-hidden', 'true');
            empty.appendChild(mark);
            empty.appendChild(el('span', null, '本次会话还没有上传文件'));
            box.appendChild(empty);
            return box;
        }

        var list = el('div', 'list');
        state.items.forEach(function (name) {
            var row = el('div', 'download-row');
            var label = el('span', 'file-name', name);
            label.title = name;
            row.appendChild(label);

            var btn = el('button', 'btn btn-quiet btn-sm');
            btn.type = 'button';
            var icon = el('span', 'btn-icon', '⬇');
            icon.setAttribute('aria-hidden', 'true');
            btn.appendChild(icon);
            btn.appendChild(el('span', 'btn-text', '下载'));
            btn.addEventListener('click', function () {
                // 直接跳转触发浏览器下载；文件名由 Content-Disposition 决定。
                window.location.href = DOWNLOAD_URL + '?file_name=' + encodeURIComponent(name);
            });
            row.appendChild(btn);
            list.appendChild(row);
        });
        box.appendChild(list);
        return box;
    }

    // ---- 行为 ----
    function selectFile(file) {
        state.file = file;
        if (file.size > MAX_BYTES) {
            // 前端先拦一道（界面稿「超出限制」态）；后端仍会再校验一次。
            state.phase = 'error';
            state.message = '文件过大，请选择小于 ' + formatBytes(MAX_BYTES) + ' 的文件。';
        } else {
            state.phase = 'idle';
            state.message = '';
        }
        render();
    }

    function uploadFile(file) {
        if (busy) return;
        busy = true;
        state.phase = 'uploading';
        state.message = '';
        render();
        announce('正在上传 ' + file.name + '…');

        var controller = new AbortController();
        var timer = window.setTimeout(function () { controller.abort(); }, 30000);

        fetch(UPLOAD_URL + '?file_name=' + encodeURIComponent(file.name), {
            method: 'POST',
            headers: { 'Content-Type': 'application/octet-stream' },
            body: file,
            cache: 'no-store',
            signal: controller.signal
        })
            .then(function (response) {
                return response.json().catch(function () { return null; }).then(function (payload) {
                    return { status: response.status, payload: payload };
                });
            })
            .then(function (result) {
                var payload = result.payload;
                if (!payload || payload.ok !== true) {
                    state.phase = 'error';
                    state.message = messageForCode(payload && payload.code, payload && payload.message);
                    announce('上传失败：' + state.message);
                    return;
                }
                state.phase = 'done';
                state.message = '上传成功';
                // 落盘名由后端决定（重名会加时间戳），用它更新下载列表。
                var saved = payload.file_name;
                if (saved && state.items.indexOf(saved) < 0) {
                    state.items.push(saved);
                    state.items.sort();
                }
                announce('上传成功');
            })
            .catch(function () {
                state.phase = 'error';
                state.message = '上传失败，请确认 aimodel 正在运行。';
                announce(state.message);
            })
            .then(function () {
                window.clearTimeout(timer);
                busy = false;
                render();
            });
    }

    // ---- 渲染 ----
    function render() {
        transferBody.textContent = '';

        if (state.phase === 'idle' || state.phase === 'error') {
            transferBody.appendChild(dropzone());
        }
        if (state.file) {
            if (state.phase === 'idle' || state.phase === 'error') {
                transferBody.appendChild(selectedFileBox());
            } else {
                // 上传中 / 成功：只显示当前文件名与状态，不重复给出选择控件。
                var row = el('div', 'file-row');
                var name = el('span', 'file-name', state.file.name);
                name.title = state.file.name;
                row.appendChild(name);
                row.appendChild(el('span', 'row-value', formatBytes(state.file.size)));
                transferBody.appendChild(row);
                transferBody.appendChild(statusNode());
            }
        }

        transferBody.appendChild(savedPlace());

        transferBody.appendChild(el('div', 'card-detail', '已上传的文件'));
        transferBody.appendChild(downloadList());
    }

    render();
})();

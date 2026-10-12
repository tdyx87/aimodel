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

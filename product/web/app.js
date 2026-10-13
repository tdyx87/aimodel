/*
 * 存档查看器界面（原生实现，无构建、无外部依赖）
 * 契约：docs/design/save-format-contract.md §4（通信协议）
 * 设计：design/design-system.md、design/screens/*.md
 *
 * 三条硬要求来自界面稿，实现时不得打折：
 *   1. 槽位卡片的 7 种状态全部实现，其中 corrupt（已损坏）**必须可点击**——
 *      PRD US-3 要求部分损坏仍展示能读出的部分，禁用的卡片等于把
 *      「整槽拒绝」写进实现。
 *   2. 诊断条目三段式（哪里坏 / 为什么坏 / 能做什么）不可裁剪——
 *      PRD US-5 要求。末段 advice 为空即为实现缺陷。
 *   3. 导出必须走 fetch + blob，禁止 window.location.href——
 *      后者在服务端返回错误 JSON 时会把用户踢出整个界面
 *      （aimodel 曾实测复现）。
 */
(function () {
    'use strict';

    var OPEN_URL = 'api/save/open';
    var SLOTS_URL = 'api/save/slots';
    var SLOT_URL = 'api/save/slot';
    var EXPORT_URL = 'api/save/export';
    var TIMEOUT_MS = 15000;
    var MAX_BYTES = 512 * 1024 * 1024;  // 与服务端 ReadFileBytesReadOnly 上限一致

    var stage = document.getElementById('stage');
    var live = document.getElementById('live');
    var openBtn = document.getElementById('open-btn');

    var state = {
        phase: 'empty',   // empty | parsing | list | detail | diagnosis
        save: null,       // /api/save/open 的 save 字段
        slots: [],        // /api/save/slots 的 slots 数组
        detail: null,     // 当前槽位详情
        diagnostics: [],  // 全局诊断（来自 open）
        message: '',
        busy: false
    };

    // ---------------------------------------------------------------- 工具

    function el(tag, className, text) {
        var node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined && text !== null) node.textContent = String(text);
        return node;
    }

    function announce(text) {
        live.textContent = text;
    }

    function formatBytes(bytes) {
        if (!isFinite(bytes) || bytes <= 0) return '0 MB';
        if (bytes >= 1024 * 1024 * 1024) {
            return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
        }
        if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
        return Math.round(bytes / 1024) + ' KB';
    }

    // ---------------------------------------------------------------- 诊断条目
    // 三段式，缺一不可（设计稿 diagnosis.md §2）。
    var SEVERITY = {
        info: { icon: 'ⓘ', label: '提示', tone: 'is-info' },
        warning: { icon: '⚠', label: '注意', tone: 'is-warn' },
        error: { icon: '✕', label: '问题', tone: 'is-bad' }
    };

    function diagnosticNode(diag) {
        var meta = SEVERITY[diag.severity] || SEVERITY.info;
        var box = el('div', 'diagnostic ' + meta.tone);
        box.setAttribute('role', 'status');

        var head = el('div', 'diagnostic-head');
        // 严重级三重表达：图标 + 文字 + 颜色（不可单靠颜色传达）
        head.appendChild(el('span', 'diagnostic-icon', meta.icon));
        head.appendChild(el('span', 'diagnostic-level', meta.label));
        head.appendChild(el('span', 'diagnostic-title', diag.title));
        box.appendChild(head);

        if (diag.detail) box.appendChild(el('p', 'diagnostic-detail', diag.detail));
        if (diag.advice) {
            var advice = el('p', 'diagnostic-advice');
            advice.appendChild(el('span', 'diagnostic-arrow', '→'));
            advice.appendChild(el('span', null, diag.advice));
            box.appendChild(advice);
        }
        return box;
    }

    // ---------------------------------------------------------------- 空态

    function emptyState() {
        var box = el('div', 'dropzone');
        box.setAttribute('role', 'button');
        box.setAttribute('tabindex', '0');
        box.setAttribute('aria-label', '选择或拖放一个存档文件');

        var icon = el('div', 'dropzone-icon', '⬢');
        icon.setAttribute('aria-hidden', 'true');
        box.appendChild(icon);
        box.appendChild(el('div', 'dropzone-title', '选择或拖放一个存档文件'));
        box.appendChild(el('div', 'dropzone-hint',
            '单个文件上限 ' + formatBytes(MAX_BYTES) +
            ' · 工具只读取存档，绝不修改你的原始文件'));

        var input = el('input');
        input.type = 'file';
        input.hidden = true;
        input.addEventListener('change', function () {
            if (input.files && input.files.length > 0) openFile(input.files[0]);
            input.value = '';  // 清空，允许再次选择同一文件
        });
        box.appendChild(input);

        // 拖放不能是唯一途径：同时提供点击与键盘入口（设计系统 §6）
        box.addEventListener('click', function () { input.click(); });
        box.addEventListener('keydown', function (event) {
            if (event.key === 'Enter' || event.key === ' ') {
                event.preventDefault();
                input.click();
            }
        });
        ['dragenter', 'dragover'].forEach(function (type) {
            box.addEventListener(type, function (event) {
                event.preventDefault();
                box.classList.add('is-over');
            });
        });
        ['dragleave', 'drop'].forEach(function (type) {
            box.addEventListener(type, function (event) {
                event.preventDefault();
                box.classList.remove('is-over');
            });
        });
        box.addEventListener('drop', function (event) {
            var files = event.dataTransfer && event.dataTransfer.files;
            if (files && files.length > 0) openFile(files[0]);
        });

        return box;
    }

    function skeletonList() {
        var wrap = el('div');
        for (var i = 0; i < 3; i++) {
            var card = el('div', 'card skeleton-card');
            card.appendChild(el('div', 'skeleton-line is-short'));
            card.appendChild(el('div', 'skeleton-line'));
            wrap.appendChild(card);
        }
        return wrap;
    }

    // ---------------------------------------------------------------- 文件信息条

    function fileBar(fileName, slotCount) {
        var bar = el('div', 'file-bar');
        bar.appendChild(el('span', 'file-bar-name', fileName));
        var meta = el('span', 'file-bar-meta');
        meta.textContent = slotCount + ' 个槽位 · 格式 v' + (state.save ? state.save.version : '?');
        bar.appendChild(meta);
        var again = el('button', 'btn btn-quiet btn-sm');
        again.type = 'button';
        again.appendChild(el('span', 'btn-text', '换一个存档'));
        again.addEventListener('click', function () {
            state.phase = 'empty';
            state.save = null;
            state.slots = [];
            state.diagnostics = [];
            render();
        });
        bar.appendChild(again);
        return bar;
    }

    // ---------------------------------------------------------------- 槽位卡片
    // 7 态：default / hover / active / healthy / degraded / corrupt / empty
    // corrupt 必须可点击（PRD US-3）。

    function slotTone(slot) {
        if (slot.level === 0 && slot.coins === 0 && slot.playtime === '0 分钟') {
            return 'is-empty';
        }
        return slot.healthy ? 'is-healthy' : 'is-corrupt';
    }

    function slotCard(slot) {
        var tone = slotTone(slot);
        var card = el('div', 'slot-card ' + tone);
        card.setAttribute('role', 'button');
        card.setAttribute('tabindex', '0');

        var head = el('div', 'slot-card-head');
        head.appendChild(el('span', 'slot-index', '槽位 ' + (slot.index + 1)));

        var badge = el('span', 'slot-badge');
        if (tone === 'is-empty') {
            badge.classList.add('is-empty');
            badge.appendChild(el('span', 'slot-badge-icon', '○'));
            badge.appendChild(el('span', null, '空槽位'));
        } else if (slot.healthy) {
            badge.classList.add('is-ok');
            badge.appendChild(el('span', 'slot-badge-icon', '✓'));
            badge.appendChild(el('span', null, '正常'));
        } else {
            badge.classList.add('is-bad');
            badge.appendChild(el('span', 'slot-badge-icon', '✕'));
            badge.appendChild(el('span', null, '存在异常'));
        }
        head.appendChild(badge);
        card.appendChild(head);

        if (tone !== 'is-empty') {
            var summary = el('div', 'slot-summary');
            summary.appendChild(el('span', 'slot-level', '第 ' + slot.level + ' 章'));
            summary.appendChild(el('span', 'slot-coins', slot.coins + ' 金币'));
            summary.appendChild(el('span', 'slot-time', slot.playtime));
            card.appendChild(summary);
        }
        card.appendChild(el('div', 'slot-last', '最近游玩：' + slot.last_played));

        card.addEventListener('click', function () { openSlot(slot.index); });
        card.addEventListener('keydown', function (event) {
            if (event.key === 'Enter' || event.key === ' ') {
                event.preventDefault();
                openSlot(slot.index);
            }
        });
        return card;
    }

    function slotList() {
        if (state.slots.length === 0) {
            var box = el('div', 'state');
            var mark = el('span', 'state-icon', '🗂');
            mark.setAttribute('aria-hidden', 'true');
            box.appendChild(mark);
            box.appendChild(el('span', null, '这个存档还没有任何槽位'));
            box.appendChild(el('div', 'state-hint', '用游戏本体创建并保存一次，然后再回到这里'));
            return box;
        }
        var list = el('div', 'slot-list');
        state.slots.forEach(function (slot) {
            list.appendChild(slotCard(slot));
        });
        return list;
    }

    // ---------------------------------------------------------------- 槽位详情

    function statRow(label, value) {
        var row = el('div', 'row');
        row.appendChild(el('span', 'row-label', label));
        row.appendChild(el('span', 'row-value', value));
        return row;
    }

    function section(title, bodyNode) {
        var sec = el('section', 'card');
        sec.appendChild(el('h2', 'card-title', title));
        sec.appendChild(bodyNode);
        return sec;
    }

    function detailView() {
        var slot = state.detail;
        var wrap = el('div');

        var head = el('div', 'detail-head');
        var back = el('button', 'btn btn-quiet btn-sm');
        back.type = 'button';
        back.appendChild(el('span', 'btn-text', '← 返回列表'));
        back.addEventListener('click', function () {
            state.phase = 'list';
            state.detail = null;
            render();
        });
        head.appendChild(back);
        head.appendChild(el('span', 'detail-index', '槽位 ' + slot.slot));
        var badge = el('span', 'slot-badge ' + (slot.healthy ? 'is-ok' : 'is-bad'));
        badge.appendChild(el('span', null, slot.healthy ? '正常' : '存在异常'));
        head.appendChild(badge);
        wrap.appendChild(head);

        if (slot.upgraded) {
            var up = el('div', 'inline-note is-info');
            up.appendChild(el('span', null,
                '本槽位由旧版本存档升级读取，你的原始文件没有被修改。'));
            wrap.appendChild(up);
        }

        // 进度
        var progress = el('div');
        progress.appendChild(statRow('当前关卡', '第 ' + slot.level + ' 章'));
        progress.appendChild(statRow('累计金币', String(slot.coins)));
        progress.appendChild(statRow('累计游玩', slot.playtime));
        progress.appendChild(statRow('最近游玩', slot.last_played));
        wrap.appendChild(section('进度', progress));

        // 道具：不可读字段**就地标注**，不整块隐藏（设计稿 slot-detail.md §2）
        var items = el('div', 'list');
        if (!slot.items || slot.items.length === 0) {
            items.appendChild(el('div', 'state-inline', '没有道具'));
        } else {
            slot.items.forEach(function (item) {
                var row = el('div', 'row');
                row.appendChild(el('span', 'row-label', item.name));
                row.appendChild(el('span', 'row-value', '× ' + item.count));
                items.appendChild(row);
            });
        }
        wrap.appendChild(section('道具（' + (slot.items ? slot.items.length : 0) + '）', items));

        // 成就
        var achs = el('div', 'list');
        if (!slot.achievements || slot.achievements.length === 0) {
            achs.appendChild(el('div', 'state-inline', '还没有解锁成就'));
        } else {
            var chips = el('div', 'chips');
            slot.achievements.forEach(function (id) {
                chips.appendChild(el('span', 'chip chip-ach', '成就 #' + id));
            });
            achs.appendChild(chips);
        }
        wrap.appendChild(section('已解锁成就（' + (slot.achievements ? slot.achievements.length : 0) + '）', achs));

        // 导出
        var actions = el('div', 'detail-actions');
        var jsonBtn = el('button', 'btn');
        jsonBtn.type = 'button';
        jsonBtn.appendChild(el('span', 'btn-text', '导出 JSON'));
        jsonBtn.addEventListener('click', function () { exportSlot(slot.slot - 1, 'json', jsonBtn); });
        actions.appendChild(jsonBtn);

        var textBtn = el('button', 'btn');
        textBtn.type = 'button';
        textBtn.appendChild(el('span', 'btn-text', '导出可读报告'));
        textBtn.addEventListener('click', function () { exportSlot(slot.slot - 1, 'text', textBtn); });
        actions.appendChild(textBtn);

        if (state.message) {
            actions.appendChild(el('span', 'detail-message', state.message));
        }
        wrap.appendChild(section('导出', actions));

        return wrap;
    }

    // ---------------------------------------------------------------- 诊断视图

    function diagnosisView() {
        var wrap = el('div');
        wrap.appendChild(el('div', 'diagnosis-lead',
            '这个存档有一些问题，但下面的建议可能帮到你'));

        state.diagnostics.forEach(function (diag) {
            wrap.appendChild(diagnosticNode(diag));
        });

        var actions = el('div', 'diagnosis-actions');
        var again = el('button', 'btn');
        again.type = 'button';
        again.appendChild(el('span', 'btn-text', '重新选择文件'));
        again.addEventListener('click', function () {
            state.phase = 'empty';
            state.diagnostics = [];
            render();
        });
        actions.appendChild(again);

        // 仅当还有可读槽位时才提供「仍要查看可读内容」
        if (state.slots && state.slots.length > 0) {
            var peek = el('button', 'btn btn-quiet');
            peek.type = 'button';
            peek.appendChild(el('span', 'btn-text', '仍要查看可读内容 →'));
            peek.addEventListener('click', function () {
                state.phase = 'list';
                render();
            });
            actions.appendChild(peek);
        }
        wrap.appendChild(actions);
        return wrap;
    }

    // ---------------------------------------------------------------- 行为

    function openFile(file) {
        if (state.busy) return;
        if (file.size > MAX_BYTES) {
            state.phase = 'empty';
            state.message = '这个存档文件太大，无法打开。';
            render();
            announce(state.message);
            return;
        }
        state.busy = true;
        state.phase = 'parsing';
        state.message = '';
        state.fileName = file.name;
        render();
        announce('正在读取 ' + file.name + '…');

        var controller = new AbortController();
        var timer = window.setTimeout(function () { controller.abort(); }, TIMEOUT_MS);

        // 以二进制上传：服务端不接收本机路径（契约 §5），只见字节。
        fetch(OPEN_URL, {
            method: 'POST',
            headers: { 'Content-Type': 'application/octet-stream' },
            body: file,
            cache: 'no-store',
            signal: controller.signal
        })
            .then(function (response) {
                return response.json().catch(function () { return null; }).then(function (payload) {
                    return { ok: response.ok, payload: payload };
                });
            })
            .then(function (result) {
                var payload = result.payload;
                if (payload && payload.ok === true && payload.save) {
                    state.save = payload.save;
                    state.diagnostics = payload.save.diagnostics || [];
                    announce('已读取 ' + file.name);
                    return refreshSlots();
                }
                // 业务失败：ok:false + diagnostics（契约 §4）
                state.diagnostics = (payload && payload.diagnostics) || [{
                    severity: 'error',
                    title: '无法读取这个文件',
                    detail: '服务端没有给出具体原因。',
                    advice: '请确认选择的是游戏的存档文件。'
                }];
                state.phase = 'diagnosis';
                announce('读取失败');
                return null;
            })
            .catch(function () {
                state.phase = 'empty';
                state.message = '无法连接到本机服务，请确认存档查看器正在运行。';
                announce(state.message);
            })
            .then(function () {
                window.clearTimeout(timer);
                state.busy = false;
                render();
            });
    }

    function refreshSlots() {
        return fetch(SLOTS_URL, { cache: 'no-store' })
            .then(function (response) { return response.json(); })
            .then(function (payload) {
                if (payload && payload.ok === true) {
                    state.slots = payload.slots || [];
                    // 有致命诊断且完全读不出槽位 → 诊断视图；否则列表
                    var fatal = state.diagnostics.some(function (d) { return d.severity === 'error'; });
                    state.phase = (fatal && state.slots.length === 0) ? 'diagnosis' : 'list';
                } else {
                    state.phase = 'diagnosis';
                }
            })
            .catch(function () { state.phase = 'diagnosis'; });
    }

    function openSlot(index) {
        if (state.busy) return;
        state.busy = true;
        state.phase = 'parsing';
        state.message = '';
        render();

        var controller = new AbortController();
        var timer = window.setTimeout(function () { controller.abort(); }, TIMEOUT_MS);

        fetch(SLOT_URL + '?index=' + encodeURIComponent(index), {
            cache: 'no-store',
            signal: controller.signal
        })
            .then(function (response) {
                return response.json().catch(function () { return null; });
            })
            .then(function (payload) {
                if (payload && payload.ok === true) {
                    state.detail = payload;
                    state.phase = 'detail';
                    announce('已打开槽位 ' + payload.slot);
                } else {
                    state.phase = 'list';
                    state.message = (payload && payload.message) || '打不开这个槽位。';
                    announce(state.message);
                }
            })
            .catch(function () {
                state.phase = 'list';
                state.message = '打不开这个槽位。';
                announce(state.message);
            })
            .then(function () {
                window.clearTimeout(timer);
                state.busy = false;
                render();
            });
    }

    // 导出走 fetch + blob（设计稿 slot-detail.md §3）：
    // 绝不能用 window.location.href —— 服务端失败时返回 JSON，
    // 直接跳转会让浏览器把 JSON 当正文渲染，把用户踢出整个界面。
    function exportSlot(index, format, button) {
        if (button) button.disabled = true;
        state.message = '正在导出…';
        render();

        fetch(EXPORT_URL + '?index=' + encodeURIComponent(index) +
              '&format=' + encodeURIComponent(format), { cache: 'no-store' })
            .then(function (response) {
                var type = response.headers.get('Content-Type') || '';
                if (!response.ok || type.indexOf('application/json') >= 0) {
                    return response.json().catch(function () { return null; }).then(function (payload) {
                        throw new Error((payload && payload.message) || '导出失败，请重试。');
                    });
                }
                return response.blob();
            })
            .then(function (blob) {
                var name = 'save-slot-' + (index + 1) + '.' + format;
                var url = URL.createObjectURL(blob);
                var anchor = document.createElement('a');
                anchor.href = url;
                anchor.download = name;  // 服务端 Content-Disposition 优先，此处为回退
                document.body.appendChild(anchor);
                anchor.click();
                document.body.removeChild(anchor);
                URL.revokeObjectURL(url);
                state.message = '已导出 ' + name;
                announce(state.message);
            })
            .catch(function (err) {
                state.message = err && err.message ? err.message : '导出失败，请重试。';
                announce(state.message);
            })
            .then(function () {
                if (button) button.disabled = false;
                render();
            });
    }

    // ---------------------------------------------------------------- 渲染

    function render() {
        openBtn.disabled = state.busy;
        stage.textContent = '';

        if (state.phase === 'empty') {
            stage.appendChild(emptyState());
            if (state.message) {
                stage.appendChild(el('div', 'global-error', state.message));
            }
            return;
        }
        if (state.phase === 'parsing') {
            stage.appendChild(skeletonList());
            return;
        }
        if (state.phase === 'detail' && state.detail) {
            stage.appendChild(detailView());
            return;
        }
        if (state.phase === 'diagnosis') {
            stage.appendChild(diagnosisView());
            return;
        }

        // 列表
        if (state.fileName) {
            stage.appendChild(fileBar(state.fileName, state.slots.length));
        }
        // 升级/警告类诊断嵌在列表顶部（设计稿 slot-list.md §2）
        state.diagnostics.forEach(function (diag) {
            if (diag.severity !== 'error') {
                stage.appendChild(diagnosticNode(diag));
            }
        });
        stage.appendChild(slotList());
    }

    openBtn.addEventListener('click', function () {
        state.phase = 'empty';
        state.message = '';
        render();
        // 触发首屏空态里的文件选择器
        var input = stage.querySelector('input[type=file]');
        if (input) input.click();
    });

    render();
})();

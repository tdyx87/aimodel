/* llm.c —— OpenAI 兼容协议适配
 * 契约：docs/design/contract.md §4.3
 *
 * 响应形状（流式）：
 *   data: {"choices":[{"index":0,"delta":{"content":"你"}}]}
 *   data: {"choices":[{"index":0,"delta":{"tool_calls":[
 *             {"index":0,"id":"call_a","function":{"name":"read_file",
 *              "arguments":"{\"pa"}}]}}]}
 *   data: {"choices":[{"index":0,"delta":{"tool_calls":[
 *             {"index":0,"function":{"arguments":"th\":\"a.c\"}"}}]}}]}
 *   data: [DONE]
 *
 * 工具参数分片到达，按 index 归并到对应槽位；到达结束标记时发 TOOL_CALL_END。
 */

#include "cagent/llm.h"

#include <stdlib.h>
#include <string.h>

#include "cagent/json.h"

/* ---------------------------------------------------------------- 槽位 */

static ca_llm_tool_slot *find_slot(ca_llm *llm, unsigned index) {
    for (int i = 0; i < CA_LLM_MAX_TOOL_CALLS; i++) {
        if (llm->tools[i].active && llm->tools[i].index == index) {
            return &llm->tools[i];
        }
    }
    return NULL;
}

/* 取已有槽位，或占用一个空槽位。
 * 返回 NULL 表示槽位已满——这是硬上限，不动态扩容（契约 §2.1）。 */
static ca_llm_tool_slot *acquire_slot(ca_llm *llm, unsigned index) {
    ca_llm_tool_slot *s = find_slot(llm, index);
    if (s != NULL) return s;
    for (int i = 0; i < CA_LLM_MAX_TOOL_CALLS; i++) {
        if (!llm->tools[i].active) {
            ca_llm_tool_slot *fresh = &llm->tools[i];
            memset(fresh, 0, sizeof(*fresh));
            fresh->active = 1;
            fresh->index = index;
            return fresh;
        }
    }
    return NULL;
}

static void release_all(ca_llm *llm) {
    for (int i = 0; i < CA_LLM_MAX_TOOL_CALLS; i++) {
        llm->tools[i].active = 0;
    }
}

/* ---------------------------------------------------------------- 事件 */

static void emit(ca_llm_on_event cb, void *user, const ca_event *ev) {
    if (cb != NULL) cb(ev, user);
}

static void emit_text(ca_llm_on_event cb, void *user, const char *text) {
    ca_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = CA_EV_TEXT_DELTA;
    ev.text = text;
    emit(cb, user, &ev);
}

static void emit_done(ca_llm_on_event cb, void *user) {
    ca_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = CA_EV_DONE;
    emit(cb, user, &ev);
}

/* ---------------------------------------------------------------- 解析 */

/* 处理 choices[0].delta.tool_calls 数组 */
static int handle_tool_calls(ca_llm *llm, ca_json_cursor *tc_arr,
                             ca_llm_on_event cb, void *user) {
    long count = ca_json_array_count(tc_arr);
    if (count < 0) return -1;
    if (count > CA_LLM_MAX_TOOL_CALLS) return -2;

    for (long i = 0; i < count; i++) {
        ca_json_cursor item, index_v, id_v, function_v, name_v, args_v;
        if (ca_json_index(tc_arr, (size_t)i, &item) != 1) return -1;
        if (ca_json_enter_object(&item) != 0) return -1;

        /* index 可选：缺省用数组位置（部分服务不返回 index） */
        unsigned index = (unsigned)i;
        if (ca_json_field(&item, "index", &index_v) == 1) {
            long long n = 0;
            if (ca_json_read_int(&index_v, &n) != 0) return -1;
            if (n < 0) return -1;
            index = (unsigned)n;
        }

        ca_llm_tool_slot *slot = acquire_slot(llm, index);
        if (slot == NULL) return -2; /* 槽位已满：硬上限，不扩容 */

        /* id 与 name 只在第一帧出现；超长即拒绝，不截断 */
        if (ca_json_field(&item, "id", &id_v) == 1) {
            char tmp[CA_LLM_MAX_ID];
            if (ca_json_read_string(&id_v, tmp, sizeof(tmp), NULL) != 0) return -2;
            memcpy(slot->id, tmp, sizeof(tmp));
        }
        /* 注意：同一个字段对象里查找多个 key 时，**每次都必须用独立的游标副本**。
 * 早先写成 ca_json_field(&v, "name", &v) 之后 v 已被 read_string 推进到
 * 下一个字段之前，再从那里找 "arguments" 会因为位置不是 '"' 或 '}'
 * 而返回 -1——表现为「参数一个都没存上」，且很容易被误判为测试数据问题。 */
        if (ca_json_field(&item, "function", &function_v) == 1) {
            if (ca_json_enter_object(&function_v) != 0) return -1;

            ca_json_cursor probe = function_v;
            if (ca_json_field(&probe, "name", &name_v) == 1) {
                char tmp[CA_LLM_MAX_NAME];
                if (ca_json_read_string(&name_v, tmp, sizeof(tmp), NULL) != 0) return -2;
                memcpy(slot->name, tmp, sizeof(tmp));
            }

            probe = function_v;
            if (ca_json_field(&probe, "arguments", &args_v) == 1) {
                /* 参数分片：直接流式追加进槽位缓冲。
                 * 早先经由 char tmp[1024] 中转，单片超过 1KB 就被
                 * 误判成超限——而一次 read_file 的参数轻易上千字节，
                 * 那会把本来合法的调用拒掉。 */
                size_t before = slot->args_len;
                int rc = ca_json_append_string(&args_v, slot->args,
                                               CA_LLM_MAX_ARGS_BYTES,
                                               &slot->args_len);
                if (rc == -2) {
                    /* 超过上限：标记并停止累积，**不截断**——
                     * 半截参数会被当成合法 JSON 执行。 */
                    slot->args_overflow = 1;
                } else if (rc != 0) {
                    slot->args_len = before;
                    return -1;
                }
            }
        }

        /* 首次拿到名字时发 BEGIN */
        if (!slot->announced && slot->name[0] != '\0') {
            slot->announced = 1;
            ca_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = CA_EV_TOOL_CALL_BEGIN;
            ev.tool_index = slot->index;
            ev.tool_id = slot->id;
            ev.tool_name = slot->name;
            emit(cb, user, &ev);
        }
    }
    return 0;
}

static int handle_delta(ca_llm *llm, ca_json_cursor *delta,
                        ca_llm_on_event cb, void *user) {
    if (ca_json_enter_object(delta) != 0) return -1;

    ca_json_cursor content_v, tools_v;
    /* content 与 tool_calls 各用独立游标：从 content 读完之后游标已推进，
     * 再从那里找 tool_calls 会因为位置不是 '"' 或 '}' 而失败。 */
    if (ca_json_field(delta, "content", &content_v) == 1) {
        if (ca_json_peek(&content_v) == '"') {
            /* 增量文本通常很短，但理论上可以很长；用大一点的栈缓冲 */
            char text[CA_LLM_MAX_TEXT * 8];
            int rc = ca_json_read_string(&content_v, text, sizeof(text), NULL);
            if (rc == -2) {
                /* 太长：不能丢。标记为超限让上层决定，而不是静默截断 */
                return -2;
            }
            if (rc != 0) return -1;
            if (text[0] != '\0') emit_text(cb, user, text);
        }
    }
    if (ca_json_field(delta, "tool_calls", &tools_v) == 1) {
        if (ca_json_enter_array(&tools_v) != 0) return -1;
        int rc = handle_tool_calls(llm, &tools_v, cb, user);
        if (rc != 0) return rc;
    }
    return 0;
}

int ca_llm_on_frame(ca_llm *llm, const char *data, size_t len,
                    ca_llm_on_event cb, void *user) {
    if (llm == NULL || data == NULL) return -1;
    if (llm->finished) return 0; /* [DONE] 之后再来的内容一律忽略 */

    /* [DONE] 结束标记。长度是 6 —— 早先误写成 9，导致 DONE 永远识别不到，
 * 于是上层永远等不到收尾，工具调用也不会发 END。 */
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        llm->finished = 1;
        /* 先把所有工具调用收尾，再发 DONE：
         * 上层可能在 END 里就开始执行工具，不能等 DONE 才拿到。 */
        for (int i = 0; i < CA_LLM_MAX_TOOL_CALLS; i++) {
            ca_llm_tool_slot *s = &llm->tools[i];
            if (!s->active || !s->announced) continue;
            ca_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = CA_EV_TOOL_CALL_END;
            ev.tool_index = s->index;
            ev.tool_id = s->id;
            ev.tool_name = s->name;
            emit(cb, user, &ev);
        }
        emit_done(cb, user);
        return 0;
    }

    ca_json_cursor root, v;
    ca_json_init(&root, data, len);
    /* 必须先进入对象：ca_json_field 的前提是游标已在容器内。
     * 早先在根游标上直接调它，错误响应永远识别不到——
     * 而错误识别失败会让「服务端说错了什么」变成一片空白。 */
    if (ca_json_enter_object(&root) != 0) return -1;

    /* 错误响应优先判断：{"error":{"message":"..."}} */
    if (ca_json_field(&root, "error", &v) == 1) {
        char msg[CA_LLM_MAX_TEXT * 2];
        int rc = ca_json_enter_object(&v);
        if (rc != 0) {
            ca_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = CA_EV_ERROR;
            ev.text = "服务端返回错误，但格式无法解析";
            emit(cb, user, &ev);
            return 0;
        }
        ca_json_cursor mv;
        rc = ca_json_field(&v, "message", &mv);
        if (rc == 1 && ca_json_read_string(&mv, msg, sizeof(msg), NULL) == 0) {
            ca_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = CA_EV_ERROR;
            ev.text = msg;
            emit(cb, user, &ev);
            return 0;
        }
        ca_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = CA_EV_ERROR;
        ev.text = "服务端返回错误，但未给出可读原因";
        emit(cb, user, &ev);
        return 0;
    }

    /* usage 通常在最后一帧出现 */
    if (ca_json_field(&root, "usage", &v) == 1) {
        ca_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = CA_EV_USAGE;
        ca_json_cursor u;
        if (ca_json_enter_object(&v) == 0) {
            long long n = 0;
            if (ca_json_field(&v, "prompt_tokens", &u) == 1 &&
                ca_json_read_int(&u, &n) == 0) {
                ev.prompt_tokens = n;
            }
            if (ca_json_field(&v, "completion_tokens", &u) == 1 &&
                ca_json_read_int(&u, &n) == 0) {
                ev.completion_tokens = n;
            }
        }
        emit(cb, user, &ev);
    }

    if (ca_json_field(&root, "choices", &v) == 1) {
        if (ca_json_enter_array(&v) != 0) return -1;
        ca_json_cursor choice;
        if (ca_json_index(&v, 0, &choice) == 1) {
            if (ca_json_enter_object(&choice) != 0) return -1;
            ca_json_cursor delta;
            if (ca_json_field(&choice, "delta", &delta) == 1) {
                int rc = handle_delta(llm, &delta, cb, user);
                if (rc != 0) return rc;
            }
        }
    }
    /* 走到这里：既没有 choices，也没有 error。
     * 那要么是一个**合法**的空帧（部分服务发 {} 做保活），
     * 要么是根本解析不了的内容。
     *
     * 早先这里无条件 return 0，于是 `{这不是 JSON` 这种垃圾帧被静默放过——
     * 用户看到的是「空回答 + 完成」，而实际上什么都没收到。
     * 静默失败比报错难查得多。
     *
     * 用一次完整的 skip_value 判定：能跳完 = 合法帧（当保活忽略），
     * 跳不完 = 坏帧，报错让上层知道。 */
    {
        ca_json_cursor probe;
        ca_json_init(&probe, data, len);
        if (ca_json_skip_value(&probe) != 0) return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 生命周期 */

ca_llm *ca_llm_create(void) {
    ca_llm *llm = (ca_llm *)calloc(1, sizeof(ca_llm));
    return llm;
}

void ca_llm_free(ca_llm *llm) {
    if (llm == NULL) return;
    release_all(llm);
    free(llm);
}

const char *ca_llm_tool_args(const ca_llm *llm, unsigned index, size_t *len,
                             int *overflow) {
    if (llm == NULL) return NULL;
    for (int i = 0; i < CA_LLM_MAX_TOOL_CALLS; i++) {
        if (llm->tools[i].active && llm->tools[i].index == index) {
            if (len != NULL) *len = llm->tools[i].args_len;
            if (overflow != NULL) *overflow = llm->tools[i].args_overflow;
            return llm->tools[i].args;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- 请求构造 */

/* json.c 现在是 JSON 语法的**唯一**出处：读与写都在那里。
 * 本文件此前的两份本地实现（write_json_string / append_raw）已删除——
 * 转义表复制一份，迟早会在某一边漏掉某个字符，
 * 而漏掉的后果是产出坏 JSON，且只在特定输入下才出现。 */
static int write_json_string(char *buf, size_t cap, size_t *len, const char *s) {
    return ca_json_write_string(buf, cap, len, s);
}

static int append_raw(char *buf, size_t cap, size_t *len, const char *s) {
    return ca_json_write_raw(buf, cap, len, s);
}

static const char *role_name(ca_msg_role r) {
    switch (r) {
        case CA_MSG_SYSTEM: return "system";
        case CA_MSG_ASSISTANT: return "assistant";
        case CA_MSG_USER:
        default: return "user";
    }
}

int ca_llm_build_request_full(char *buf, size_t cap,
                              const char *model, const char *system,
                              const ca_llm_msg *msgs, size_t msg_count,
                              const char *tools_json, size_t *need_len) {
    if (model == NULL) return -1;
    if (msgs == NULL && msg_count != 0) return -1;
    for (size_t i = 0; i < msg_count; i++) {
        if (msgs[i].text == NULL) return -1;
    }
    if (buf == NULL && cap == 0) return -1; /* 需要能算出长度 */

    /* 两遍构造：第一遍只计量，装不下就**完全不碰缓冲区**。
     * 留半截请求体比失败更糟——服务端会把它当成协议错误，
     * 报出来的信息离真正的原因很远。 */
    for (int pass = 0; pass < 2; pass++) {
        char *dst = (pass == 0) ? NULL : buf;
        size_t len = 0;
        int bad = 0;

        if (append_raw(dst, cap, &len, "{\"model\":") != 0) bad = 1;
        if (!bad && write_json_string(dst, cap, &len, model) != 0) bad = 1;
        if (!bad && append_raw(dst, cap, &len, ",\"stream\":true,\"messages\":[") != 0) bad = 1;
        if (!bad && system != NULL && system[0] != '\0') {
            if (append_raw(dst, cap, &len, "{\"role\":\"system\",\"content\":") != 0) bad = 1;
            if (!bad && write_json_string(dst, cap, &len, system) != 0) bad = 1;
            if (!bad && append_raw(dst, cap, &len, "},") != 0) bad = 1;
        }
        for (size_t i = 0; !bad && i < msg_count; i++) {
            if (i > 0 && append_raw(dst, cap, &len, ",") != 0) { bad = 1; break; }
            if (append_raw(dst, cap, &len, "{\"role\":\"") != 0) { bad = 1; break; }
            if (append_raw(dst, cap, &len, role_name(msgs[i].role)) != 0) { bad = 1; break; }
            if (append_raw(dst, cap, &len, "\",\"content\":") != 0) { bad = 1; break; }
            if (write_json_string(dst, cap, &len, msgs[i].text) != 0) { bad = 1; break; }
            if (append_raw(dst, cap, &len, "}") != 0) { bad = 1; break; }
        }
        /* 先关 messages 数组，再挂 tools，最后关根对象。
         * 顺序错了就是 {...,"tools":[...]]}：服务端直接 400，而界面上
         * 只看到「请求失败」（错误体为空）。真实故障现场。 */
        if (!bad && append_raw(dst, cap, &len, "]") != 0) bad = 1;
        /* 工具声明必须**真的发出去**。早先请求体里没有这个字段，
         * 于是真实服务永远只会聊天：脚本化的冒烟伪造 tool_calls 帧，
         * 看着全绿，实际模型根本没被给过工具。 */
        if (!bad && tools_json != NULL && tools_json[0] != '\0') {
            if (append_raw(dst, cap, &len, ",\"tools\":") != 0) bad = 1;
            if (!bad && append_raw(dst, cap, &len, tools_json) != 0) bad = 1;
        }
        if (!bad && append_raw(dst, cap, &len, "}") != 0) bad = 1;

        if (pass == 0) {
            if (need_len != NULL) *need_len = len + 1; /* 含 NUL */
            if (len + 1 > cap) return -2;
        } else {
            if (bad) return -2; /* 第二遍仍写不下：与第一遍同因，按容量不足报 */
            buf[len] = '\0';
        }
    }
    return 0;
}

int ca_llm_build_request_msgs(char *buf, size_t cap,
                              const char *model, const char *system,
                              const ca_llm_msg *msgs, size_t msg_count,
                              size_t *need_len) {
    return ca_llm_build_request_full(buf, cap, model, system, msgs, msg_count,
                                     NULL, need_len);
}

int ca_llm_build_request(char *buf, size_t cap, const ca_llm_request *req,
                         size_t *need_len) {
    if (req == NULL) return -1;
    if (req->api_base == NULL || req->model == NULL || req->user == NULL) return -1;

    /* 单轮请求 = 历史为空、只有一条 user 的多轮请求。
     * 保留这个函数是为了让既有调用点与测试不必知道历史的存在。 */
    ca_llm_msg only;
    only.role = CA_MSG_USER;
    only.text = req->user;
    return ca_llm_build_request_msgs(buf, cap, req->model, req->system,
                                     &only, 1, need_len);
}
/* web_page.c —— Web UI 的页面（内嵌在二进制里）
 *
 * 为什么内嵌：单文件交付是这个项目的风格（一个 exe 就是全部），
 * 页面不依赖磁盘上的额外文件，也就没有「路径不对就白屏」的问题。
 *
 * 页面本身不依赖任何前端框架：原生 fetch + EventSource。
 * 状态：
 *   - 令牌从 URL 的 ?t= 取，之后放进 X-Cagent-Token 头
 *   - EventSource 订阅 /api/events（带 token 的 query，因为 EventSource
 *     不能自定义请求头，所以这里用 query 传 token，服务端两种都收）
 *   - 批准请求用按钮 → POST /api/approve
 */

#include <stddef.h>

/* 页面以**片段数组**给出，而不是一个巨大的字符串字面量。
 *
 * ISO C99 只要求编译器支持长度不超过 4095 字符的字符串字面量；
 * 页面有 5000 多字符，GCC 会报 -Woverlength-strings，
 * 而 CI 把告警当失败（这正是「零告警」门禁的用意）。
 * 分成多片从根上绕开限制，也方便以后单独替换某一段。 */
const char *const *ca_web_page_chunks(size_t *count) {
    static const char *const chunks[] = {
    "<!DOCTYPE html>\n",
    "<html lang=\"zh-CN\">\n",
    "<head>\n",
    "<meta charset=\"utf-8\">\n",
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n",
    "<title>cagent</title>\n",
    "<style>\n",
    ":root{--bg:#0f1115;--panel:#171a21;--line:#262b36;--fg:#e6e8ee;--dim:#8b93a7;\n",
    "      --accent:#6ea8fe;--ok:#57c98a;--warn:#e8c468;--err:#e8697d;}\n",
    "*{box-sizing:border-box}\n",
    "body{margin:0;background:var(--bg);color:var(--fg);\n",
    "     font:15px/1.6 -apple-system,Segoe UI,Microsoft YaHei,sans-serif}\n",
    "header{display:flex;align-items:center;gap:12px;padding:10px 18px;\n",
    "       border-bottom:1px solid var(--line);background:var(--panel)}\n",
    "header b{font-size:16px}\n",
    "#info{color:var(--dim);font-size:13px}\n",
    "#status{margin-left:auto;font-size:13px;color:var(--dim)}\n",
    "#log{padding:18px;max-width:900px;margin:0 auto}\n",
    ".msg{padding:10px 14px;border-radius:10px;margin:0 0 12px;white-space:pre-wrap;\n",
    "     word-break:break-word;border:1px solid var(--line)}\n",
    ".user{background:#1d2430;border-color:#2c3646}\n",
    ".assistant{background:var(--panel)}\n",
    ".err{border-color:#5a2b34;color:#ffd7dd;background:#241419}\n",
    ".tool{border-color:#2a3342;background:#141922}\n",
    ".tool .t{color:var(--accent);font-weight:600;font-size:13px}\n",
    ".tool pre{margin:6px 0 0;white-space:pre-wrap;word-break:break-word;\n",
    "          color:var(--dim);font:13px/1.5 ui-monospace,Consolas,monospace}\n",
    ".approve{margin-top:10px;display:flex;gap:8px;align-items:center}\n",
    "button{font:inherit;border-radius:8px;border:1px solid var(--line);\n",
    "       background:#222936;color:var(--fg);padding:6px 14px;cursor:pointer}\n",
    "button:hover{background:#2a3242}\n",
    "button.allow{border-color:#2f6b4a;color:var(--ok)}\n",
    "button.deny{border-color:#6b3040;color:var(--err)}\n",
    "#bar{position:sticky;bottom:0;border-top:1px solid var(--line);\n",
    "     background:var(--panel);padding:12px 18px}\n",
    "#form{max-width:900px;margin:0 auto;display:flex;gap:10px}\n",
    "textarea{flex:1;resize:vertical;min-height:52px;max-height:220px;\n",
    "         background:#11151c;color:var(--fg);border:1px solid var(--line);\n",
    "         border-radius:10px;padding:10px 12px;font:inherit}\n",
    "textarea:focus{outline:1px solid var(--accent)}\n",
    "#send{align-self:flex-end}\n",
    ".hint{color:var(--dim);font-size:12px;margin:6px 2px 0}\n",
    "</style>\n",
    "</head>\n",
    "<body>\n",
    "<header><b>cagent</b><span id=\"info\">连接中…</span>\n",
    "<span id=\"status\"></span></header>\n",
    "<div id=\"log\"></div>\n",
    "<div id=\"bar\">\n",
    "  <form id=\"form\">\n",
    "    <textarea id=\"input\" placeholder=\"说点什么…（Enter 发送，Shift+Enter 换行）\"></textarea>\n",
    "    <button id=\"send\" type=\"submit\">发送</button>\n",
    "  </form>\n",
    "  <div class=\"hint\">写文件/执行命令前会出现批准按钮；只有点「允许」才会执行。</div>\n",
    "</div>\n",
    "<script>\n",
    "(function(){\n",
    "  var q=new URLSearchParams(location.search);\n",
    "  var token=q.get('t')||'';\n",
    "  var log=document.getElementById('log');\n",
    "  var statusEl=document.getElementById('status');\n",
    "  var infoEl=document.getElementById('info');\n",
    "  var form=document.getElementById('form');\n",
    "  var input=document.getElementById('input');\n",
    "  function el(tag,cls,text){var d=document.createElement(tag);\n",
    "    if(cls)d.className=cls; if(text!=null)d.textContent=text; return d;}\n",
    "  function add(cls,text){var d=el('div',cls,text); log.appendChild(d);\n",
    "    d.scrollIntoView({block:'end'}); return d;}\n",
    "  function post(path,obj){return fetch(path,{method:'POST',\n",
    "    headers:{'Content-Type':'application/json','X-Cagent-Token':token},\n",
    "    body:JSON.stringify(obj||{})}).then(function(r){return r.ok;});}\n",
    "  var es=new EventSource('/api/events?t='+encodeURIComponent(token));\n",
    "  es.onopen=function(){infoEl.textContent='已连接';};\n",
    "  es.onerror=function(){infoEl.textContent='连接断开，正在重连…';};\n",
    "  es.addEventListener('text',function(e){var d=JSON.parse(e.data);\n",
    "    if(d.kind==='text'){ if(d.text) add('assistant',d.text); } });\n",
    "  es.addEventListener('tool',function(e){var d=JSON.parse(e.data);\n",
    "    var box=add('tool',null);\n",
    "    var t=el('div','t','[工具] '+(d.name||'')+(d.status?'  ·  '+d.status:''));\n",
    "    box.appendChild(t);\n",
    "    if(d.text){var pre=el('pre',null,d.text); box.appendChild(pre);} });\n",
    "  es.addEventListener('approval',function(e){var d=JSON.parse(e.data);\n",
    "    var box=add('tool',null);\n",
    "    box.appendChild(el('div','t','[需要批准] '+(d.name||'')));\n",
    "    if(d.text) box.appendChild(el('pre',null,d.text));\n",
    "    var row=el('div','approve');\n",
    "    var ok=el('button','allow','允许');\n",
    "    var no=el('button','deny','拒绝');\n",
    "    ok.onclick=function(){post('/api/approve',{decision:'allow'});\n",
    "      row.textContent='已允许';};\n",
    "    no.onclick=function(){post('/api/approve',{decision:'deny'});\n",
    "      row.textContent='已拒绝';};\n",
    "    row.appendChild(ok); row.appendChild(no); box.appendChild(row); });\n",
    "  es.addEventListener('error',function(e){try{var d=JSON.parse(e.data);\n",
    "    add('err',d.text||'出错了');}catch(x){add('err','出错了');}});\n",
    "  es.addEventListener('done',function(){statusEl.textContent='';});\n",
    "  form.onsubmit=function(ev){ev.preventDefault();\n",
    "    var text=input.value.trim(); if(!text) return;\n",
    "    add('user',text); input.value='';\n",
    "    post('/api/send',{text:text}).then(function(ok){\n",
    "      if(!ok) add('err','发送失败（会话已停止？）');});};\n",
    "  input.addEventListener('keydown',function(ev){\n",
    "    if(ev.key==='Enter'&&!ev.shiftKey){ev.preventDefault(); form.requestSubmit();}});\n",
    "  fetch('/api/info?t='+encodeURIComponent(token),{headers:{'X-Cagent-Token':token}})\n",
    "    .then(function(r){return r.ok?'已连接':'令牌无效';})\n",
    "    .then(function(t){if(t!=='已连接'){infoEl.textContent=t; statusEl.textContent=t;}});\n",
    "})();\n",
    "</script>\n",
    "</body>\n",
    "</html>\n",
    };
    if (count != NULL) *count = sizeof(chunks) / sizeof(chunks[0]);
    return chunks;
}

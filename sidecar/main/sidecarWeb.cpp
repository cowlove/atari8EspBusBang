// The Sidecar's web pages: see sidecarWeb.h.
//
// A minimal file manager: "/" is one page whose script drives a small API,
// on the Sidecar's own storage (/sd, /spiffs) and main's (main:/spiffs,
// over the link, in diagnostic mode only: sidecarMainFiles.h).
//   GET  /files/status            {"main": true|false, "why": "..."}
//   GET  /files/ls?p=<spec>       [{"n": name, "s": size (-1: a directory)}, ...]
//   GET  /files/get?p=<spec>      the file (a download)
//   POST /files/put?p=<spec>      the body becomes the file (<spec>.part, then renamed);
//                                 {"size": n, "sha256": hex} of what was received
//   GET  /console                 the web console (a page)
//   GET  /console/out?since=<n>&wait=<ms>
//                                 the console's output from byte offset n
//                                 (text/plain; headers X-Console-Next: the
//                                 offset to ask for next, X-Console-Lost:
//                                 bytes already overwritten); no since: the
//                                 last 4 KB; wait (<= 5000 ms, not in csim):
//                                 hold the request until there is some
//                                 (it holds the whole server, one request at
//                                 a time: for a lone client, e.g. a script;
//                                 the page polls without it)
//   POST /console/cmd             the body's lines (or ?c=<line>) run as
//                                 console commands, as if typed;
//                                 {"queued": n, "at": offset} (output from
//                                 "at" on)
//   POST /files/rm?p=<spec>       delete it
//   GET  /files/hash?p=<spec>     {"sha256": hex, "size": n}
// A spec is a VFS path ("/sd/d1.atr") or "main:" + one of main's
// ("main:/spiffs/d1.atr.gz"). Errors: a 4xx/5xx with a one-line reason.
//
// Threads: on the board the web server has its own task; local file work
// runs there, main's goes to the service task (sidecarRunInServiceLoop()),
// holding main's file channel for the whole transfer (mainFiles::claim()).
// csim polls the server from the service loop, so everything runs there.

#include "sidecarWeb.h"
#include "sidecarMainFiles.h"
#include "sidecarApp.h"   // the console mirror and command queue
#include "sidecar_sha256.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#ifndef CSIM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace {

uint32_t requests = 0;

// ---- helpers ----

// A query parameter, URL-decoded ("" if absent).
std::string queryParam(httpd_req_t *req, const char *key)
{
    const size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) return "";
    std::string query(len + 1, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return "";
    std::string raw(len + 1, '\0');
    if (httpd_query_key_value(query.c_str(), key, raw.data(), raw.size()) != ESP_OK) return "";
    raw.resize(strlen(raw.c_str()));
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size() + 0 && isxdigit((unsigned char)raw[i + 1]) &&
            isxdigit((unsigned char)raw[i + 2])) {
            out += static_cast<char>(strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += raw[i] == '+' ? ' ' : raw[i];
        }
    }
    return out;
}

std::string jsonString(const std::string &s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            out += esc;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

esp_err_t sendJson(httpd_req_t *req, const std::string &json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t fail(httpd_req_t *req, const char *status, const std::string &why)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, why.c_str());
}

struct Spec {
    bool main = false;
    std::string path;   // main's path, or the local one (csim: under the web root)
    bool ok = false;
};

Spec parseSpec(httpd_req_t *req)
{
    Spec spec;
    std::string p = queryParam(req, "p");
    if (p.rfind("main:", 0) == 0) {
        spec.main = true;
        p = p.substr(5);
    } else if (p.rfind("sidecar:", 0) == 0) {
        p = p.substr(8);
    }
    if (p.empty() || p[0] != '/' || p.find("/../") != std::string::npos ||
        (p.size() >= 3 && p.compare(p.size() - 3, 3, "/..") == 0)) {
        return spec;
    }
#ifdef CSIM
    // csim: the Sidecar's storage is a host directory (CSIM_WEB_ROOT, default
    // ./webfs, with sd/ and spiffs/ in it).
    if (!spec.main) {
        const char *root = getenv("CSIM_WEB_ROOT");
        p = std::string(root ? root : "./webfs") + p;
    }
#endif
    spec.path = p;
    spec.ok = true;
    return spec;
}

const char *baseName(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

// Main's file channel for one request: available, then claimed; released
// on scope exit.
struct MainClaim {
    bool held = false;
    std::string why;
    MainClaim()
    {
        bool up = false;
        sidecarRunInServiceLoop([&] { up = mainFiles::available(&why); });
        if (!up) return;
        held = mainFiles::claim();
        if (!held) why = "main's files are busy (another transfer)";
    }
    ~MainClaim()
    {
        if (held) mainFiles::release();
    }
};

// ---- pages ----

const char kIndexPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sidecar files</title>
<style>
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0066cc;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mut:#a1a1a6;--line:#3a3a3c;--acc:#4da3ff;--bad:#ff6b6b}}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0 auto;max-width:860px;padding:16px}
h1{font-size:20px;margin:0 0 12px}
nav button,button{font:inherit;padding:4px 10px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);cursor:pointer}
nav button.on{border-color:var(--acc);color:var(--acc)}
nav button:disabled,button:disabled{opacity:.4;cursor:default}
table{width:100%;border-collapse:collapse;margin:12px 0}
td{padding:6px 4px;border-bottom:1px solid var(--line);vertical-align:middle}
td.s{text-align:right;color:var(--mut);font-variant-numeric:tabular-nums;white-space:nowrap}
td.a{text-align:right;white-space:nowrap}
a{color:var(--acc);text-decoration:none}
#path{color:var(--mut);margin:8px 0;word-break:break-all}
#msg{min-height:1.4em;color:var(--mut);word-break:break-all}
#msg.bad{color:var(--bad)}
.up{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
</style></head><body>
<h1>Sidecar files</h1>
<p><a href="/console">Console</a></p>
<nav id="roots"></nav>
<div id="path"></div>
<div class="up"><input type="file" id="file"><button id="upb">Upload here</button></div>
<table id="list"></table>
<div id="msg"></div>
<script>
const roots=[["Sidecar SD","/sd"],["Sidecar flash","/spiffs"],["Main flash","main:/spiffs"]];
let cur="/sd", mainOk=false;
const $=id=>document.getElementById(id);
function msg(t,bad){const m=$("msg");m.textContent=t||"";m.className=bad?"bad":""}
function q(p){return "?p="+encodeURIComponent(p)}
function join(d,n){return d.endsWith("/")?d+n:d+"/"+n}
function size(s){return s<0?"":s<1024?s+" B":s<1048576?(s/1024).toFixed(1)+" KB":(s/1048576).toFixed(2)+" MB"}
async function call(url,opt){const r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text()||r.statusText);return r}
function drawRoots(){const n=$("roots");n.innerHTML="";for(const [label,p] of roots){const b=document.createElement("button");
 b.textContent=label;b.className=cur.startsWith(p)?"on":"";if(p.startsWith("main:")&&!mainOk){b.disabled=true;b.title=mainWhy}
 b.onclick=()=>go(p);n.appendChild(b);n.append(" ")}}
let mainWhy="";
async function go(p){cur=p;drawRoots();$("path").textContent=p;const t=$("list");t.innerHTML="";msg("Loading…");
 try{const list=await (await call("/files/ls"+q(p))).json();list.sort((a,b)=>(b.s<0)-(a.s<0)||a.n.localeCompare(b.n));
  const top=roots.find(r=>p===r[1]);if(!top){const up=p.slice(0,p.lastIndexOf("/"))||"/";row("..",-1,()=>go(up),true)}
  for(const e of list){if(e.n==="."||e.n==="..")continue;const f=join(p,e.n);e.s<0?row(e.n+"/",-1,()=>go(f),true):row(e.n,e.s,null,false,f)}
  msg(list.length+" entries")}catch(e){msg(e.message,true)}}
function row(name,s,open,dir,f){const tr=$("list").insertRow();const a=tr.insertCell();
 if(open){const l=document.createElement("a");l.href="#";l.textContent=name;l.onclick=ev=>{ev.preventDefault();open()};a.appendChild(l)}
 else{const l=document.createElement("a");l.href="/files/get"+q(f);l.textContent=name;l.download=name;a.appendChild(l)}
 tr.insertCell().textContent=size(s);tr.cells[1].className="s";const c=tr.insertCell();c.className="a";if(dir)return;
 const h=document.createElement("button");h.textContent="Hash";h.onclick=async()=>{msg("Hashing "+name+"…");
  try{const r=await (await call("/files/hash"+q(f))).json();msg(name+": sha256 "+r.sha256+", "+r.size+" bytes")}catch(e){msg(e.message,true)}};
 const d=document.createElement("button");d.textContent="Delete";d.onclick=async()=>{if(!confirm("Delete "+f+"?"))return;
  try{await call("/files/rm"+q(f),{method:"POST"});go(cur)}catch(e){msg(e.message,true)}};c.append(h," ",d)}
$("upb").onclick=async()=>{const file=$("file").files[0];if(!file){msg("Choose a file first",true);return}
 const f=join(cur,file.name);msg("Uploading "+file.name+" ("+size(file.size)+")…");const t0=Date.now();
 try{await call("/files/put"+q(f),{method:"POST",body:file});msg("Uploaded "+f+" in "+((Date.now()-t0)/1000).toFixed(1)+" s");go(cur)}
 catch(e){msg(e.message,true)}};
(async()=>{try{const s=await (await call("/files/status")).json();mainOk=s.main;mainWhy=s.why||""}catch(e){}go(cur)})();
</script></body></html>
)HTML";

esp_err_t indexHandler(httpd_req_t *req)
{
    ++requests;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kIndexPage, sizeof(kIndexPage) - 1);
}

// "/hello": plain text (main's old page).
esp_err_t helloHandler(httpd_req_t *req)
{
    ++requests;
    char page[160];
    snprintf(page, sizeof(page), "Hello World! Atari8 Sidecar, up %lld s, %u requests\n",
             (long long)(esp_timer_get_time() / 1000000), (unsigned)requests);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t statusHandler(httpd_req_t *req)
{
    ++requests;
    std::string why;
    bool up = false;
    sidecarRunInServiceLoop([&] { up = mainFiles::available(&why); });
    return sendJson(req, std::string("{\"main\":") + (up ? "true" : "false") + ",\"why\":" + jsonString(why) + "}");
}

esp_err_t lsHandler(httpd_req_t *req)
{
    ++requests;
    const Spec spec = parseSpec(req);
    if (!spec.ok) return fail(req, "400 Bad Request", "bad path");
    std::vector<mainFiles::Entry> entries;
    if (spec.main) {
        MainClaim claim;
        if (!claim.held) return fail(req, "503 Service Unavailable", claim.why);
        bool ok = false;
        std::string why;
        sidecarRunInServiceLoop([&] { ok = mainFiles::list(spec.path.c_str(), &entries, &why); });
        if (!ok) return fail(req, "500 Internal Server Error", why);
    } else {
        DIR *dir = opendir(spec.path.c_str());
        if (!dir) return fail(req, "404 Not Found", strerror(errno));
        while (const dirent *e = readdir(dir)) {
            const std::string full = spec.path + (spec.path.back() == '/' ? "" : "/") + e->d_name;
            struct stat st;
            const bool ok = stat(full.c_str(), &st) == 0;
            entries.push_back({e->d_name, ok && S_ISDIR(st.st_mode) ? -1L : ok ? (long)st.st_size : 0L});
        }
        closedir(dir);
    }
    std::string json = "[";
    for (const mainFiles::Entry &e : entries) {
        if (json.size() > 1) json += ",";
        json += "{\"n\":" + jsonString(e.name) + ",\"s\":" + std::to_string(e.size) + "}";
    }
    return sendJson(req, json + "]");
}

esp_err_t getHandler(httpd_req_t *req)
{
    ++requests;
    const Spec spec = parseSpec(req);
    if (!spec.ok) return fail(req, "400 Bad Request", "bad path");
    const std::string disposition = std::string("attachment; filename=\"") + baseName(spec.path) + "\"";
    constexpr size_t kBuf = 4096;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kBuf]);
    if (!buf) return fail(req, "500 Internal Server Error", "no memory");
    if (!spec.main) {
        FILE *f = fopen(spec.path.c_str(), "rb");
        if (!f) return fail(req, "404 Not Found", strerror(errno));
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
        size_t n;
        esp_err_t err = ESP_OK;
        while (err == ESP_OK && (n = fread(buf.get(), 1, kBuf, f)) > 0) {
            err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf.get()), n);
        }
        fclose(f);
        return err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : err;
    }
    MainClaim claim;
    if (!claim.held) return fail(req, "503 Service Unavailable", claim.why);
    bool ok = false;
    std::string why;
    uint32_t size = 0;
    sidecarRunInServiceLoop([&] { ok = mainFiles::open(spec.path.c_str(), SidecarFileMode::Read, &size, &why); });
    if (!ok) return fail(req, "404 Not Found", why);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
    uint32_t offset = 0;
    esp_err_t err = ESP_OK;
    bool done = false;
    while (err == ESP_OK && !done) {
        // A buffer's worth of chunks per hand-off to the service task.
        uint32_t fill = 0;
        sidecarRunInServiceLoop([&] {
            while (fill + mainFiles::kReadChunkBytes <= kBuf) {
                uint32_t n = 0;
                if (!(ok = mainFiles::read(offset + fill, buf.get() + fill, &n, &why))) return;
                if (n == 0) {
                    done = true;
                    return;
                }
                fill += n;
            }
        });
        if (!ok) break;   // the client sees a short file: its size won't match
        if (fill) err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf.get()), fill);
        offset += fill;
    }
    sidecarRunInServiceLoop([&] { mainFiles::close(false, nullptr); });
    if (err != ESP_OK) return err;
    if (!ok) return httpd_resp_send_chunk(req, nullptr, 0);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t putHandler(httpd_req_t *req)
{
    ++requests;
    const Spec spec = parseSpec(req);
    if (!spec.ok) return fail(req, "400 Bad Request", "bad path");
    constexpr size_t kBuf = 8 * mainFiles::kWriteChunkBytes;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kBuf]);
    if (!buf) return fail(req, "500 Internal Server Error", "no memory");
    std::unique_ptr<MainClaim> claim;
    FILE *out = nullptr;
    const std::string part = spec.path + ".part";
    bool ok = true;
    std::string why;
    if (spec.main) {
        claim.reset(new MainClaim);
        if (!claim->held) return fail(req, "503 Service Unavailable", claim->why);
        uint32_t ignored = 0;
        sidecarRunInServiceLoop([&] { ok = mainFiles::open(spec.path.c_str(), SidecarFileMode::Write, &ignored, &why); });
        if (!ok) return fail(req, "500 Internal Server Error", why);
    } else {
        out = fopen(part.c_str(), "wb");
        if (!out) return fail(req, "500 Internal Server Error", std::string("can't create: ") + strerror(errno));
    }
    size_t left = req->content_len;
    uint32_t offset = 0;
    int timeouts = 0;
    SidecarSha256 sha;   // of what was received, for the reply
    while (ok && left > 0) {
        const int n = httpd_req_recv(req, reinterpret_cast<char *>(buf.get()), left < kBuf ? left : kBuf);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            ok = false;
            why = "upload interrupted";
            break;
        }
        timeouts = 0;
        sha.update(buf.get(), n);
        if (spec.main) {
            sidecarRunInServiceLoop([&] {
                for (int at = 0; ok && at < n; at += mainFiles::kWriteChunkBytes) {
                    const uint32_t len = std::min<uint32_t>(mainFiles::kWriteChunkBytes, n - at);
                    ok = mainFiles::write(offset + at, buf.get() + at, len, &why);
                }
            });
        } else if (fwrite(buf.get(), 1, n, out) != static_cast<size_t>(n)) {
            ok = false;
            why = std::string("write failed: ") + strerror(errno);
        }
        offset += n;
        left -= n;
    }
    if (spec.main) {
        std::string closeWhy;
        bool closed = false;
        sidecarRunInServiceLoop([&] { closed = mainFiles::close(ok, &closeWhy); });
        if (ok && !closed) {
            ok = false;
            why = closeWhy;
        }
    } else {
        if (fclose(out) != 0 && ok) {
            ok = false;
            why = strerror(errno);
        }
        if (ok) {
            unlink(spec.path.c_str());
            if (rename(part.c_str(), spec.path.c_str()) != 0) {
                ok = false;
                why = std::string("rename failed: ") + strerror(errno);
            }
        }
        if (!ok) unlink(part.c_str());
    }
    if (!ok) return fail(req, "500 Internal Server Error", why);
    uint8_t digest[32];
    sha.finish(digest);
    char hex[65];
    sidecarHexDigest(digest, hex);
    return sendJson(req, "{\"size\":" + std::to_string(offset) + ",\"sha256\":\"" + hex + "\"}");
}

// ---- the web console ----

const char kConsolePage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sidecar console</title>
<style>
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0066cc;--bad:#c62828;--out:#f5f5f7}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mut:#a1a1a6;--line:#3a3a3c;--acc:#4da3ff;--bad:#ff6b6b;--out:#000}}
html,body{height:100%}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0 auto;max-width:1100px;padding:16px;box-sizing:border-box;display:flex;flex-direction:column;gap:8px}
h1{font-size:20px;margin:0}
a{color:var(--acc);text-decoration:none}
#out{flex:1;min-height:200px;overflow:auto;margin:0;padding:8px;background:var(--out);border:1px solid var(--line);border-radius:6px;font:12px/1.35 ui-monospace,Menlo,monospace;white-space:pre-wrap;word-break:break-all}
form{display:flex;gap:8px}
input{flex:1;font:14px ui-monospace,Menlo,monospace;padding:6px 8px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg)}
button{font:inherit;padding:4px 12px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);cursor:pointer}
#msg{color:var(--mut);min-height:1.4em}#msg.bad{color:var(--bad)}
</style></head><body>
<h1>Sidecar console <small><a href="/">files</a></small></h1>
<pre id="out"></pre>
<form id="f"><input id="c" autocomplete="off" autofocus placeholder="command (help)"><button>Send</button></form>
<div id="msg"></div>
<script>
const $=id=>document.getElementById(id),out=$("out");let next=null;const hist=[];let hi=0;
function msg(t,bad){$("msg").textContent=t;$("msg").className=bad?"bad":""}
function add(t){const end=out.scrollTop+out.clientHeight>=out.scrollHeight-4;out.textContent+=t;
 if(out.textContent.length>400000)out.textContent=out.textContent.slice(-300000);if(end)out.scrollTop=out.scrollHeight}
async function poll(){for(;;){try{const r=await fetch("/console/out"+(next===null?"":"?since="+next));
  if(!r.ok)throw new Error(await r.text());const lost=+r.headers.get("X-Console-Lost");if(lost)add("\n[... "+lost+" bytes lost]\n");
  const t=await r.text();add(t);next=r.headers.get("X-Console-Next");msg("");if(!t)await new Promise(r=>setTimeout(r,300))}catch(e){msg("Disconnected: "+e.message,true);await new Promise(r=>setTimeout(r,2000))}}}
$("f").onsubmit=async ev=>{ev.preventDefault();const c=$("c").value.trim();if(!c)return;hist.push(c);hi=hist.length;$("c").value="";
 try{const r=await fetch("/console/cmd",{method:"POST",body:c});if(!r.ok)throw new Error(await r.text())}catch(e){msg(e.message,true)}};
$("c").onkeydown=ev=>{if(ev.key==="ArrowUp"&&hi>0){$("c").value=hist[--hi];ev.preventDefault()}
 else if(ev.key==="ArrowDown"&&hi<hist.length){hi++;$("c").value=hist[hi]||"";ev.preventDefault()}};
poll();
</script></body></html>
)HTML";

esp_err_t consolePageHandler(httpd_req_t *req)
{
    ++requests;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kConsolePage, sizeof(kConsolePage) - 1);
}

esp_err_t consoleOutHandler(httpd_req_t *req)
{
    ++requests;
    const std::string sinceArg = queryParam(req, "since");
    uint64_t since = 0;
    if (sinceArg.empty()) {
        const uint64_t total = sidecarConsoleTotal();
        since = total > 4096 ? total - 4096 : 0;
    } else {
        since = strtoull(sinceArg.c_str(), nullptr, 10);
    }
#ifndef CSIM   // csim serves pages from the Sidecar's service loop: never wait there
    const int64_t waitUs = std::min<int64_t>(5000, atoll(queryParam(req, "wait").c_str())) * 1000;
    const int64_t until = esp_timer_get_time() + waitUs;
    while (sidecarConsoleTotal() <= since && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
    constexpr size_t kMax = 16384;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kMax]);
    if (!buf) return fail(req, "500 Internal Server Error", "no memory");
    uint64_t next = 0, lost = 0;
    const size_t n = sidecarConsoleRead(since, buf.get(), kMax, &next, &lost);
    const std::string nextStr = std::to_string(next), lostStr = std::to_string(lost);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Console-Next", nextStr.c_str());
    httpd_resp_set_hdr(req, "X-Console-Lost", lostStr.c_str());
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Console-Next, X-Console-Lost");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf.get(), static_cast<ssize_t>(n));
}

esp_err_t consoleCmdHandler(httpd_req_t *req)
{
    ++requests;
    std::string body = queryParam(req, "c");
    if (body.empty()) {
        if (req->content_len > 2048) return fail(req, "413 Payload Too Large", "2 KB of commands at most");
        body.resize(req->content_len);
        size_t got = 0;
        while (got < body.size()) {
            const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
            if (n <= 0) return fail(req, "400 Bad Request", "body interrupted");
            got += static_cast<size_t>(n);
        }
    }
    const uint64_t at = sidecarConsoleTotal();
    int queued = 0;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find_first_of("\r\n", start);
        if (end == std::string::npos) end = body.size();
        std::string line = body.substr(start, end - start);
        while (!line.empty() && isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && isspace(static_cast<unsigned char>(line[lead]))) ++lead;
        line.erase(0, lead);
        if (!line.empty()) {
            std::string why;
            if (!sidecarConsoleQueue(line, &why)) {
                return fail(req, "400 Bad Request", "'" + line + "': " + why + " (" + std::to_string(queued) +
                                                        " queued before it)");
            }
            ++queued;
        }
        start = end + 1;
    }
    if (queued == 0) return fail(req, "400 Bad Request", "no command");
    return sendJson(req, "{\"queued\":" + std::to_string(queued) + ",\"at\":" + std::to_string(at) + "}");
}

esp_err_t rmHandler(httpd_req_t *req)
{
    ++requests;
    const Spec spec = parseSpec(req);
    if (!spec.ok) return fail(req, "400 Bad Request", "bad path");
    bool ok = false;
    std::string why;
    if (spec.main) {
        MainClaim claim;
        if (!claim.held) return fail(req, "503 Service Unavailable", claim.why);
        sidecarRunInServiceLoop([&] { ok = mainFiles::remove(spec.path.c_str(), &why); });
    } else {
        ok = unlink(spec.path.c_str()) == 0;
        if (!ok) why = strerror(errno);
    }
    if (!ok) return fail(req, "404 Not Found", why);
    return sendJson(req, "{}");
}

esp_err_t hashHandler(httpd_req_t *req)
{
    ++requests;
    const Spec spec = parseSpec(req);
    if (!spec.ok) return fail(req, "400 Bad Request", "bad path");
    uint8_t digest[32];
    uint32_t size = 0;
    bool ok = false;
    std::string why;
    if (spec.main) {
        MainClaim claim;
        if (!claim.held) return fail(req, "503 Service Unavailable", claim.why);
        sidecarRunInServiceLoop([&] { ok = mainFiles::hash(spec.path.c_str(), digest, &size, &why); });
    } else {
        ok = sidecarHashFile(spec.path.c_str(), digest, &size);
        if (!ok) why = strerror(errno);
    }
    if (!ok) return fail(req, "404 Not Found", why);
    char hex[65];
    sidecarHexDigest(digest, hex);
    return sendJson(req, std::string("{\"sha256\":\"") + hex + "\",\"size\":" + std::to_string(size) + "}");
}

}  // namespace

void sidecarWebRegister(httpd_handle_t server)
{
    static const httpd_uri_t pages[] = {
        {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr},
        {.uri = "/hello", .method = HTTP_GET, .handler = helloHandler, .user_ctx = nullptr},
        {.uri = "/files/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr},
        {.uri = "/files/ls", .method = HTTP_GET, .handler = lsHandler, .user_ctx = nullptr},
        {.uri = "/files/get", .method = HTTP_GET, .handler = getHandler, .user_ctx = nullptr},
        {.uri = "/files/put", .method = HTTP_POST, .handler = putHandler, .user_ctx = nullptr},
        {.uri = "/files/rm", .method = HTTP_POST, .handler = rmHandler, .user_ctx = nullptr},
        {.uri = "/files/hash", .method = HTTP_GET, .handler = hashHandler, .user_ctx = nullptr},
        {.uri = "/console", .method = HTTP_GET, .handler = consolePageHandler, .user_ctx = nullptr},
        {.uri = "/console/out", .method = HTTP_GET, .handler = consoleOutHandler, .user_ctx = nullptr},
        {.uri = "/console/cmd", .method = HTTP_POST, .handler = consoleCmdHandler, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &page : pages) {
        httpd_register_uri_handler(server, &page);
    }
}

uint32_t sidecarWebRequests()
{
    return requests;
}

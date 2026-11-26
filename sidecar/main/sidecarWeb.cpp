// The Sidecar's web pages: see sidecarWeb.h.
//
// A minimal file manager: "/" is one page whose script drives a small API,
// on the Sidecar's own storage (/sd, /spiffs) and main's (main:/spiffs,
// over the link, in diagnostic mode only: sidecarMainFiles.h).
//   GET  /files/status            {"main": true|false, "why": "..."}
//   GET  /files/ls?p=<spec>       [{"n": name, "s": size (-1: a directory)}, ...]
//   GET  /files/get?p=<spec>      the file (a download)
//   POST /files/put?p=<spec>      the body becomes the file (<spec>.part, then renamed;
//                                 missing directories made, e.g. /sd/update);
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
//   GET  /screen                  main's screen, rendered (a page): Capture
//                                 posts "setflags screenshot interrupt"
//   GET  /screen/data             the latest screenshot from main, as main's
//                                 dumpScreenshot() text (main/screenshot.cpp);
//                                 header X-Screenshot-Seq; 404 before the first
//   GET  /config                  the boot configurations (a page): select one,
//                                 Boot now / Make default / Edit (console commands)
//   GET  /config/list             {"dir","default","next","boot","fallback",
//                                 "items":[{"n": name, "file": bool}]}: files A-Z,
//                                 then the built-in profiles A-Z
//   GET  /config/text?n=<name>    the file, or a built-in profile written out
//   POST /config/check            the body parsed as a boot would:
//                                 {"ok": bool, "errors": [{"line", "msg"}]}
//   POST /debug/abort             the Sidecar panics on purpose: a core dump
//                                 of every task (a hung one's backtrace), then
//                                 it reboots (not in csim)
//   GET  /debug/coredump          the Sidecar's coredump partition, raw
//                                 (decode: esp-coredump info_corefile -t raw
//                                 -c <file> sidecar/build/sidecar.elf; not in
//                                 csim)
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
#include "../../main/sysConfig.h"   // boot configurations: the CONFIG page

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
#include "esp_partition.h"
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

// The menu bar across the top of every page: the pages' names, the current
// one marked. Its CSS goes into each page's <style> (adjacent string
// literals), in the pages' own colour variables.
#define SIDECAR_MENU_CSS \
    ".menu{display:flex;align-items:stretch;gap:2px;margin:-16px -16px 16px;padding:0 16px;border-bottom:1px solid var(--line)}" \
    ".menu .brand{display:flex;align-items:center;padding:6px 14px 6px 0;margin-right:4px}" \
    ".menu .badge .pin{fill:var(--mut)}.menu .badge .die{fill:#1d1d1f;stroke:var(--mut);stroke-width:.8}" \
    ".menu .badge .bolt{stroke:#1d1d1f}" \
    ".menu a{padding:10px 14px;color:var(--mut);font-weight:600;letter-spacing:.04em;border-bottom:2px solid transparent;margin-bottom:-1px}" \
    ".menu a:hover{color:var(--fg)}" \
    ".menu a.on{color:var(--acc);border-bottom-color:var(--acc)}"
// The project's badge (an original design: a chip on the bus, retro
// stripes, a bolt for the bang), in the menu bar and as the page icon.
#define SIDECAR_BADGE_SVG \
    "<svg class='badge' viewBox='0 0 32 32' width='30' height='30' role='img' aria-label='atari8EspBusBang Sidecar'>" \
    "<title>atari8EspBusBang Sidecar</title>" \
    "<rect x='1' y='9' width='5' height='2.4' rx='.6' class='pin'/><rect x='1' y='14.8' width='5' height='2.4' rx='.6' class='pin'/><rect x='1' y='20.6' width='5' height='2.4' rx='.6' class='pin'/><rect x='26' y='9' width='5' height='2.4' rx='.6' class='pin'/><rect x='26' y='14.8' width='5' height='2.4' rx='.6' class='pin'/><rect x='26' y='20.6' width='5' height='2.4' rx='.6' class='pin'/><rect x='5' y='4' width='22' height='24' rx='3.5' class='die'/><rect x='8' y='9' width='16' height='3' fill='#f04e23'/><rect x='8' y='13' width='16' height='3' fill='#f7a21b'/><rect x='8' y='17' width='16' height='3' fill='#2bb673'/><rect x='8' y='21' width='16' height='3' fill='#2a8fd6'/><path d='M18.5 5.5 11 17h4.6l-2.6 9.5 8.5-12.6h-4.8l2.4-8.4z' fill='#fff' class='bolt' stroke-width='1' stroke-linejoin='round'/>" \
    "</svg>"
// The same badge as the page icon (a data: URL, '#' escaped): in each
// page's <head>.
#define SIDECAR_FAVICON \
    "<link rel=\"icon\" href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>" \
    "<rect x='1' y='9' width='5' height='2.4' rx='.6' fill='%236e6e73'/><rect x='1' y='14.8' width='5' height='2.4' rx='.6' fill='%236e6e73'/><rect x='1' y='20.6' width='5' height='2.4' rx='.6' fill='%236e6e73'/><rect x='26' y='9' width='5' height='2.4' rx='.6' fill='%236e6e73'/><rect x='26' y='14.8' width='5' height='2.4' rx='.6' fill='%236e6e73'/><rect x='26' y='20.6' width='5' height='2.4' rx='.6' fill='%236e6e73'/><rect x='5' y='4' width='22' height='24' rx='3.5' fill='%231d1d1f'/><rect x='8' y='9' width='16' height='3' fill='%23f04e23'/><rect x='8' y='13' width='16' height='3' fill='%23f7a21b'/><rect x='8' y='17' width='16' height='3' fill='%232bb673'/><rect x='8' y='21' width='16' height='3' fill='%232a8fd6'/><path d='M18.5 5.5 11 17h4.6l-2.6 9.5 8.5-12.6h-4.8l2.4-8.4z' fill='%23fff' stroke='%231d1d1f' stroke-width='1' stroke-linejoin='round'/>" \
    "</svg>\">"
#define SIDECAR_MENU(files, console, screen, cfg) \
    "<header class=\"menu\"><span class=\"brand\">" SIDECAR_BADGE_SVG "</span>" \
    "<a href=\"/\"" files ">FILES</a>" \
    "<a href=\"/console\"" console ">CONSOLE</a>" \
    "<a href=\"/screen\"" screen ">SCREEN</a>" \
    "<a href=\"/config\"" cfg ">CONFIG</a></header>"
#define SIDECAR_MENU_ON " class=\"on\""

const char kIndexPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sidecar files</title>
)HTML" SIDECAR_FAVICON R"HTML(
<style>
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0066cc;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mut:#a1a1a6;--line:#3a3a3c;--acc:#4da3ff;--bad:#ff6b6b}}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0;max-width:860px;padding:16px}
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
)HTML" SIDECAR_MENU_CSS R"HTML(
</style></head><body>
)HTML" SIDECAR_MENU(SIDECAR_MENU_ON, "", "", "") R"HTML(
<nav id="roots"></nav>
<div id="path"></div>
<div class="up"><input type="file" id="file"><button id="upb">Upload here</button></div>
<table id="list"></table>
<div id="msg"></div>
<script>
const roots=[["Sidecar flash","/spiffs"],["Sidecar SD","/sd"],["Main flash","main:/spiffs"]];
let cur="/spiffs", mainOk=false;
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

// The directories a put's path needs, made as needed: "/sd/update/main.bin.gz"
// makes /sd/update on the SD card (FAT). Past the mount point ("/sd/", the
// second '/'); errors ignored: an existing directory, or spiffs (flat, no
// directories: its names just contain '/'); fopen() then decides.
void makeParentDirs(const std::string &path)
{
    size_t at = path.find('/', 1);   // the end of the mount point
    while (at != std::string::npos && (at = path.find('/', at + 1)) != std::string::npos)
        mkdir(path.substr(0, at).c_str(), 0755);
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
        makeParentDirs(spec.path);
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
)HTML" SIDECAR_FAVICON R"HTML(
<style>
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0066cc;--bad:#c62828;--out:#f5f5f7}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mut:#a1a1a6;--line:#3a3a3c;--acc:#4da3ff;--bad:#ff6b6b;--out:#000}}
html,body{height:100%}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0;max-width:1100px;padding:16px;box-sizing:border-box;display:flex;flex-direction:column;gap:8px}
h1{font-size:20px;margin:0}
a{color:var(--acc);text-decoration:none}
#out{flex:1;min-height:200px;overflow:auto;margin:0;padding:8px;background:var(--out);border:1px solid var(--line);border-radius:6px;font:12px/1.35 ui-monospace,Menlo,monospace;white-space:pre-wrap;word-break:break-all}
form{display:flex;gap:8px}
input{flex:1;font:14px ui-monospace,Menlo,monospace;padding:6px 8px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg)}
button{font:inherit;padding:4px 12px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);cursor:pointer}
#msg{color:var(--mut);min-height:1.4em}#msg.bad{color:var(--bad)}
)HTML" SIDECAR_MENU_CSS R"HTML(
.menu{margin-bottom:0}
</style></head><body>
)HTML" SIDECAR_MENU("", SIDECAR_MENU_ON, "", "") R"HTML(
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

// ---- the screen page: main's screenshot, rendered ----

// main's screenshot (SidecarMainFlag::Screenshot): its dumpScreenshot()
// text (the format: main/screenshot.cpp), rendered in the browser as
// tools/screenshot_png.py renders it (the same walk of the "dl:" records,
// the same NTSC palette; no DLIs, players/missiles or fine scrolling).
// Capture posts "setflags screenshot interrupt" and waits for a newer one.
// The OS font: main can't read the Atari's own ROM ("--" in the dump), so
// charset bytes it couldn't read at $E000-$E3FF come from AltirraOS's font
// (AltirraOS 3.11, XL, $E000: Copyright (C) 2008-2018 Avery Lee. Copying and
// distribution of this file, with or without modification, are permitted in
// any medium without royalty provided the copyright notice and this notice
// are preserved. This file is offered as-is, without any warranty.)
const char kScreenPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sidecar screen</title>
)HTML" SIDECAR_FAVICON R"HTML(
<style>
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0066cc;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mut:#a1a1a6;--line:#3a3a3c;--acc:#4da3ff;--bad:#ff6b6b}}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0;max-width:860px;padding:16px}
a{color:var(--acc);text-decoration:none}
button{font:inherit;padding:4px 12px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);cursor:pointer}
button:disabled{opacity:.4;cursor:default}
.bar{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:12px}
canvas{display:block;max-width:100%;image-rendering:pixelated;background:#000;border-radius:4px}
#scale{width:110px;vertical-align:middle}
#msg{color:var(--mut);margin-top:8px;min-height:1.4em}#msg.bad{color:var(--bad)}
)HTML" SIDECAR_MENU_CSS R"HTML(
</style></head><body>
)HTML" SIDECAR_MENU("", "", SIDECAR_MENU_ON, "") R"HTML(
<div class="bar"><button id="cap">Capture</button>
<label><input type="checkbox" id="auto"> every 5 s</label>
<label>scale <input type="range" id="scale" min="0.5" max="4" step="0.5" value="1"> <span id="sv">1x</span></label>
<a href="/screen/data" target="_blank">raw</a></div>
<canvas id="cv" width="384" height="240"></canvas>
<div id="msg"></div>
<script>
const $=id=>document.getElementById(id);let seq=0,busy=false;
function msg(t,bad){$("msg").textContent=t;$("msg").className=bad?"bad":""}
const OSFONT=Uint8Array.from(atob("AAAAAAAAAAAAGBgYGAAYAABmZmYAAAAAAGb/Zmb/ZgAYPmA8BnwYAABmbBgwZkYAHDYcOG9mOwAAGBgYAAAAAAAOHBgYHA4AAHA4GBg4cAAAZjz/PGYAAAAYGH4YGAAAAAAAAAAYGDAAAAB+AAAAAAAAAAAAGBgAAAYMGDBgQAAAPGZudmY8AAAYOBgYGH4AADxmDBgwfgAAfgwYDGY8AAAMHDxsfgwAAH5gfAZmPAAAPGB8ZmY8AAB+BgwYMDAAADxmPGZmPAAAPGY+Bgw4AAAAGBgAGBgAAAAYGAAYGDAGDBgwGAwGAAAAfgAAfgAAYDAYDBgwYAAAPGYMGAAYAAA8Zm5uYD4AABg8ZmZ+ZgAAfGZ8ZmZ8AAA8ZmBgZjwAAHhsZmZseAAAfmB8YGB+AAB+YHxgYGAAAD5gYG5mPgAAZmZ+ZmZmAAB+GBgYGH4AAAYGBgZmPAAAZmx4eGxmAABgYGBgYH4AAGN3f2tjYwAAZnZ+fm5mAAA8ZmZmZjwAAHxmZnxgYAAAPGZmZmw2AAB8ZmZ8bGYAADxgPAYGPAAAfhgYGBgYAABmZmZmZn4AAGZmZmY8GAAAY2Nrf3djAABmZjw8ZmYAAGZmPBgYGAAAfgwYMGB+AAAeGBgYGB4AAEBgMBgMBgAAeBgYGBh4AAAIHDZjAAAAAAAAAAAA/wAANn9/PhwIABgYGB8fGBgYAwMDAwMDAwMYGBj4+AAAABgYGPj4GBgYAAAA+PgYGBgDBw4cOHDgwMDgcDgcDgcDAQMHDx8/f/8AAAAADw8PD4DA4PD4/P7/Dw8PDwAAAADw8PDwAAAAAP//AAAAAAAAAAAAAAAA//8AAAAA8PDw8AAcHHd3CBwAAAAAHx8YGBgAAAD//wAAABgYGP//GBgYAAA8fn5+PAAAAAAA/////8DAwMDAwMDAAAAA//8YGBgYGBj//wAAAPDw8PDw8PDwGBgYHx8AAAB4YHhgfhgeAAAYPH4YGBgAABgYGH48GAAAGDB+MBgAAAAYDH4MGAAAABg8fn48GAAAADwGPmY+AABgYHxmZnwAAAA8YGBgPAAABgY+ZmY+AAAAPGZ+YDwAAA4YPhgYGAAAAD5mZj4GfABgYHxmZmYAABgAOBgYPAAABgAGBgYGPABgYGx4bGYAADgYGBgYPAAAAGZ/f2tjAAAAfGZmZmYAAAA8ZmZmPAAAAHxmZnxgYAAAPmZmPgYGAAB8ZmBgYAAAAD5gPAZ8AAAYfhgYGA4AAABmZmZmPgAAAGZmZjwYAAAAY2t/PjYAAABmPBg8ZgAAAGZmZj4MeAAAfgwYMH4AABg8fn4YPAAYGBgYGBgYGAB+eHxuZgYACBg4eDgYCAAQGBweHBgQAA=="),c=>c.charCodeAt(0));
function parse(text){let cur=null,shot=null;
 for(const raw of text.split("\n")){const m=raw.match(/(?:^|\s)(screenshot|antic|gtia|shadow|dlist|dl|screenmem|charset): (.*)$/);if(!m)continue;
  const kind=m[1],rest=m[2].trim().split(/\s+/);
  if(kind==="screenshot"){if(rest[0]==="begin")cur={regs:{},dl:[],cs:new Map(),unread:0};else if(rest[0]==="end"&&cur){shot=cur;cur=null}continue}
  if(!cur)continue;
  if(kind==="antic"||kind==="gtia"||kind==="shadow"){const r=cur.regs[kind]={};let i=0;
   while(i<rest.length){const name=rest[i++],v=[];while(i<rest.length&&/^([0-9a-f]{2}|[0-9a-f]{4})$/.test(rest[i]))v.push(parseInt(rest[i++],16));r[name]=v.length===1?v[0]:v}}
  else if(kind==="dl")cur.dl.push(rest);
  else if(kind==="screenmem"){const d=rest.slice(1).map(f=>f==="--"?(cur.unread++,0):parseInt(f,16));const last=cur.dl[cur.dl.length-1];if(last&&last[2]==="mode")last.data=d}
  else if(kind==="charset"){const a=parseInt(rest[0],16);rest.slice(1).forEach((f,k)=>{if(f!=="--")cur.cs.set(a+k,parseInt(f,16))})}}
 return shot}
function ntsc(c){const hue=c>>4,lum=(c&14)>>1,y=.08+lum*.13;let i=0,q=0;
 if(hue){const a=(-58+(hue-1)*25.7)*Math.PI/180;i=.22*Math.cos(a);q=.22*Math.sin(a)}
 return [y+.956*i+.621*q,y-.272*i-.647*q,y-1.106*i+1.703*q].map(v=>Math.max(0,Math.min(255,Math.round(v*255))))}
const PER={2:8,3:8,4:8,5:8,6:16,7:16,8:32,9:32,10:16,11:16,12:16,13:8,14:8,15:8},W=384;
function render(shot){const an=shot.regs.antic||{},gt=shot.regs.gtia||{};
 const colpf=gt.colpf||[0,0,0,0],colpm=gt.colpm||[0,0,0,0],colbk=gt.colbk||0,prior=gt.prior||0,chactl=an.chactl||0,chbase=an.chbase||0;
 let osUsed=0,missing=0;
 const fb=a=>{const v=shot.cs.get(a);if(v!==undefined)return v;if(a>=0xe000&&a<0xe400){osUsed++;return OSFONT[a-0xe000]}missing++;return 0};
 const bk=ntsc(colbk),pf=colpf.map(ntsc),hi=ntsc((colpf[2]&0xf0)|(colpf[1]&14)),two=[bk,pf[0],pf[1],pf[2]];
 const rows=[];const blank=n=>{for(let k=0;k<n;k++)rows.push(null)};
 const g=(base,code,r)=>fb(base+code*8+(chactl&4?7-r:r));
 for(const rec of shot.dl){const what=rec[2];
  if(what==="blank"){blank(+rec[3]);continue}if(what==="jmp"){blank(1);continue}
  if(what==="jvb"||what==="end"||rec[1]==="end")break;if(what!=="mode")continue;
  const mode=parseInt(rec[3],16),lines=+rec[5],data=rec.data||[],width=data.length*(PER[mode]||8),x0=Math.max(0,(W-width)>>1);
  for(let line=0;line<lines;line++){const px=[];
   for(const b of data){
    if(mode===2||mode===3){const code=b&127,inv=b&128;let bits;
     if(mode===3){const fr=code>=0x60?(line<2?null:line<8?line:line-8):(line<8?line:null);bits=fr===null?0:g((chbase&0xfc)<<8,code,fr)}
     else bits=g((chbase&0xfc)<<8,code,line);
     if(inv&&chactl&1)bits=0;if(inv&&chactl&2)bits^=255;for(let k=0;k<8;k++)px.push(bits&(128>>k)?hi:pf[2])}
    else if(mode===4||mode===5){const bits=g((chbase&0xfc)<<8,b&127,mode===4?line:line>>1),cs=[bk,pf[0],pf[1],b&128?pf[3]:pf[2]];
     for(let k=0;k<4;k++){const c=cs[(bits>>(6-2*k))&3];px.push(c,c)}}
    else if(mode===6||mode===7){const bits=g((chbase&0xfe)<<8,b&63,mode===6?line:line>>1),on=pf[b>>6];
     for(let k=0;k<8;k++){const c=bits&(128>>k)?on:bk;px.push(c,c)}}
    else if([8,10,13,14].includes(mode)){const each={8:8,10:4,13:2,14:2}[mode];for(let k=0;k<4;k++)for(let e=0;e<each;e++)px.push(two[(b>>(6-2*k))&3])}
    else if([9,11,12].includes(mode)){const each={9:4,11:2,12:2}[mode];for(let k=0;k<8;k++)for(let e=0;e<each;e++)px.push(b&(128>>k)?pf[0]:bk)}
    else if(mode===15){const gm=prior>>6;
     if(!gm)for(let k=0;k<8;k++)px.push(b&(128>>k)?hi:pf[2]);
     else for(const nib of [b>>4,b&15]){let c;if(gm===1)c=ntsc((colbk&0xf0)|(nib&14));else if(gm===3)c=ntsc((nib<<4)|(colbk&14));
      else{const regs=colpm.concat(colpf,[colbk]);c=ntsc(nib<9?regs[nib]:colbk)}px.push(c,c,c,c)}}}
   rows.push({x0,px})}}
 // ANTIC shows the background after the display list ends, to the frame's
 // end: pad to the usual 240 visible scan lines (the bottom border).
 if(rows.length<240)blank(240-rows.length);
 const cv=$("cv"),h=rows.length;cv.width=W;cv.height=h;cv.style.aspectRatio=W+"/"+(h*2);
 const ctx=cv.getContext("2d"),img=ctx.createImageData(W,h);
 rows.forEach((r,y)=>{for(let x=0;x<W;x++){const c=r&&x>=r.x0&&x<r.x0+r.px.length?r.px[x-r.x0]:bk,o=(y*W+x)*4;img.data[o]=c[0];img.data[o+1]=c[1];img.data[o+2]=c[2];img.data[o+3]=255}});
 ctx.putImageData(img,0,0);applyScale();return {osUsed,missing,unread:shot.unread}}
async function load(){const r=await fetch("/screen/data",{cache:"no-store"});if(r.status===404)return 0;if(!r.ok)throw new Error(await r.text());
 const s=+r.headers.get("X-Screenshot-Seq"),text=await r.text();if(s===seq)return s;seq=s;
 const shot=parse(text);if(!shot){msg("screenshot "+s+": no complete record set",true);return s}
 const st=render(shot);msg("screenshot "+s+" ("+text.length+" bytes)"+(st.osUsed?", OS font: AltirraOS's":"")+(st.missing?", "+st.missing+" font bytes unreadable":"")+(st.unread?", "+st.unread+" screen bytes unreadable":""));return s}
async function capture(){if(busy)return;busy=true;$("cap").disabled=true;const before=seq;
 try{const r=await fetch("/console/cmd",{method:"POST",body:"setflags screenshot interrupt"});if(!r.ok)throw new Error(await r.text());
  const end=Date.now()+10000;while(Date.now()<end){await new Promise(r=>setTimeout(r,400));if(await load()>before)return}
  msg("no new screenshot from main in 10 s (is it running, not in diagnostic mode?)",true)}
 catch(e){msg(e.message,true)}finally{busy=false;$("cap").disabled=false}}
// The size on screen: scale x the 384 pixel width (1x by default), each scan
// line two pixels tall; remembered in this browser.
function applyScale(){const k=+$("scale").value,cv=$("cv");$("sv").textContent=k+"x";cv.style.width=(cv.width*k)+"px";
 try{localStorage.setItem("screenScale",k)}catch(e){}}
try{const k=localStorage.getItem("screenScale");if(k)$("scale").value=k}catch(e){}
$("scale").oninput=applyScale;applyScale();
$("cap").onclick=capture;
setInterval(()=>{if($("auto").checked)capture()},5000);
load().then(s=>{if(!s)msg("no screenshot yet: Capture asks main for one")}).catch(e=>msg(e.message,true));
</script></body></html>
)HTML";

esp_err_t screenPageHandler(httpd_req_t *req)
{
    ++requests;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kScreenPage, sizeof(kScreenPage) - 1);
}

// The latest screenshot's text; header X-Screenshot-Seq (1, 2 ...); 404
// before the first.
esp_err_t screenDataHandler(httpd_req_t *req)
{
    ++requests;
    std::string text;
    uint32_t seq = 0;
    if (!sidecarScreenshotLatest(&text, &seq)) return fail(req, "404 Not Found", "no screenshot yet");
    const std::string seqStr = std::to_string(seq);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Screenshot-Seq", seqStr.c_str());
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Screenshot-Seq");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, text.data(), static_cast<ssize_t>(text.size()));
}

// ---- the config page: boot configurations ----

// The boot configurations (main/sysConfig.h bootConfigFiles): the saved
// files, then the built-in profiles, one selected; Boot now (config next,
// reselect, reset main: the Atari restarts with it), Make default, Edit
// (the text in an editor: Check runs the real parser here, Save writes
// config/<name>.cfg). The actions are console commands (/console/cmd), so
// they run in the service loop as typed and show in the console.
const char kConfigPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sidecar config</title>
)HTML" SIDECAR_FAVICON R"HTML(
<style>
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0066cc;--bad:#c62828;--sel:#e8f1fc;--box:#fbfbfd}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mut:#a1a1a6;--line:#3a3a3c;--acc:#4da3ff;--bad:#ff6b6b;--sel:#17304d;--box:#232325}}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0;max-width:860px;padding:16px}
a{color:var(--acc);text-decoration:none}
button{font:inherit;padding:5px 14px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);cursor:pointer}
button.pri{border-color:var(--acc);color:var(--acc)}
button:disabled{opacity:.4;cursor:default}
#now{color:var(--mut);margin-bottom:10px}
#now b{color:var(--fg);font-weight:600}
#list{height:55vh;min-height:200px;overflow:auto;border:1px solid var(--line);border-radius:8px;background:var(--box)}
.sec{position:sticky;top:0;background:var(--box);color:var(--mut);font-size:12px;font-weight:600;letter-spacing:.06em;padding:8px 12px 4px;border-bottom:1px solid var(--line)}
.row{display:flex;align-items:center;gap:8px;padding:7px 12px;border-bottom:1px solid var(--line);cursor:pointer;user-select:none}
.row:hover{background:var(--sel)}
.row.on{background:var(--sel);box-shadow:inset 3px 0 var(--acc)}
.row .n{font:14px ui-monospace,Menlo,monospace;flex:1;overflow:hidden;text-overflow:ellipsis}
.pill{font-size:11px;font-weight:600;padding:1px 7px;border-radius:10px;border:1px solid var(--line);color:var(--mut);white-space:nowrap}
.pill.def{border-color:var(--acc);color:var(--acc)}
.pill.boot{background:var(--acc);border-color:var(--acc);color:var(--bg)}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
#ed{display:none;margin-top:16px}
#ed .top{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px}
#ed input{font:14px ui-monospace,Menlo,monospace;padding:4px 8px;border:1px solid var(--line);border-radius:6px;background:transparent;color:var(--fg);width:16em}
#txt{width:100%;box-sizing:border-box;height:45vh;font:13px/1.4 ui-monospace,Menlo,monospace;padding:8px;border:1px solid var(--line);border-radius:8px;background:var(--box);color:var(--fg);resize:vertical}
#errs{font:13px ui-monospace,Menlo,monospace;color:var(--bad);white-space:pre-wrap;margin-top:6px}
#msg{color:var(--mut);margin-top:10px;min-height:1.4em}#msg.bad{color:var(--bad)}
)HTML" SIDECAR_MENU_CSS R"HTML(
</style></head><body>
)HTML" SIDECAR_MENU("", "", "", SIDECAR_MENU_ON) R"HTML(
<div id="now"></div>
<div id="list" role="listbox" aria-label="boot configurations"></div>
<div class="bar"><button class="pri" id="boot" disabled>Boot now</button><button id="def" disabled>Make default</button><button id="edit" disabled>Edit config</button></div>
<div id="ed"><div class="top"><span>Save as</span><input id="name" spellcheck="false"><button id="chk">Check</button><button class="pri" id="save">Save</button><button id="cancel">Close</button></div>
<textarea id="txt" spellcheck="false"></textarea><div id="errs"></div></div>
<div id="msg"></div>
<script>
const $=id=>document.getElementById(id);let sel=null,info=null;
function msg(t,bad){$("msg").textContent=t||"";$("msg").className=bad?"bad":""}
async function cmd(lines){const r=await fetch("/console/cmd",{method:"POST",body:lines});if(!r.ok)throw new Error(await r.text())}
function pick(n){sel=n;for(const r of document.querySelectorAll(".row"))r.classList.toggle("on",r.dataset.n===n);
 for(const b of ["boot","def","edit"])$(b).disabled=!n}
async function load(){const r=await fetch("/config/list",{cache:"no-store"});if(!r.ok)throw new Error(await r.text());info=await r.json();
 $("now").innerHTML="This boot: <b>"+(info.boot||"fallback ("+info.fallback+")")+"</b> &middot; default: <b>"+(info.default||"none")+"</b>"+(info.next?" &middot; next: <b>"+info.next+"</b>":"");
 const list=$("list");list.textContent="";
 for(const [title,file] of [["SAVED",true],["BUILT IN",false]]){const items=info.items.filter(i=>i.file===file);if(!items.length)continue;
  const h=document.createElement("div");h.className="sec";h.textContent=title;list.appendChild(h);
  for(const it of items){const row=document.createElement("div");row.className="row";row.dataset.n=it.n;row.setAttribute("role","option");
   const n=document.createElement("span");n.className="n";n.textContent=it.n;row.appendChild(n);
   const pill=(t,c)=>{const p=document.createElement("span");p.className="pill "+c;p.textContent=t;row.appendChild(p)};
   if(it.n===info.boot)pill("this boot","boot");if(it.n===info.default)pill("default","def");if(it.n===info.next)pill("next","");
   row.onclick=()=>pick(it.n);row.ondblclick=()=>{pick(it.n);edit()};list.appendChild(row)}}
 if(!sel||!info.items.some(i=>i.n===sel))sel=info.boot||info.default||null;pick(sel);
 const on=document.querySelector(".row.on");if(on)on.scrollIntoView({block:"nearest"})}
$("boot").onclick=async()=>{if(!sel||!confirm("Boot "+sel+" now? Main and the Atari restart."))return;
 try{await cmd("config next "+sel+"\nconfig reselect\nreboot");msg("booting "+sel+": main restarts with it");setTimeout(()=>load().catch(()=>{}),1500)}catch(e){msg(e.message,true)}};
$("def").onclick=async()=>{if(!sel)return;try{await cmd("config default "+sel);msg(sel+" is the default from the next boot");setTimeout(()=>load().catch(()=>{}),600)}catch(e){msg(e.message,true)}};
async function edit(){if(!sel)return;try{const r=await fetch("/config/text?n="+encodeURIComponent(sel),{cache:"no-store"});if(!r.ok)throw new Error(await r.text());
 const it=info.items.find(i=>i.n===sel);$("txt").value=await r.text();$("name").value=it&&it.file?sel:"MY_"+sel;$("errs").textContent="";
 $("ed").style.display="block";$("txt").focus();msg(it&&it.file?"editing "+sel:"a copy of the built-in "+sel+": save it under a name of your own")}catch(e){msg(e.message,true)}}
$("edit").onclick=edit;$("cancel").onclick=()=>{$("ed").style.display="none";msg("")};
async function check(){const r=await fetch("/config/check",{method:"POST",body:$("txt").value});if(!r.ok)throw new Error(await r.text());
 const j=await r.json();$("errs").textContent=j.errors.map(e=>(e.line?"line "+e.line+": ":"")+e.msg).join("\n");return j}
$("chk").onclick=async()=>{try{const j=await check();msg(j.ok?"no errors":j.errors.length+" error(s): those lines are ignored",!j.ok)}catch(e){msg(e.message,true)}};
$("save").onclick=async()=>{const n=$("name").value.trim();if(!/^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,30}$/.test(n)){msg("a name is 1-31 of A-Z a-z 0-9 _ . - (not starting with .)",true);return}
 try{const j=await check();if(!j.ok&&!confirm(j.errors.length+" error(s): save anyway? Those lines are ignored at boot."))return;
  if(info.items.some(i=>i.n===n&&!i.file)&&!confirm(n+" is a built-in profile's name: the file will be used instead of it. Save?"))return;
  const r=await fetch("/files/put?p="+encodeURIComponent(info.dir+"/"+n+".cfg"),{method:"POST",body:$("txt").value});if(!r.ok)throw new Error(await r.text());
  msg("saved "+n);sel=n;await load()}catch(e){msg(e.message,true)}};
load().catch(e=>msg(e.message,true));
</script></body></html>
)HTML";

esp_err_t configPageHandler(httpd_req_t *req)
{
    ++requests;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kConfigPage, sizeof(kConfigPage) - 1);
}

bool configFileExists(const std::string &name)
{
    struct stat st;
    return bootConfigFiles::validName(name) &&
           stat((sidecarConfigDir() + "/" + name + bootConfigFiles::kSuffix).c_str(), &st) == 0;
}

// {"dir": the files' path for /files/put, "default", "next", "boot" (this
// boot's; "" the fallback), "fallback", "items": [{"n", "file"}]}: files
// A-Z, then the built-in profiles A-Z (one a file shadows left out).
esp_err_t configListHandler(httpd_req_t *req)
{
    ++requests;
    const std::string dir = sidecarConfigDir();
    std::string def, next, boot;
    bootConfigFiles::readName(dir + "/" + bootConfigFiles::kDefault, def);
    bootConfigFiles::readName(dir + "/" + bootConfigFiles::kNext, next);
    sidecarBootConfigName(&boot);
    std::vector<std::string> files, builtins;
    if (DIR *d = opendir(dir.c_str())) {
        const size_t suffix = strlen(bootConfigFiles::kSuffix);
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > suffix && n.compare(n.size() - suffix, suffix, bootConfigFiles::kSuffix) == 0 &&
                bootConfigFiles::validName(n.substr(0, n.size() - suffix)))
                files.push_back(n.substr(0, n.size() - suffix));
        }
        closedir(d);
    }
    for (const char *const *p = SysConfig::builtinNames; *p; p++)
        if (std::find(files.begin(), files.end(), *p) == files.end()) builtins.push_back(*p);
    std::sort(files.begin(), files.end());
    std::sort(builtins.begin(), builtins.end());
    std::string json = "{\"dir\":" + jsonString("/spiffs/config") + ",\"default\":" + jsonString(def) +
                       ",\"next\":" + jsonString(next) + ",\"boot\":" + jsonString(boot) +
                       ",\"fallback\":" + jsonString(SysConfig::kFallback) + ",\"items\":[";
    bool first = true;
    for (const auto *group : { &files, &builtins })
        for (const std::string &n : *group) {
            json += std::string(first ? "" : ",") + "{\"n\":" + jsonString(n) +
                    ",\"file\":" + (group == &files ? "true" : "false") + "}";
            first = false;
        }
    return sendJson(req, json + "]}");
}

// ?n=<name>: the file as it is, or a built-in profile written out whole
// (with comments), to start an edit from.
esp_err_t configTextHandler(httpd_req_t *req)
{
    ++requests;
    const std::string name = queryParam(req, "n");
    if (!bootConfigFiles::validName(name)) return fail(req, "400 Bad Request", "not a configuration name");
    std::string text;
    if (configFileExists(name)) {
        FILE *f = fopen((sidecarConfigDir() + "/" + name + bootConfigFiles::kSuffix).c_str(), "r");
        if (!f) return fail(req, "500 Internal Server Error", "can't read it");
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 16384) text.append(buf, n);
        fclose(f);
    } else {
        SysConfig profile;
        if (!profile.loadBuiltin(name)) return fail(req, "404 Not Found", "no configuration " + name);
        text = "# The built-in profile " + name + ", written out whole. Delete what you\n"
               "# don't change, or start a new file with BASE=" + name + " and only your changes.\n" +
               static_cast<const AtariConfig &>(profile).toText();
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, text.data(), static_cast<ssize_t>(text.size()));
}

// The body, parsed as a boot would (BASE= names resolved here):
// {"ok": bool, "errors": [{"line", "msg"}]}.
esp_err_t configCheckHandler(httpd_req_t *req)
{
    ++requests;
    if (req->content_len > 16384) return fail(req, "413 Payload Too Large", "16 KB at most");
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) return fail(req, "400 Bad Request", "body interrupted");
        got += static_cast<size_t>(n);
    }
    const std::string dir = sidecarConfigDir();
    AtariConfig c;
    std::vector<AtariConfig::Error> errors;
    const bool ok = c.fromText(body.c_str(), &errors, [&](const std::string &base, AtariConfig &out) {
        return bootConfigFiles::load(dir, base, out, &errors);
    });
    std::string json = std::string("{\"ok\":") + (ok && errors.empty() ? "true" : "false") + ",\"errors\":[";
    for (size_t i = 0; i < errors.size(); i++)
        json += std::string(i ? "," : "") + "{\"line\":" + std::to_string(errors[i].line) +
                ",\"msg\":" + jsonString(errors[i].msg) + "}";
    return sendJson(req, json + "]}");
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

#ifndef CSIM
esp_err_t debugAbortHandler(httpd_req_t *req)
{
    ++requests;
    httpd_resp_sendstr(req, "aborting: a core dump, then a reboot\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    abort();
}

esp_err_t debugCoredumpHandler(httpd_req_t *req)
{
    ++requests;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!part) return fail(req, "404 Not Found", "no coredump partition");
    constexpr size_t kChunk = 4096;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk]);
    if (!buf) return fail(req, "500 Internal Server Error", "no memory");
    httpd_resp_set_type(req, "application/octet-stream");
    for (size_t at = 0; at < part->size; at += kChunk) {
        const size_t n = std::min(kChunk, static_cast<size_t>(part->size - at));
        if (esp_partition_read(part, at, buf.get(), n) != ESP_OK ||
            httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf.get()), static_cast<ssize_t>(n)) != ESP_OK) {
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}
#endif

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
        {.uri = "/screen", .method = HTTP_GET, .handler = screenPageHandler, .user_ctx = nullptr},
        {.uri = "/screen/data", .method = HTTP_GET, .handler = screenDataHandler, .user_ctx = nullptr},
        {.uri = "/config", .method = HTTP_GET, .handler = configPageHandler, .user_ctx = nullptr},
        {.uri = "/config/list", .method = HTTP_GET, .handler = configListHandler, .user_ctx = nullptr},
        {.uri = "/config/text", .method = HTTP_GET, .handler = configTextHandler, .user_ctx = nullptr},
        {.uri = "/config/check", .method = HTTP_POST, .handler = configCheckHandler, .user_ctx = nullptr},
#ifndef CSIM
        {.uri = "/debug/abort", .method = HTTP_POST, .handler = debugAbortHandler, .user_ctx = nullptr},
        {.uri = "/debug/coredump", .method = HTTP_GET, .handler = debugCoredumpHandler, .user_ctx = nullptr},
#endif
    };
    for (const httpd_uri_t &page : pages) {
        httpd_register_uri_handler(server, &page);
    }
}

uint32_t sidecarWebRequests()
{
    return requests;
}

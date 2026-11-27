#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <minizip/unzip.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <map>

using json = nlohmann::json;

static SDL_Window* win; static SDL_Renderer* ren; static TTF_Font* font;
static std::map<std::string, std::string> L;
static bool rtl = true;
static std::string status;
static json state;
static const char* STATE_PATH = "/config/myupdater/state.json";

// ---------- helpers ----------
static json readJson(const std::string& p) {
    std::ifstream f(p); if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}
static void mkdirs(std::string path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') { path[i] = 0; mkdir(path.c_str(), 0777); path[i] = '/'; }
    mkdir(path.c_str(), 0777);
}
static void loadLang(const std::string& code) {
    json j = readJson("romfs:/lang/" + code + ".json");
    L.clear();
    for (auto& [k, v] : j.items()) L[k] = v.get<std::string>();
    rtl = (L["dir"] == "rtl");
    TTF_SetFontDirection(font, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);
    TTF_SetFontScriptName(font, rtl ? "Arab" : "Latn");
    TTF_SetFontLanguage(font, code.c_str());
}
static const std::string& T(const char* k) { return L[k]; }

// ---------- rendering ----------
static void text(const std::string& s, int y, SDL_Color c) {
    if (s.empty()) return;
    SDL_Surface* sf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!sf) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, sf);
    SDL_Rect r{ rtl ? 1280 - 60 - sf->w : 60, y, sf->w, sf->h };
    SDL_RenderCopy(ren, t, nullptr, &r);
    SDL_DestroyTexture(t); SDL_FreeSurface(sf);
}
static float progress = -1;
static void bar(float pct, int y) {
    SDL_Rect bg{60, y, 1160, 20};
    int w = int(1160 * pct);
    SDL_Rect fg{rtl ? 60 + 1160 - w : 60, y, w, 20};
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255); SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 0, 190, 140, 255); SDL_RenderFillRect(ren, &fg);
}
static std::vector<json> sources; static int sel = 0;

static void draw() {
    SDL_SetRenderDrawColor(ren, 30, 30, 36, 255); SDL_RenderClear(ren);
    SDL_Color w{255,255,255,255}, g{0,220,160,255}, d{160,160,160,255};
    text(T("title"), 30, g);
    for (size_t i = 0; i < sources.size(); i++) {
        std::string name = sources[i]["name"], v = state.value(name, "");
        std::string line = name + "  -  " + T("installed") + ": " + (v.empty() ? T("none") : v);
        text(line, 110 + int(i) * 50, (int)i == sel ? g : w);
    }
    text(status, 560, w);
    if (progress >= 0) bar(progress, 610);
    text(T("hint"), 665, d);
    SDL_RenderPresent(ren);
}

// ---------- network ----------
static size_t wr(char* p, size_t s, size_t n, void* u) { ((std::string*)u)->append(p, s * n); return s * n; }
static bool httpGet(const std::string& url, std::string& out) {
    CURL* c = curl_easy_init(); if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "my-updater");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    return r == CURLE_OK;
}
static int xfer(void*, curl_off_t tot, curl_off_t now, curl_off_t, curl_off_t) {
    if (tot > 0) { progress = float(now) / float(tot); draw(); }
    return 0;
}
static bool download(const std::string& url, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "my-updater");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer);
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c); fclose(f);
    return r == CURLE_OK;
}

// ---------- zip ----------
static bool extractZip(const std::string& zip) {
    unzFile z = unzOpen(zip.c_str()); if (!z) return false;
    if (unzGoToFirstFile(z) != UNZ_OK) { unzClose(z); return false; }
    do {
        char name[512]; unz_file_info info;
        unzGetCurrentFileInfo(z, &info, name, sizeof name, 0, 0, 0, 0);
        std::string out = std::string("/") + name;
        if (out.find("..") != std::string::npos) continue;  // path traversal guard
        if (out.back() == '/') { mkdirs(out); continue; }
        mkdirs(out.substr(0, out.find_last_of('/')));
        if (unzOpenCurrentFile(z) != UNZ_OK) continue;
        FILE* f = fopen(out.c_str(), "wb");
        if (f) {
            static char buf[0x10000]; int n;
            while ((n = unzReadCurrentFile(z, buf, sizeof buf)) > 0) fwrite(buf, 1, n, f);
            fclose(f);
        }
        unzCloseCurrentFile(z);
    } while (unzGoToNextFile(z) == UNZ_OK);
    unzClose(z); return true;
}

// ---------- update flow ----------
static void update(const json& src) {
    std::string name = src["name"];
    progress = -1; status = T("fetching"); draw();
    std::string body;
    if (!httpGet("https://api.github.com/repos/" + src["repo"].get<std::string>() + "/releases/latest", body))
        { status = T("err_net"); return; }
    json rel = json::parse(body, nullptr, false);
    if (rel.is_discarded() || !rel.contains("tag_name")) { status = T("err_net"); return; }
    std::string tag = rel["tag_name"];
    if (state.value(name, "") == tag) { status = T("uptodate"); return; }

    std::regex re(src["asset"].get<std::string>());
    std::string url;
    for (auto& a : rel["assets"])
        if (std::regex_match(a["name"].get<std::string>(), re)) { url = a["browser_download_url"].get<std::string>(); break; }
    if (url.empty()) { status = T("err_asset"); return; }

    mkdirs("/switch/.temp");
    const std::string tmp = "/switch/.temp/update.zip";
    status = T("downloading") + std::string(": ") + name; draw();
    if (!download(url, tmp)) { status = T("err_net"); return; }

    progress = -1; status = T("extracting"); draw();
    if (!extractZip(tmp)) { status = T("err_zip"); return; }
    remove(tmp.c_str());

    state[name] = tag; mkdirs("/config/myupdater");
    std::ofstream(STATE_PATH) << state.dump(2);
    status = T("done");
}

int main() {
    romfsInit(); socketInitializeDefault();
    SDL_Init(SDL_INIT_VIDEO); TTF_Init();
    win = SDL_CreateWindow("u", 0, 0, 1280, 720, 0);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    font = TTF_OpenFont("romfs:/fonts/NotoSansArabic-Regular.ttf", 32);
    if (!font) return 1;  // ضع الخط في romfs/fonts

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);

    loadLang("ar");
    sources = readJson("romfs:/sources.json")["sources"].get<std::vector<json>>();
    state = readJson(STATE_PATH);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (appletMainLoop()) {
        padUpdate(&pad); u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Plus) break;
        if (k & HidNpadButton_Down) sel = (sel + 1) % sources.size();
        if (k & HidNpadButton_Up)   sel = (sel + sources.size() - 1) % sources.size();
        if (k & HidNpadButton_A)    update(sources[sel]);
        draw();
    }
    curl_global_cleanup(); TTF_CloseFont(font); TTF_Quit(); SDL_Quit();
    socketExit(); romfsExit();
}

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <minizip/unzip.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <map>

using json = nlohmann::json;

static SDL_Window* win = nullptr;
static SDL_Renderer* ren = nullptr;
static TTF_Font* font = nullptr;
static std::map<std::string, std::string> L;
static bool rtl = true;
static std::string status;
static json state;
static const char* STATE_PATH = "/config/myupdater/state.json";

static json readJson(const std::string& p) {
    std::ifstream f(p);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

static void mkdirs(std::string path) {
    if (path.empty()) return;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            path[i] = '\0';
            mkdir(path.c_str(), 0777);
            path[i] = '/';
        }
    }
    mkdir(path.c_str(), 0777);
}

static bool hasParentTraversal(const std::string& p) {
    return p.find("/../") != std::string::npos ||
           p.rfind("../", 0) == 0 ||
           (p.size() >= 3 && p.compare(p.size() - 3, 3, "/..") == 0);
}

static void loadLang(const std::string& code) {
    json j = readJson("romfs:/lang/" + code + ".json");
    L.clear();
    if (j.is_object()) {
        for (auto& [k, v] : j.items()) {
            if (v.is_string()) L[k] = v.get<std::string>();
        }
    }
    rtl = (L["dir"] == "rtl");
    if (font) {
        TTF_SetFontDirection(font, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);
        TTF_SetFontScriptName(font, rtl ? "Arab" : "Latn");
        TTF_SetFontLanguage(font, code.c_str());
    }
}

static std::string T(const char* k) {
    auto it = L.find(k);
    return (it != L.end()) ? it->second : std::string(k);
}

static void text(const std::string& s, int y, SDL_Color c) {
    if (s.empty() || !font || !ren) return;
    SDL_Surface* sf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!sf) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, sf);
    if (t) {
        int x = rtl ? std::max(20, 1280 - 60 - sf->w) : 60;
        SDL_Rect r{x, y, sf->w, sf->h};
        SDL_RenderCopy(ren, t, nullptr, &r);
        SDL_DestroyTexture(t);
    }
    SDL_FreeSurface(sf);
}

static float progress = -1.0f;

static void bar(float pct, int y) {
    pct = std::clamp(pct, 0.0f, 1.0f);
    SDL_Rect bg{60, y, 1160, 20};
    int w = static_cast<int>(1160.0f * pct);
    SDL_Rect fg{rtl ? (60 + 1160 - w) : 60, y, w, 20};
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 0, 190, 140, 255);
    SDL_RenderFillRect(ren, &fg);
}

static std::vector<json> sources;
static int sel = 0;

static void draw() {
    if (!ren) return;
    SDL_SetRenderDrawColor(ren, 30, 30, 36, 255);
    SDL_RenderClear(ren);
    SDL_Color w{255, 255, 255, 255}, g{0, 220, 160, 255}, d{160, 160, 160, 255};

    text(T("title"), 30, g);
    for (size_t i = 0; i < sources.size(); i++) {
        std::string name = sources[i].value("name", "unknown");
        std::string v = state.value(name, "");
        std::string verText = v.empty() ? T("none") : v;
        std::string line = rtl
            ? (verText + " :" + T("installed") + "  -  " + name)
            : (name + "  -  " + T("installed") + ": " + verText);
        text(line, 110 + static_cast<int>(i) * 50, static_cast<int>(i) == sel ? g : w);
    }
    text(status, 560, w);
    if (progress >= 0.0f) bar(progress, 610);
    text(T("hint"), 665, d);
    SDL_RenderPresent(ren);
}

static size_t wr(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}

static bool httpGet(const std::string& url, std::string& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    out.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "my-updater");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK;
}

static int xfer(void*, curl_off_t tot, curl_off_t now, curl_off_t, curl_off_t) {
    if (tot > 0) {
        progress = static_cast<float>(now) / static_cast<float>(tot);
        draw();
    }
    return 0;
}

static bool download(const std::string& url, const std::string& path) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        curl_easy_cleanup(c);
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "my-updater");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    std::fclose(f);
    return r == CURLE_OK;
}

static bool extractZip(const std::string& zip) {
    unzFile z = unzOpen(zip.c_str());
    if (!z) return false;
    if (unzGoToFirstFile(z) != UNZ_OK) {
        unzClose(z);
        return false;
    }
    do {
        char name[512];
        unz_file_info info;
        if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(z);
            return false;
        }
        std::string entry(name);
        if (hasParentTraversal(entry)) continue;
        std::string out = "/" + entry;
        if (!out.empty() && out.back() == '/') {
            mkdirs(out);
            continue;
        }
        auto slash = out.find_last_of('/');
        if (slash != std::string::npos) mkdirs(out.substr(0, slash));
        if (unzOpenCurrentFile(z) != UNZ_OK) {
            unzClose(z);
            return false;
        }
        FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) {
            unzCloseCurrentFile(z);
            unzClose(z);
            return false;
        }
        char buf[0x10000];
        int n = 0;
        while ((n = unzReadCurrentFile(z, buf, sizeof(buf))) > 0) {
            if (std::fwrite(buf, 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) {
                std::fclose(f);
                unzCloseCurrentFile(z);
                unzClose(z);
                return false;
            }
        }
        std::fclose(f);
        unzCloseCurrentFile(z);
        if (n < 0) {
            unzClose(z);
            return false;
        }
    } while (unzGoToNextFile(z) == UNZ_OK);
    unzClose(z);
    return true;
}

static void update(const json& src) {
    if (!src.contains("name") || !src.contains("repo") || !src.contains("asset")) {
        status = T("err_asset");
        return;
    }
    std::string name = src["name"].get<std::string>();
    progress = -1.0f;
    status = T("fetching");
    draw();

    std::string body;
    if (!httpGet("https://api.github.com/repos/" + src["repo"].get<std::string>() + "/releases/latest", body)) {
        status = T("err_net");
        return;
    }
    json rel = json::parse(body, nullptr, false);
    if (rel.is_discarded() || !rel.contains("tag_name") || !rel["tag_name"].is_string()) {
        status = T("err_net");
        return;
    }
    std::string tag = rel["tag_name"].get<std::string>();
    if (state.value(name, "") == tag) {
        status = T("uptodate");
        return;
    }

    std::regex re(src["asset"].get<std::string>());
    std::string url;
    if (rel.contains("assets") && rel["assets"].is_array()) {
        for (const auto& a : rel["assets"]) {
            if (a.contains("name") && a["name"].is_string() &&
                a.contains("browser_download_url") && a["browser_download_url"].is_string() &&
                std::regex_match(a["name"].get<std::string>(), re)) {
                url = a["browser_download_url"].get<std::string>();
                break;
            }
        }
    }
    if (url.empty()) {
        status = T("err_asset");
        return;
    }

    mkdirs("/switch/.temp");
    const std::string tmp = "/switch/.temp/update.zip";
    status = T("downloading") + std::string(": ") + name;
    draw();
    if (!download(url, tmp)) {
        std::remove(tmp.c_str());
        status = T("err_net");
        return;
    }

    progress = -1.0f;
    status = T("extracting");
    draw();
    if (!extractZip(tmp)) {
        std::remove(tmp.c_str());
        status = T("err_zip");
        return;
    }
    std::remove(tmp.c_str());

    state[name] = tag;
    mkdirs("/config/myupdater");
    std::ofstream sf(STATE_PATH);
    if (sf) sf << state.dump(2);
    progress = -1.0f;
    status = T("done");
}

static void cleanup() {
    if (font) TTF_CloseFont(font);
    TTF_Quit();
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    socketExit();
    romfsExit();
}

int main() {
    romfsInit();
    socketInitializeDefault();
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();

    win = SDL_CreateWindow("myupdater", 0, 0, 1280, 720, 0);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    font = TTF_OpenFont("romfs:/fonts/NotoSansArabic-Regular.ttf", 32);
    if (!font) {
        cleanup();
        return 1;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    loadLang("ar");
    json srcJson = readJson("romfs:/sources.json");
    if (srcJson.contains("sources") && srcJson["sources"].is_array()) {
        sources = srcJson["sources"].get<std::vector<json>>();
    }
    state = readJson(STATE_PATH);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Plus) break;
        if (!sources.empty()) {
            int n = static_cast<int>(sources.size());
            if (k & HidNpadButton_Down) sel = (sel + 1) % n;
            if (k & HidNpadButton_Up)   sel = (sel + n - 1) % n;
            if (k & HidNpadButton_A)    update(sources[sel]);
        }
        draw();
    }

    curl_global_cleanup();
    cleanup();
    return 0;
}

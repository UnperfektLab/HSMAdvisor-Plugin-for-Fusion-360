// HSMAdvisor Plugin for Fusion 360 - native add-in.
// Copyright (c) 2026 UnperfektLab. MIT License; see LICENSE.
//
// Unofficial third-party plugin. Not supported by HSMAdvisor / Eldar Gerfanov.
// It drives a separately installed, licensed copy of HSMAdvisor.

#include <Core/CoreAll.h>
#include <Fusion/FusionAll.h>
#include <Cam/CamAll.h>

#include <windows.h>
#include <urlmon.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <regex>
#include <thread>

// HSMAdvisor's native integration client (loaded at run time) and the JSON it exchanges.
#include <hsmadvisor/hsmadvisor_integration_loader.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

using json = nlohmann::ordered_json;

using namespace adsk::core;
using namespace adsk::fusion;
using namespace adsk::cam;

Ptr<Application> app;
Ptr<UserInterface> ui;

static const char* kCmdId = "HSMAdvisorTestCmd";

// Lowercased copy of a string (ASCII).
static std::string toLower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// Parameter helpers. CAM lengths come back in centimeters (*10 -> mm); we write
// values back as expressions with unit tokens (rpm/mmpm/mm) so Fusion converts them
// to the document's display units.

// Typed value of a parameter (nullptr if absent or wrong type).
template <typename T>
static Ptr<T> paramValue(const Ptr<CAMParameters>& params, const std::string& name)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    return p ? Ptr<T>(p->value()) : nullptr;
}

// Evaluated numeric value of a float parameter (internal units).
static bool readFloatVal(const Ptr<CAMParameters>& params, const std::string& name, double& out)
{
    Ptr<FloatParameterValue> fv = paramValue<FloatParameterValue>(params, name);
    if (!fv) return false;
    out = fv->value();
    return true;
}

static double readLenMm(const Ptr<CAMParameters>& params, const std::string& name, double fallback)
{
    double v;
    return readFloatVal(params, name, v) ? v * 10.0 : fallback; // cm -> mm
}

// Reads the first candidate length that exists on this strategy.
static double readFirstLenMm(const Ptr<CAMParameters>& params,
                             const std::vector<std::string>& candidates)
{
    for (const std::string& name : candidates)
    {
        double v;
        if (readFloatVal(params, name, v)) return v * 10.0; // cm -> mm
    }
    return 0.0;
}

static int readInt(const Ptr<CAMParameters>& params, const std::string& name, int fallback)
{
    Ptr<IntegerParameterValue> iv = paramValue<IntegerParameterValue>(params, name);
    return iv ? iv->value() : fallback;
}

// CAM angle parameters return value() already in degrees. So read it straight, no radian conversion.
static double readAngleDeg(const Ptr<CAMParameters>& params, const std::string& name, double fallback)
{
    double v;
    return readFloatVal(params, name, v) ? v : fallback;
}

static bool readBool(const Ptr<CAMParameters>& params, const std::string& name, bool fallback)
{
    Ptr<BooleanParameterValue> bv = paramValue<BooleanParameterValue>(params, name);
    return bv ? bv->value() : fallback;
}

static std::string readChoice(const Ptr<CAMParameters>& params, const std::string& name, const std::string& fallback)
{
    Ptr<ChoiceParameterValue> cv = paramValue<ChoiceParameterValue>(params, name);
    return cv ? cv->value() : fallback;
}

static std::string readString(const Ptr<CAMParameters>& params, const std::string& name)
{
    Ptr<StringParameterValue> sv = paramValue<StringParameterValue>(params, name);
    return sv ? sv->value() : "";
}

// Current expression of a parameter
static std::string readExpr(const Ptr<CAMParameters>& params, const std::string& name)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    return p ? p->expression() : std::string();
}

// Flank half-angle (degrees, measured from the tool axis) used to convert a chamfer's
// radial width to an axial depth.
static double chamferHalfAngleDeg(const Ptr<CAMParameters>& toolParams)
{
    double taper = readAngleDeg(toolParams, "tool_taperAngle", 0.0);
    double tip   = readAngleDeg(toolParams, "tool_tipAngle", 0.0);
    if (taper > 0.0) return taper;
    if (tip   > 0.0) return tip * 0.5;
    return 45.0;
}

// tan(flank angle); 0 is treated as "no axial component" (guard div-by-zero).
static double chamferTan(double halfDeg)
{
    const double kPi = 3.14159265358979323846;
    return std::tan(halfDeg * kPi / 180.0);
}

// Recognizes a chamfer operation by strategy name or by tool type. This is used to decide whether to write chamferWidth/chamferTipOffset instead of maximumStepdown/maximumStepover.
static bool isChamferEngagement(const Ptr<Operation>& op)
{
    if (!op) return false;

    std::string strat = toLower(op->strategy());

    // 2D Chamfer operation (strategy id contains "chamfer").
    if (strat.find("chamfer") != std::string::npos)
        return true;

    // 2D Contour with a chamfer tool.
    if (strat.find("contour") != std::string::npos)
    {
        Ptr<Tool> tool = op->tool();
        std::string toolType = toLower(readChoice(tool ? tool->parameters() : nullptr, "tool_type", ""));
        if (toolType.find("chamfer") != std::string::npos)
            return true;
    }
    return false;
}

// Outcome of trying to set a parameter that may or may not exist on this strategy.
enum class SetResult { NotPresent, Failed, Ok };

// Writes only if the parameter exists; distinguishes "absent on this strategy"
// from "present but rejected".
static SetResult trySet(const Ptr<CAMParameters>& params, const std::string& name,
                        const std::string& expr)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return SetResult::NotPresent;
    
    Ptr<ChoiceParameterValue> cv = p->value();
    if (cv) return cv->value(expr) ? SetResult::Ok : SetResult::Failed;
    return p->expression(expr) ? SetResult::Ok : SetResult::Failed;
}

// True if the parameter exists on this operation.
static bool hasParam(const Ptr<CAMParameters>& params, const std::string& name)
{
    return params && params->itemByName(name);
}

// First of `candidates` that exists on this operation ("" if none), without writing.
static std::string firstExistingName(const Ptr<CAMParameters>& params,
                                     const std::vector<std::string>& candidates)
{
    for (const std::string& name : candidates)
        if (hasParam(params, name)) return name;
    return "";
}

// Label for a CAM parameter id (falls back to the raw id).
static std::string friendlyParamName(const std::string& raw)
{
    if (raw == "tool_spindleSpeed") return "Spindle speed";
    if (raw == "tool_rampSpindleSpeed") return "Ramp spindle speed";
    if (raw == "tool_feedCutting")  return "Cutting feed";
    if (raw == "tool_feedPlunge")   return "Plunge feed";
    if (raw == "tool_feedRamp")     return "Ramp feed";
    if (raw == "peckingDepth")      return "Peck depth";
    if (raw == "maximumStepdown")   return "Depth of cut";
    if (raw == "optimalLoad" || raw == "maximumStepover" || raw == "stepover")
        return "Width of cut";
    if (raw == "chamferWidth")      return "Chamfer width";
    if (raw == "chamferTipOffset")  return "Chamfer tip offset";
    if (raw == "rampType")          return "Ramp type";
    if (raw == "rampAngle")         return "Ramp angle";
    return raw;
}

static std::string numToStr(double v, int decimals)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(decimals);
    oss << v;
    return oss.str();
}

static std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

// Folder of this add-in's DLL (manifest, prefs, settings, bundled HSMAdvisorIntegration.dll).
static const std::string& addinFolder()
{
    static const std::string dir = []() -> std::string {
        char pathBuf[MAX_PATH] = {0};
        HMODULE self = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&addinFolder, &self);
        GetModuleFileNameA(self, pathBuf, MAX_PATH);
        std::string full(pathBuf);
        size_t slash = full.find_last_of("\\/");
        return (slash == std::string::npos) ? "." : full.substr(0, slash);
    }();
    return dir;
}

// Which results the user wants written back. Persisted
// between sessions in apply_prefs.txt.
struct ApplySel { bool all, ad, ae, rpm, feed, plungeRpm, plungeFeed, ramp, rampType, rampAngle; };
static ApplySel g_apply = { true, true, true, true, true, true, true, true, true, true };

static std::string prefsPath() { return addinFolder() + "\\apply_prefs.txt"; }

// Parses key=value lines (ignoring CR and blank/keyless lines) into a map.
static std::map<std::string, std::string> parseKvText(const std::string& text)
{
    std::map<std::string, std::string> m;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

static std::string kvGet(const std::map<std::string, std::string>& m,
                         const std::string& k, const std::string& def)
{
    auto it = m.find(k);
    return (it == m.end()) ? def : it->second;
}

static void loadApplyPrefs()
{
    std::ifstream f(prefsPath().c_str(), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::map<std::string, std::string> m = parseKvText(ss.str());
    auto gb = [&](const char* k, bool d) { auto it = m.find(k); return it == m.end() ? d : (it->second == "1"); };
    g_apply.all        = gb("all", true);
    g_apply.ad         = gb("ad", true);
    g_apply.ae         = gb("ae", true);
    g_apply.rpm        = gb("rpm", true);
    g_apply.feed       = gb("feed", true);
    g_apply.plungeRpm  = gb("plungeRpm", true);
    g_apply.plungeFeed = gb("plungeFeed", true);
    g_apply.ramp       = gb("ramp", true);
    g_apply.rampType   = gb("rampType", true);
    g_apply.rampAngle  = gb("rampAngle", true);
}

static void saveApplyPrefs()
{
    std::ofstream f(prefsPath().c_str(), std::ios::binary);
    if (!f) return;
    f << "all=" << (g_apply.all ? 1 : 0) << "\n"
      << "ad=" << (g_apply.ad ? 1 : 0) << "\n"
      << "ae=" << (g_apply.ae ? 1 : 0) << "\n"
      << "rpm=" << (g_apply.rpm ? 1 : 0) << "\n"
      << "feed=" << (g_apply.feed ? 1 : 0) << "\n"
      << "plungeRpm=" << (g_apply.plungeRpm ? 1 : 0) << "\n"
      << "plungeFeed=" << (g_apply.plungeFeed ? 1 : 0) << "\n"
      << "ramp=" << (g_apply.ramp ? 1 : 0) << "\n"
      << "rampType=" << (g_apply.rampType ? 1 : 0) << "\n"
      << "rampAngle=" << (g_apply.rampAngle ? 1 : 0) << "\n";
}

static const char* kHostDoneEventId = "HSMAdvisorHostDoneEvent";

typedef std::map<std::string, std::string> KvMap;

// The result as key=value text (see planHostResult), produced on the worker thread and
// consumed by the custom-event handler on the main thread. Only one request is in flight at a time.
static std::string g_pendingResponse;

// ---- HSMAdvisor integration client -------------------------------------------------------------
// HSMAdvisor runs as its own process and serves integrations over a named pipe; its dialogs open
// in that process, owned by Fusion's window. HSMAdvisorIntegration.dll is the native client: the
// copy installed with HSMAdvisor is preferred, the one shipped next to this add-in is the fallback.

static const char* kMinimumHSMAdvisor = "3.2.1"; // integration server with the "high_speed_machining" cut key
static hsma_api g_hsma;
static hsma_load_result g_hsmaLoad = HSMA_LOAD_NOT_FOUND;
static hsma_client* g_client = nullptr;

// The request in flight and its cancellation token. Both are only touched on Fusion's main thread
// (run, stop, the command handler); the worker only uses the token it was given.
static std::thread g_worker;
static hsma_cancel* g_cancel = nullptr;
static std::atomic<bool> g_stopping(false);

// This add-in's version (from its manifest).
static std::string pluginVersion()
{
    std::string version = "0.0.0";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((addinFolder() + "\\*.manifest").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return version;
    do
    {
        std::ifstream f((addinFolder() + "\\" + fd.cFileName).c_str(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        std::smatch m;
        if (std::regex_search(text, m, std::regex("\"version\"\\s*:\\s*\"([^\"]+)\"")))
        {
            version = m[1];
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return version;
}

// ---- update check (GitHub Releases) ----------------------------------------------------------

static std::mutex g_updateLock;
static std::string g_updateVersion, g_updateUrl; // set when a newer release exists
static std::thread g_updateThread;
static bool g_updateStarted = false;

static std::vector<int> parseVer(std::string s)
{
    s.erase(0, s.find_first_not_of("vV"));
    std::vector<int> r(3, 0);
    std::istringstream ss(s);
    std::string part;
    for (int i = 0; i < 3 && std::getline(ss, part, '.'); ++i)
        r[i] = std::atoi(part.c_str()); // stops at the first non-digit ("1-beta" -> 1)
    return r;
}

static int compareVersions(const std::string& a, const std::string& b)
{
    std::vector<int> pa = parseVer(a), pb = parseVer(b);
    for (int i = 0; i < 3; ++i)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

// HTTPS GET with short timeouts; "" on any failure.
static std::string httpsGet(const wchar_t* host, const wchar_t* path)
{
    std::string body;
    HINTERNET session = WinHttpOpen(L"HSMAdvisorPlugin-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request)
    {
        WinHttpSetTimeouts(request, 3000, 3000, 3000, 3000);
        DWORD statusCode = 0, size = sizeof(statusCode);
        if (WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200)
        {
            char buf[8192];
            DWORD read = 0;
            while (WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0)
                body.append(buf, read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return body;
}

static void checkForUpdate()
{
    json release = json::parse(httpsGet(L"api.github.com",
        L"/repos/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360/releases/latest"), nullptr, false);
    if (!release.is_object() || !release.contains("tag_name") || !release["tag_name"].is_string()) return;
    std::string tag = release["tag_name"].get<std::string>();
    if (compareVersions(tag, pluginVersion()) <= 0) return;

    std::string url;
    if (release.contains("assets") && release["assets"].is_array())
    {
        for (const json& asset : release["assets"])
        {
            if (!asset.is_object() || !asset.contains("browser_download_url") || !asset["browser_download_url"].is_string())
                continue;
            std::string u = asset["browser_download_url"].get<std::string>();
            if (u.size() < 4 || toLower(u.substr(u.size() - 4)) != ".exe") continue;
            url = u;
            if (toLower(u).find("setup") != std::string::npos) break;
        }
    }
    std::lock_guard<std::mutex> guard(g_updateLock);
    g_updateVersion = tag;
    g_updateUrl = url;
}

// Starts the update check once per session (skipped when no_update_check.txt exists).
static void startUpdateCheck()
{
    std::lock_guard<std::mutex> guard(g_updateLock);
    if (g_updateStarted) return;
    g_updateStarted = true;
    if (GetFileAttributesA((addinFolder() + "\\no_update_check.txt").c_str()) != INVALID_FILE_ATTRIBUTES) return;
    g_updateThread = std::thread(checkForUpdate);
}

// ---- requests -------------------------------------------------------------------------------

struct HsmResult
{
    int status = HSMA_ERROR;
    json payload;
    std::string code, message;
};

// One request to HSMAdvisor (blocking; worker thread only).
static HsmResult hsmRequest(const char* type, const json* payload, long long ownerWindow, hsma_cancel* cancel)
{
    HsmResult r;
    char* p = nullptr;
    char* e = nullptr;
    std::string text = payload ? payload->dump(-1, ' ', false, json::error_handler_t::replace) : std::string();
    r.status = g_hsma.request(g_client, type, payload ? text.c_str() : nullptr, ownerWindow, cancel, &p, &e);
    if (p) r.payload = json::parse(p, nullptr, false);
    if (e)
    {
        json err = json::parse(e, nullptr, false);
        if (err.is_object())
        {
            r.code = err.value("code", "");
            r.message = err.value("message", "");
        }
    }
    g_hsma.free_memory(p);
    g_hsma.free_memory(e);
    if (r.status == HSMA_OK && !r.payload.is_object())
    {
        r.status = HSMA_ERROR;
        r.code = HSMA_ERR_IO;
        r.message = "HSMAdvisor sent an invalid response.";
    }
    return r;
}

// Failures of the connection itself (as opposed to a message HSMAdvisor rejected).
static bool isClientFailure(const HsmResult& r)
{
    return r.code == HSMA_ERR_NOT_INSTALLED || r.code == HSMA_ERR_VERSION_TOO_OLD ||
           r.code == HSMA_ERR_TIMEOUT || r.code == HSMA_ERR_IO;
}

static std::string friendlyError(const HsmResult& r)
{
    if (r.status == HSMA_BUSY)
        return "HSMAdvisor already has a dialog open. Finish or close it, then try again.";
    if (r.code == HSMA_ERR_NOT_INSTALLED)
        return "HSMAdvisor could not be started. Make sure HSMAdvisor is installed and licensed "
               "(https://hsmadvisor.com/download), then try again.";
    if (r.code == HSMA_ERR_TIMEOUT)
        return "HSMAdvisor did not start in time. Please try again.";
    // version_too_old: the message names both versions and the download page
    return r.message.empty() ? "HSMAdvisor returned an error (" + r.code + ")." : r.message;
}

// A string field of a JSON record ("" when absent); numbers are returned as text.
static std::string jstr(const json& o, const char* key)
{
    if (!o.is_object()) return "";
    auto it = o.find(key);
    if (it == o.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number()) return it->dump();
    return "";
}

static std::string trimmed(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// ---- Fusion tool -> HSMAdvisor tool -------------------------------------------------------------

// HSMAdvisor tool type ids (name_id).
enum HsmToolType
{
    TT_SolidEndMill = 1, TT_IndexedFaceMill = 5, TT_VbitEngraver = 6, TT_JobberTwistDrill = 9,
    TT_Reamer = 10, TT_ThreadMill = 15, TT_Tap = 19, TT_Counterbore = 21, TT_ChamferMill = 22,
    TT_SolidBallMill = 24, TT_IndexedBallMill = 25, TT_BoringHead = 26, TT_WoodRuff = 27,
    TT_SpotDrill = 29, TT_CounterSink = 30, TT_Unknown = 99
};

// Maps a Fusion tool type to the nearest HSMAdvisor one.
// Notes: the chamfer mill is also Fusion's engraving tool, so engraving is told apart by the
// operation strategy. Types with no equivalent map to Unknown, which writes no geometry at all
// (see fieldsFor) and leaves the dialog for the user to fill in.
static int mapToolType(const std::string& type, const std::string& strategy)
{
    std::string t = toLower(type);
    bool engrave = toLower(strategy).find("engrave") != std::string::npos;

    // --- mills ---
    if (t == "flat end mill" || t == "bull nose end mill" || t == "tapered mill" || t == "form mill") return TT_SolidEndMill;
    if (t == "ball end mill" || t == "circle segment taper") return TT_SolidBallMill;
    if (t == "lollipop mill") return TT_IndexedBallMill;
    if (t == "face mill") return TT_IndexedFaceMill;
    if (t == "thread mill") return TT_ThreadMill;
    if (t == "dovetail mill" || t == "slot mill") return TT_WoodRuff; // nearest to a T-slot cutter
    // Same tool for chamfering and engraving; tell them apart by the operation.
    if (t == "chamfer mill" || t == "corner chamfer end mill") return engrave ? TT_VbitEngraver : TT_ChamferMill;

    // --- drilling ---
    if (t == "drill" || t == "block drill") return TT_JobberTwistDrill;
    if (t == "spot drill" || t == "center drill") return TT_SpotDrill;
    if (t == "counter bore") return TT_Counterbore;
    if (t == "counter sink") return TT_CounterSink;
    if (t == "reamer") return TT_Reamer;
    if (t == "boring bar") return TT_BoringHead;
    if (t == "tap right hand" || t == "tap left hand") return TT_Tap;

    // radius mill, circle segment barrel/oval/lens, turning, jet, probe...: no HSMAdvisor equivalent
    return TT_Unknown;
}

// HSMAdvisor tool material ids (tool_material_id).
static int mapToolMaterial(const std::string& material)
{
    std::string m = toLower(material);
    if (m.find("carbide") != std::string::npos) return 5;
    if (m.find("cobalt") != std::string::npos) return 4;
    if (m.find("hss") != std::string::npos || m.find("high speed steel") != std::string::npos) return 1;
    if (m.find("ceramic") != std::string::npos) return 10;
    if (m.find("pcd") != std::string::npos) return 7;
    return 5;
}

// Which geometry fields each HSMAdvisor tool type actually exposes.
enum GeoField
{
    G_Dia = 1, G_TipDia = 2, G_Flutes = 4, G_FluteLen = 8, G_ShoulderLen = 16, G_ShoulderDia = 32,
    G_Shank = 128, G_Corner = 256, G_Angle = 512, G_Pitch = 1024, G_Stickout = 2048,
    G_All = G_Dia | G_TipDia | G_Flutes | G_FluteLen | G_ShoulderLen | G_ShoulderDia | G_Shank |
            G_Corner | G_Angle | G_Pitch | G_Stickout
};

// Helix angle is deliberately never written for any type (HSMAdvisor uses the type's default).
static int fieldsFor(int tt)
{
    switch (tt)
    {
    case TT_Tap:           return 0; // nothing is written for a tap for now
    case TT_Unknown:       return 0; // unmapped Fusion types: write nothing
    case TT_CounterSink:   return G_Dia | G_TipDia | G_Flutes | G_Shank | G_Angle | G_Stickout;
    case TT_ChamferMill:
    case TT_VbitEngraver:
    case TT_JobberTwistDrill:
    case TT_SpotDrill:     return G_Dia | G_Flutes | G_FluteLen | G_Shank | G_Angle | G_Stickout;
    case TT_Reamer:
    case TT_Counterbore:   return G_Dia | G_Flutes | G_Stickout;
    // Turn dia = diameter, insert length = flute length, "holder size" = shank diameter.
    case TT_BoringHead:    return G_Dia | G_Flutes | G_FluteLen | G_Shank | G_Stickout;
    case TT_ThreadMill:    return G_Dia | G_Flutes | G_FluteLen | G_ShoulderLen | G_ShoulderDia | G_Shank | G_Pitch | G_Stickout;
    case TT_SolidEndMill:
    case TT_IndexedFaceMill:
    case TT_WoodRuff:      return G_Dia | G_Flutes | G_FluteLen | G_ShoulderLen | G_ShoulderDia | G_Shank | G_Corner | G_Angle | G_Stickout;
    case TT_SolidBallMill:
    case TT_IndexedBallMill: return G_Dia | G_Flutes | G_FluteLen | G_ShoulderLen | G_ShoulderDia | G_Shank | G_Angle | G_Stickout;
    default:               return G_All;
    }
}

// What the add-in read from the selected operation (lengths in mm, angles in degrees).
struct HsmRequest
{
    std::string description, productId, strategy, toolType, toolMaterial;
    double diameter = 0, tipDiameter = 0, cornerRadius = 0, taperAngle = 0, tipAngle = 0;
    double shoulderDiameter = 0, threadPitch = 0, fluteLength = 0, shoulderLength = 0, shaftDiameter = 0;
    double stickout = 0, docIn = 0, wocIn = 0, rampAngle = 0;
    int flutes = 0;
    bool rampHelix = false;
    long long ownerWindow = 0;
};

// A metric length: the value plus its unit flag (record values are strings).
static void setMm(json& record, const char* name, const char* flag, double mm)
{
    record[name] = numToStr(mm, 4);
    record[flag] = "1";
}

// The tool record from Fusion's geometry.
static json toolGeometry(const HsmRequest& r)
{
    json tool = json::object();
    int tt = mapToolType(r.toolType, r.strategy);
    int has = fieldsFor(tt);
    if (tt != TT_Unknown) tool["name_id"] = std::to_string(tt); // unknown: HSMAdvisor's default type, the user picks
    tool["tool_material_id"] = std::to_string(mapToolMaterial(r.toolMaterial));
    tool["coating_id"] = "1"; // none
    // Name the tool as Fusion does (else HSMAdvisor generates one): description, else product id.
    std::string desc = trimmed(r.description), pid = trimmed(r.productId);
    if (!desc.empty() || !pid.empty()) tool["comment"] = desc.empty() ? pid : desc;
    if (!pid.empty()) tool["supplier_pid"] = pid;

    if ((has & G_Flutes) && r.flutes > 0) tool["Flute_N"] = std::to_string(r.flutes);
    if ((has & G_FluteLen) && r.fluteLength > 0) setMm(tool, "Flute_Len", "flute_len_mm", r.fluteLength);
    if ((has & G_ShoulderLen) && r.shoulderLength > 0) setMm(tool, "Shoulder_Len", "shoulder_len_mm", r.shoulderLength);
    if ((has & G_ShoulderDia) && r.shoulderDiameter > 0) setMm(tool, "Shoulder_Dia", "Shoulder_Dia_mm", r.shoulderDiameter);
    // Shank_Dia is shown as "holder size" on a boring head; it still takes Fusion's shaft diameter.
    if ((has & G_Shank) && r.shaftDiameter > 0) setMm(tool, "Shank_Dia", "Shank_Dia_mm", r.shaftDiameter);
    if ((has & G_Corner) && r.cornerRadius > 0) setMm(tool, "corner_rad", "corner_rad_mm", r.cornerRadius);
    if ((has & G_Pitch) && r.threadPitch > 0)
    {
        tool["thread_pitch"] = numToStr(r.threadPitch, 4);
        tool["thread_pitch_m"] = "1";
    }

    std::string tl = toLower(r.toolType);
    bool isThread = tl.find("thread") != std::string::npos;
    bool isPointTool = tl.find("counter sink") != std::string::npos || tl.find("drill") != std::string::npos;
    if (!isThread && (has & G_Angle))
    {
        // HSMAdvisor stores a lead angle plus the mode the UI shows it in. Point tools (countersink,
        // drills) are defined by their tip angle; everything else by the taper, written even when 0
        // so the tool cannot keep HSMAdvisor's default lead angle.
        if (isPointTool && r.tipAngle > 0)
        {
            tool["leadangle_mode"] = "2"; // tip
            tool["leadangle"] = numToStr(90.0 - r.tipAngle / 2.0, 4);
        }
        else
        {
            double taper = tl.find("dovetail") != std::string::npos ? -r.taperAngle : r.taperAngle;
            tool["leadangle_mode"] = "1"; // taper
            tool["leadangle"] = numToStr(90.0 - taper, 4);
        }
    }

    // HSMAdvisor's single diameter field is labelled "tip diameter", but for most types that is
    // really the tool diameter. Use Fusion's tip diameter only for the genuinely tapered types.
    bool useTipDia = tt == TT_ChamferMill || tt == TT_VbitEngraver;
    // Pointed tools (V-bits) report a 0 tip diameter, which HSMAdvisor rejects: use a tiny one.
    double dia = useTipDia ? (r.tipDiameter > 0.001 ? r.tipDiameter : 0.001) : r.diameter;
    setMm(tool, "diameter", "diameter_mm", dia);
    if (has & G_TipDia) setMm(tool, "Tip_Diameter", "Tip_Diameter_mm", r.tipDiameter > 0 ? r.tipDiameter : r.diameter);
    return tool;
}

// Finds the tool in the user's HSMAdvisor library by exact name: description, then product id.
// Returns false (tool untouched) when it is not there; sets error on a connection failure.
static bool findLibraryTool(const HsmRequest& r, hsma_cancel* cancel, json& tool, HsmResult& failure)
{
    for (const std::string* name : { &r.description, &r.productId })
    {
        std::string key = trimmed(*name);
        if (key.empty()) continue;
        json query = { { "comment", key } };
        HsmResult hits = hsmRequest("FindTools", &query, 0, cancel);
        if (hits.status == HSMA_CANCELLED || (hits.status != HSMA_OK && isClientFailure(hits)))
        {
            failure = hits;
            return false;
        }
        if (hits.status != HSMA_OK || !hits.payload.contains("tools") || !hits.payload["tools"].is_array() ||
            hits.payload["tools"].empty())
            continue;

        json get = { { "guid", jstr(hits.payload["tools"][0], "guid") } };
        HsmResult loaded = hsmRequest("GetTool", &get, 0, cancel);
        if (loaded.status == HSMA_CANCELLED || (loaded.status != HSMA_OK && isClientFailure(loaded)))
        {
            failure = loaded;
            return false;
        }
        if (loaded.status == HSMA_OK && loaded.payload.contains("tool") && loaded.payload["tool"].is_object())
        {
            tool = loaded.payload["tool"];
            tool.erase("Cuts"); // the library tool's saved cuts are not part of a calculation
            return true;
        }
    }
    return false;
}

// ---- last-used settings ------------------------------------------------------------------------
// Material / machine / productivity / HSM mode / units the user last chose, in the add-in folder.

struct LastSettings
{
    std::string materialId, machineId, productivity, hss;
    bool metric = true;
};

static std::string settingsPath() { return addinFolder() + "\\settings.txt"; }

static std::string readFile(const std::string& path)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// <tag>value</tag> of the settings.xml earlier versions wrote.
static std::string xmlTag(const std::string& text, const std::string& tag)
{
    size_t b = text.find("<" + tag + ">");
    if (b == std::string::npos) return "";
    b += tag.size() + 2;
    size_t e = text.find("</" + tag + ">", b);
    return e == std::string::npos ? "" : trimmed(text.substr(b, e - b));
}

static LastSettings loadLastSettings()
{
    LastSettings s;
    std::string text = readFile(settingsPath());
    if (!text.empty())
    {
        KvMap m = parseKvText(text);
        s.materialId = kvGet(m, "material_id", "");
        s.machineId = kvGet(m, "machine_id", "");
        s.productivity = kvGet(m, "productivity", "");
        s.hss = kvGet(m, "hss", "");
        s.metric = kvGet(m, "metric", "1") == "1";
        return s;
    }
    std::string xml = readFile(addinFolder() + "\\settings.xml"); // written by versions up to 0.4
    if (!xml.empty())
    {
        s.materialId = xmlTag(xml, "material_id");
        s.machineId = xmlTag(xml, "machine_id");
        s.productivity = xmlTag(xml, "productivity");
        s.hss = xmlTag(xml, "hsm");
        s.metric = xmlTag(xml, "metric") != "0";
    }
    return s;
}

static void saveLastSettings(const json& cut)
{
    std::string material = jstr(cut, "material_id");
    std::ofstream f(settingsPath().c_str(), std::ios::binary);
    if (!f) return;
    if (!material.empty() && material != "0") f << "material_id=" << material << "\n";
    if (!jstr(cut, "machine_id").empty()) f << "machine_id=" << jstr(cut, "machine_id") << "\n";
    if (!jstr(cut, "productivity").empty()) f << "productivity=" << jstr(cut, "productivity") << "\n";
    f << "hss=" << (jstr(cut, "high_speed_machining") == "1" ? "1" : "0") << "\n";
    f << "metric=" << (jstr(cut, "input_units_mm") == "0" ? "0" : "1") << "\n";
}

// ---- the request ---------------------------------------------------------------------------------

// Seeds HSMAdvisor with the operation's tool and cut, lets the user edit it in the HSMAdvisor
// calculator and returns the result (mm, mm/min) as the key=value map planHostResult consumes.
static KvMap processRequest(const HsmRequest& r, hsma_cancel* cancel)
{
    KvMap out;
    out["status"] = "error";
    if (g_hsmaLoad != HSMA_LOAD_OK || !g_client)
    {
        out["error"] = g_hsmaLoad == HSMA_LOAD_TOO_OLD
            ? "The HSMAdvisor integration component (HSMAdvisorIntegration.dll) is outdated. Update HSMAdvisor or reinstall the add-in."
            : "The HSMAdvisor integration component (HSMAdvisorIntegration.dll) is missing. Reinstall the add-in.";
        return out;
    }
    startUpdateCheck();

    auto fail = [&](const HsmResult& res) {
        if (res.status == HSMA_CANCELLED) out["status"] = "cancel";
        else out["error"] = friendlyError(res);
        return out;
    };

    // 1. The tool: the user's library tool when it is there (its saved data), else Fusion's geometry.
    json tool;
    HsmResult failure;
    failure.status = HSMA_OK;
    bool found = findLibraryTool(r, cancel, tool, failure);
    if (failure.status != HSMA_OK) return fail(failure);
    if (!found) tool = toolGeometry(r);

    // 2. The cut. Stick-out is editable even for a library tool, so take Fusion's (body length):
    // deflection and rigidity then reflect the real setup. Skipped for types that don't take it.
    json cut = json::object();
    if (r.stickout > 0 && (fieldsFor(mapToolType(r.toolType, r.strategy)) & G_Stickout))
        setMm(tool, "stickout", "stickout_mm", r.stickout);
    // The cut's diameter and stick-out win over the tool's, and an absent cut unit flag would turn
    // the tool's millimetres into inches: send them on the cut too, with the tool's flags.
    for (const char* name : { "diameter", "stickout" })
    {
        std::string flag = std::string(name) + "_mm";
        if (jstr(tool, name).empty()) continue;
        cut[name] = jstr(tool, name);
        cut[flag] = jstr(tool, flag.c_str()).empty() ? "0" : jstr(tool, flag.c_str());
    }
    // The operation's engagement seeds HSMAdvisor's cut.
    if (r.docIn > 0) setMm(cut, "doc", "doc_mm", r.docIn);
    if (r.wocIn > 0) setMm(cut, "woc", "woc_mm", r.wocIn);
    if (r.rampAngle > 0) cut["ramp"] = numToStr(r.rampAngle, 4); // degrees
    if (r.rampHelix)
    {
        cut["rampmilling"] = "0";
        cut["circle_comp"] = "1";
    }

    // Open where the previous run left off: material, machine, productivity, HSM, units.
    LastSettings last = loadLastSettings();
    cut["material_id"] = last.materialId.empty() ? "227" : last.materialId; // default until one is picked
    cut["productivity"] = last.productivity.empty() ? "4" : last.productivity; // slider -> x1.0
    if (!last.machineId.empty()) cut["machine_id"] = last.machineId;
    if (!last.hss.empty()) cut["high_speed_machining"] = last.hss;
    cut["input_units_mm"] = last.metric ? "1" : "0";
    cut["result_units_mm"] = last.metric ? "1" : "0";

    // 3. The HSMAdvisor calculator, owned by Fusion's window.
    std::string prompt = "Select workpiece material and machine, then click OK to apply to Fusion.";
    if (!found)
    {
        std::string desc = trimmed(r.description), pid = trimmed(r.productId);
        prompt = "Tool not found in your HSMAdvisor library (" + (desc.empty() ? std::string("no description") : desc) +
                 (pid.empty() ? std::string() : ", " + pid) + "). Check the tool and cut values, then click OK to apply to Fusion.";
    }
    // tool_is_new defaults to true: a library tool must say it is not, or auto-save adds it twice.
    json edit = { { "calculation", { { "tool", tool }, { "cut", cut }, { "tool_is_new", !found } } }, { "prompt", prompt }, { "calculate_first", true } };
    HsmResult edited = hsmRequest("EditCalculation", &edit, r.ownerWindow, cancel);
    if (edited.status != HSMA_OK) return fail(edited);
    if (!edited.payload.contains("calculation") || !edited.payload["calculation"].is_object())
    {
        out["error"] = "HSMAdvisor returned no calculation.";
        return out;
    }
    json calculation = edited.payload["calculation"];
    if (calculation.contains("cut")) saveLastSettings(calculation["cut"]);

    // 4. Always report millimetres: the add-in writes them back with mm unit tokens.
    json convert = { { "calculation", calculation }, { "metric", true } };
    HsmResult metric = hsmRequest("ConvertUnits", &convert, 0, cancel);
    if (metric.status != HSMA_OK) return fail(metric);
    json result = metric.payload.contains("calculation") ? metric.payload["calculation"] : json::object();
    json resultCut = result.is_object() && result.contains("cut") ? result["cut"] : json::object();

    out["status"] = "ok";
    out["error"] = "";
    out["rpm"] = jstr(resultCut, "rpm");
    out["rpmPlunge"] = jstr(resultCut, "rpm_plunge");
    out["feedCut"] = jstr(resultCut, "feed");
    out["feedPlunge"] = jstr(resultCut, "feed_plunge");
    out["doc"] = jstr(resultCut, "doc");
    out["woc"] = jstr(resultCut, "woc");
    out["peck"] = jstr(resultCut, "peck");
    out["sfm"] = jstr(resultCut, "sfm");
    out["chipload"] = jstr(resultCut, "ipt");
    out["rampAngleOut"] = jstr(resultCut, "ramp");

    // Attach update info if a newer release was found by now.
    std::lock_guard<std::mutex> guard(g_updateLock);
    if (!g_updateVersion.empty())
    {
        out["updateVersion"] = g_updateVersion;
        out["currentVersion"] = pluginVersion();
        if (!g_updateUrl.empty()) out["updateUrl"] = g_updateUrl;
    }
    return out;
}

static std::string toKvText(const KvMap& m)
{
    std::string text;
    for (const auto& kv : m)
    {
        std::string v = kv.second;
        std::replace(v.begin(), v.end(), '\r', ' ');
        std::replace(v.begin(), v.end(), '\n', ' ');
        text += kv.first + "=" + v + "\n";
    }
    return text;
}

// Fusion's main window, which owns the HSMAdvisor dialog so it stays in front of Fusion.
static BOOL CALLBACK findTopLevelWindow(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER))
    {
        *(HWND*)param = hwnd;
        return FALSE;
    }
    return TRUE;
}

static long long fusionMainWindow()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == GetCurrentProcessId())
        return (long long)(INT_PTR)GetAncestor(fg, GA_ROOTOWNER);
    HWND found = nullptr;
    EnumWindows(findTopLevelWindow, (LPARAM)&found);
    return (long long)(INT_PTR)found;
}

// Waits for the previous request's worker and releases its token (main thread).
static void finishWorker()
{
    if (g_worker.joinable()) g_worker.join();
    if (g_cancel)
    {
        g_hsma.cancel_destroy(g_cancel);
        g_cancel = nullptr;
    }
}

// Runs the request on a worker thread, then fires the custom event so the result is applied on
// Fusion's main thread. Never blocks Fusion's UI thread.
static void runHsmAdvisorAsync(const HsmRequest& request)
{
    finishWorker(); // one request at a time: the previous one already delivered its result
    g_cancel = g_hsmaLoad == HSMA_LOAD_OK ? g_hsma.cancel_create() : nullptr;
    hsma_cancel* cancel = g_cancel;
    g_worker = std::thread([request, cancel]()
    {
        g_pendingResponse = toKvText(processRequest(request, cancel));
        if (app && !g_stopping)
            app->fireCustomEvent(kHostDoneEventId, "");
    });
}

// Connects the add-in to the integration client (run()).
static void startHsmAdvisorClient()
{
    g_stopping = false; // the add-in can be stopped and run again in one Fusion session
    std::wstring bundled = utf8ToWide(addinFolder() + "\\HSMAdvisorIntegration.dll");
    g_hsmaLoad = hsma_load(&g_hsma, bundled.c_str());
    if (g_hsmaLoad != HSMA_LOAD_OK) return;
    g_client = g_hsma.client_create("HSMAdvisor Plugin for Fusion 360", pluginVersion().c_str());
    if (g_client) g_hsma.client_set_minimum_version(g_client, kMinimumHSMAdvisor);
}

// Closes an open HSMAdvisor dialog, waits for the request to end and releases the client (stop()).
// HSMAdvisor itself exits a few seconds after Fusion is gone.
static void stopHsmAdvisorClient()
{
    g_stopping = true;
    if (g_cancel) g_hsma.cancel_signal(g_cancel);
    finishWorker();
    if (g_updateThread.joinable()) g_updateThread.join();
    if (g_client)
    {
        g_hsma.client_destroy(g_client);
        g_client = nullptr;
    }
    if (g_hsmaLoad == HSMA_LOAD_OK) hsma_unload(&g_hsma);
    g_hsmaLoad = HSMA_LOAD_NOT_FOUND;
}

// Finds the CAM operation among the current selections.
static Ptr<Operation> selectedOperation()
{
    Ptr<Selections> sels = ui->activeSelections();
    size_t count = sels ? sels->count() : 0;
    for (size_t i = 0; i < count; ++i)
    {
        Ptr<Selection> sel = sels->item(i);
        Ptr<Base> ent = sel ? sel->entity() : nullptr;
        Ptr<Operation> op = ent;
        if (op) return op;
    }
    return nullptr;
}

// State shared between the launch main thread, the host-done handler, and the
// results dialog (all on the main thread).
static bool g_hostBusy = false;
static Ptr<Operation> g_pendingOp;
static std::map<std::string, std::string> g_pendingResult; // last host result, for the results dialog
static const char* kApplyCmdId = "HSMAdvisorApplyCmd";
static Ptr<CommandDefinition> g_applyCmdDef;

// A single results dialog: one table row per proposed change, each with its own
// checkbox, showing the current (old) and proposed (new) value.
struct PendingWrite { std::string name, expr; };  // parameter + expression to set
struct PlanItem
{
    std::string cbId;        // checkbox input id for this row
    std::string category;    // rpm / feed / ad / ae -- for the persisted default
    std::string name;        // raw CAM parameter id (for old-value lookup + tooltip)
    std::string label;       // name shown in the table
    std::string oldv, newv;  // current / proposed value (display units)
    std::string oldExpr;     // current expression/variable (tooltip for the Old cell)
    std::vector<PendingWrite> writes; // what to write if this row is checked
    bool defaultOn;          // initial checkbox state (from saved prefs)
};
static std::vector<PlanItem> g_plan;
static std::vector<std::string> g_planNotes; // parameters that can't be set here
static std::string g_planOpName;

// If the host reported a newer release, offer a one-click update (once per session).
static bool g_updateOffered = false;
static void maybeOfferUpdate(const std::map<std::string, std::string>& out)
{
    if (g_updateOffered) return;
    std::string ver = kvGet(out, "updateVersion", "");
    if (ver.empty()) return;
    g_updateOffered = true; // ask at most once per session, whatever the answer

    std::string cur = kvGet(out, "currentVersion", "");
    std::string url = kvGet(out, "updateUrl", "");
    const std::string relPage =
        "https://github.com/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360/releases/latest";

    std::string msg = "A new version of HSMAdvisor Plugin is available.\n\n";
    msg += "Installed: " + (cur.empty() ? std::string("(unknown)") : cur) + "\n";
    msg += "Available: " + ver + "\n\n";
    msg += url.empty()
        ? "Open the download page in your browser?"
        : "Download and run the installer now?\nYou will need to close Fusion 360 for it to finish.";

    DialogResults ans = ui->messageBox(msg, "HSMAdvisor Plugin update",
                                       YesNoButtonType, QuestionIconType);
    if (ans != DialogYes) return;

    if (url.empty())
    {
        ShellExecuteW(nullptr, L"open", utf8ToWide(relPage).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    char tmp[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tmp);
    std::string dest = std::string(tmp) + "HSMAdvisor-Plugin-Setup.exe";

    HRESULT hr = URLDownloadToFileW(nullptr, utf8ToWide(url).c_str(),
                                    utf8ToWide(dest).c_str(), 0, nullptr);
    if (SUCCEEDED(hr))
    {
        ShellExecuteW(nullptr, L"open", utf8ToWide(dest).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ui->messageBox(
            "The installer has started.\n\n"
            "Please CLOSE Fusion 360 so it can update the plugin, then restart Fusion.",
            "HSMAdvisor Plugin update");
    }
    else
    {
        // Download failed (offline, etc.) -> fall back to the release page.
        ShellExecuteW(nullptr, L"open", utf8ToWide(relPage).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// Regenerates the toolpath for a single operation, so
// the user doesn't have to regenerate by hand.
static void regenerateOperation(const Ptr<Operation>& op)
{
    if (!op)
        return;
    Ptr<Document> doc = app ? app->activeDocument() : nullptr;
    Ptr<Products> products = doc ? doc->products() : nullptr;
    Ptr<CAM> cam = products ? products->itemByProductType("CAMProductType") : nullptr;
    if (cam)
        cam->generateToolpath(op);
}

// Builds the results-table preview (parameter, old, new) and the list of parameter writes to apply later.
static void planHostResult(const Ptr<Operation>& op, const std::map<std::string, std::string>& out)
{
    if (!op)
        return;

    // SFM is returned by the host but intentionally not applied for now.
    int    rpm        = (int)strtod(kvGet(out, "rpm", "0").c_str(), nullptr);
    int    rpmPlunge  = (int)strtod(kvGet(out, "rpmPlunge", "0").c_str(), nullptr);
    double feedCut    = strtod(kvGet(out, "feedCut", "0").c_str(), nullptr);
    double feedPlunge = strtod(kvGet(out, "feedPlunge", "0").c_str(), nullptr);
    double doc        = strtod(kvGet(out, "doc", "0").c_str(), nullptr);
    double woc        = strtod(kvGet(out, "woc", "0").c_str(), nullptr);
    double peck       = strtod(kvGet(out, "peck", "0").c_str(), nullptr);
    double rampOut    = strtod(kvGet(out, "rampAngleOut", "0").c_str(), nullptr); // HSM ramp angle

    Ptr<CAMParameters> ops = op->parameters();
    if (!ops)
    {
        ui->messageBox("The operation is no longer available to update.", "HSMAdvisor Plugin");
        return;
    }

    // Expressions to actually write (mm / rpm / mmpm unit tokens; Fusion converts them).
    std::string rpmExpr = numToStr((double)rpm, 0) + "rpm";
    std::string cutExpr = numToStr(feedCut, 1) + "mmpm";

    // If is drilling operations then set feed as plunge feed, otherwise set as cutting feed.
    bool isDrill = (toLower(op->strategy()) == "drill");
    bool isChamfer = isChamferEngagement(op);
    const std::string feedParam = isDrill ? "tool_feedPlunge" : "tool_feedCutting";

    bool metric = readBool(ops, "metric", true);
    double disp = metric ? 1.0 : 1.0 / 25.4;
    const char* lenUnit  = metric ? "mm" : "in";
    const char* feedUnit = metric ? "mm/min" : "in/min";
    int lenPrec  = metric ? 3 : 4;
    int feedPrec = metric ? 1 : 2;
    auto lenNew  = [&](double mm)   { return numToStr(mm * disp, lenPrec) + " " + lenUnit; };

    g_plan.clear();
    g_planNotes.clear();
    g_planOpName = op->name();

    // Current value for the Old column: always the evaluated result value(), formatted in
    // the document's display units (the raw expression/variable goes in the cell tooltip).
    auto oldDisplay = [&](const std::string& category, const std::string& name) -> std::string
    {
        if (category == "rampType") return readChoice(ops, name, readExpr(ops, name));
        double val;
        if (!readFloatVal(ops, name, val)) return readExpr(ops, name); // no numeric value
        if (category == "rpm" || category == "plungeRpm") return numToStr(val, 0) + " rpm";
        if (category == "rampAngle") return numToStr(val, 3) + " deg"; // val already in degrees
        bool isFeed = (category == "feed" || category == "plungeFeed" || category == "ramp");
        double display = isFeed ? (val * disp) : (val * 10.0 * disp);
        return numToStr(display, isFeed ? feedPrec : lenPrec) + " " + (isFeed ? feedUnit : lenUnit);
    };

    // Adds a checkbox row to the plan. defaultOn comes from the saved per-category prefs.
    // oldExpr keeps the raw expression/variable for the Old cell's tooltip.
    auto add = [&](const std::string& category, const std::string& name,
                   const std::string& newDisplay, std::vector<PendingWrite> writes, bool defaultOn)
    {
        PlanItem it;
        it.cbId = "hsm_row" + std::to_string(g_plan.size());
        it.category = category;
        it.name = name;
        it.label = friendlyParamName(name);
        it.oldv = oldDisplay(category, name);
        it.oldExpr = readExpr(ops, name);
        it.newv = newDisplay;
        it.writes = std::move(writes);
        it.defaultOn = defaultOn;
        g_plan.push_back(std::move(it));
    };

    if (rpm > 0 && hasParam(ops, "tool_spindleSpeed"))
        add("rpm", "tool_spindleSpeed", numToStr((double)rpm, 0) + " rpm",
            { { "tool_spindleSpeed", rpmExpr } }, g_apply.all || g_apply.rpm);

    if (feedCut > 0.0 && hasParam(ops, feedParam))
    {
        // The row shows the primary feed parameter; for milling the same value also goes
        // to entry/exit/transition.
        std::vector<PendingWrite> w;
        if (isDrill)
            w.push_back({ "tool_feedPlunge", cutExpr });
        else
            for (const char* fn : { "tool_feedCutting", "tool_feedEntry",
                                    "tool_feedExit", "tool_feedTransition" })
                w.push_back({ fn, cutExpr });
        add("feed", feedParam, numToStr(feedCut * disp, feedPrec) + " " + feedUnit,
            w, g_apply.all || g_apply.feed);
    }

    // Plunge feed (HSMAdvisor FEED_Plunge) -> Fusion plunge & ramp feeds.
    if (!isDrill && feedPlunge > 0.0)
    {
        std::string plungeExpr = numToStr(feedPlunge, 1) + "mmpm";
        std::string plungeDisp = numToStr(feedPlunge * disp, feedPrec) + " " + feedUnit;
        if (hasParam(ops, "tool_feedPlunge"))
            add("plungeFeed", "tool_feedPlunge", plungeDisp,
                { { "tool_feedPlunge", plungeExpr } }, g_apply.all || g_apply.plungeFeed);
        if (hasParam(ops, "tool_feedRamp"))
            add("ramp", "tool_feedRamp", plungeDisp,
                { { "tool_feedRamp", plungeExpr } }, g_apply.all || g_apply.ramp);
    }

    // Plunge RPM (HSMAdvisor RPM_Plunge -> Fusion ramp spindle speed).
    if (!isDrill && rpmPlunge > 0 && hasParam(ops, "tool_rampSpindleSpeed"))
    {
        std::string rpmPlungeExpr = numToStr((double)rpmPlunge, 0);
        add("plungeRpm", "tool_rampSpindleSpeed", rpmPlungeExpr + " rpm",
            { { "tool_rampSpindleSpeed", rpmPlungeExpr + "rpm" } }, g_apply.all || g_apply.plungeRpm);
    }

    // Switch the operation to a helix ramp and push HSMAdvisor's ramp angle (res.Ramp) into Fusion.
    if (hasParam(ops, "rampType"))
        add("rampType", "rampType", "helix", { { "rampType", "helix" } },
            g_apply.all || g_apply.rampType);
    if (rampOut > 0.0 && hasParam(ops, "rampAngle"))
        add("rampAngle", "rampAngle", numToStr(rampOut, 3) + " deg",
            { { "rampAngle", numToStr(rampOut, 3) + "deg" } }, g_apply.all || g_apply.rampAngle);

    // Depth of cut: milling -> maximumStepdown; drilling -> peckingDepth; chamfer ->
    // chamferTipOffset. Width of cut: milling -> optimalLoad/stepover; chamfer ->
    // chamferWidth.
    if (isDrill)
    {
        if (peck > 0.0)
        {
            if (hasParam(ops, "peckingDepth"))
                add("ad", "peckingDepth", lenNew(peck), { { "peckingDepth", numToStr(peck, 3) + "mm" } }, g_apply.all || g_apply.ad);
            else
                g_planNotes.push_back(friendlyParamName("peckingDepth") + " not set");
        }
    }
    else if (isChamfer)
    {
        // Inverse of the seeding: WOC -> chamferWidth, DOC -> chamferTipOffset, where
        // chamferTipOffset = DOC - chamferWidth / tan(flank angle). effWidth is the width
        // that will be in effect (the new WOC if it is written, else the current width).
        double t = chamferTan(chamferHalfAngleDeg(op->tool() ? op->tool()->parameters() : nullptr));
        double effWidth = (woc > 0.0) ? woc : readLenMm(ops, "chamferWidth", 0.0);
        if (woc > 0.0)
        {
            if (hasParam(ops, "chamferWidth"))
                add("ae", "chamferWidth", lenNew(woc), { { "chamferWidth", numToStr(woc, 3) + "mm" } }, g_apply.all || g_apply.ae);
            else
                g_planNotes.push_back(friendlyParamName("chamferWidth") + " not set");
        }
        if (doc > 0.0)
        {
            if (hasParam(ops, "chamferTipOffset"))
            {
                double depth = (t > 1e-9) ? effWidth / t : 0.0;
                double tip = doc - depth;
                add("ad", "chamferTipOffset", lenNew(tip), { { "chamferTipOffset", numToStr(tip, 3) + "mm" } }, g_apply.all || g_apply.ad);
            }
            else g_planNotes.push_back(friendlyParamName("chamferTipOffset") + " not set");
        }
    }
    else
    {
        if (doc > 0.0)
        {
            if (hasParam(ops, "maximumStepdown"))
                add("ad", "maximumStepdown", lenNew(doc), { { "maximumStepdown", numToStr(doc, 3) + "mm" } }, g_apply.all || g_apply.ad);
            else
                g_planNotes.push_back("no depth param on this strategy");
        }
        if (woc > 0.0)
        {
            std::string wp = firstExistingName(ops, { "optimalLoad", "maximumStepover", "stepover" });
            if (!wp.empty())
                add("ae", wp, lenNew(woc), { { wp, numToStr(woc, 3) + "mm" } }, g_apply.all || g_apply.ae);
            else
                g_planNotes.push_back("no radial param on this strategy");
        }
    }
}

// Fired when the host has returned a result. On success it plans the changes and opens
// the single results dialog (checkbox + old + new per parameter). g_hostBusy stays true
// until that dialog is destroyed.
class HostDoneHandler : public CustomEventHandler
{
public:
    void notify(const Ptr<CustomEventArgs>& /*args*/) override
    {
        std::map<std::string, std::string> out = parseKvText(g_pendingResponse);
        g_pendingResponse.clear();

        std::string status = kvGet(out, "status", "error");
        if (status == "ok")
        {
            g_pendingResult = out;
            planHostResult(g_pendingOp, out);

            if ((!g_plan.empty() || !g_planNotes.empty()) && g_applyCmdDef)
            {
                g_applyCmdDef->execute(); // show the single results dialog
            }
            else
            {
                if (g_plan.empty() && g_planNotes.empty())
                    ui->messageBox("HSMAdvisor returned no values that apply to this operation.",
                                   "HSMAdvisor Plugin");
                g_pendingOp = nullptr;
                g_pendingResult.clear();
                g_hostBusy = false;
            }
        }
        else
        {
            if (status != "cancel")
                ui->messageBox("HSMAdvisor did not return a result:\n" +
                               kvGet(out, "error", "unknown error"), "HSMAdvisor Plugin");
            g_pendingOp = nullptr;
            g_hostBusy = false;
        }
    }
};
static HostDoneHandler g_onHostDone;

// --- single results dialog: a checkbox + old + new per parameter -------------
class ApplyExecuteHandler : public CommandEventHandler
{
public:
    void notify(const Ptr<CommandEventArgs>& eventArgs) override
    {
        Ptr<Command> cmd = eventArgs ? eventArgs->command() : nullptr;
        Ptr<CommandInputs> inputs = cmd ? cmd->commandInputs() : nullptr;
        if (!inputs) return;

        Ptr<CAMParameters> ops = g_pendingOp ? g_pendingOp->parameters() : nullptr;
        std::vector<std::string> failed;

        for (const PlanItem& item : g_plan)
        {
            Ptr<BoolValueCommandInput> cb = inputs->itemById(item.cbId);
            bool on = cb ? cb->value() : item.defaultOn;

            // Remember the choice per category so the next run defaults the same way.
            if      (item.category == "rpm")        g_apply.rpm        = on;
            else if (item.category == "plungeRpm")  g_apply.plungeRpm  = on;
            else if (item.category == "feed")       g_apply.feed       = on;
            else if (item.category == "plungeFeed") g_apply.plungeFeed = on;
            else if (item.category == "ramp")       g_apply.ramp       = on;
            else if (item.category == "rampType")   g_apply.rampType   = on;
            else if (item.category == "rampAngle")  g_apply.rampAngle  = on;
            else if (item.category == "ad")         g_apply.ad         = on;
            else if (item.category == "ae")         g_apply.ae         = on;

            if (on && ops)
                for (const PendingWrite& w : item.writes)
                    if (trySet(ops, w.name, w.expr) == SetResult::Failed)
                        failed.push_back(w.name);
        }
        g_apply.all = false; // per-row checkboxes replace the old master switch
        saveApplyPrefs();

        if (!failed.empty())
        {
            std::string m = "Could not set: ";
            for (size_t i = 0; i < failed.size(); ++i)
                m += (i ? ", " : "") + friendlyParamName(failed[i]);
            ui->messageBox(m, "HSMAdvisor Plugin");
        }

        Ptr<BoolValueCommandInput> regen = inputs->itemById("hsm_regen");
        if (!regen || regen->value())
            regenerateOperation(g_pendingOp);

        // Surface any available plugin update (once per session).
        maybeOfferUpdate(g_pendingResult);
    }
};
static ApplyExecuteHandler g_onApplyExecute;

// Fires on OK and Cancel; always clears the shared state.
class ApplyDestroyHandler : public CommandEventHandler
{
public:
    void notify(const Ptr<CommandEventArgs>& /*args*/) override
    {
        g_pendingOp = nullptr;
        g_pendingResult.clear();
        g_plan.clear();
        g_planNotes.clear();
        g_planOpName.clear();
        g_hostBusy = false;
    }
};
static ApplyDestroyHandler g_onApplyDestroy;

class ApplyCreatedHandler : public CommandCreatedEventHandler
{
public:
    void notify(const Ptr<CommandCreatedEventArgs>& eventArgs) override
    {
        Ptr<Command> cmd = eventArgs->command();
        if (!cmd) return;
        cmd->execute()->add(&g_onApplyExecute);
        cmd->destroy()->add(&g_onApplyDestroy);

        Ptr<CommandInputs> inputs = cmd->commandInputs();
        if (!inputs) return;

        Ptr<TextBoxCommandInput> title = inputs->addTextBoxCommandInput(
            "hsm_title", "", "<b>Apply to '" + g_planOpName + "'</b>", 1, true);
        if (title) title->isFullWidth(true);

        // Table: [checkbox] | Parameter | Old | New
        Ptr<TableCommandInput> table =
            inputs->addTableCommandInput("hsm_table", "", 4, "1:4:3:3");
        if (table)
        {
            table->hasGrid(true);
            table->isFullWidth(true);
            int visRows = (int)g_plan.size() + 1; // + header
            table->minimumVisibleRows(visRows);
            table->maximumVisibleRows(visRows);

            auto textCell = [&](const std::string& id, const std::string& text,
                                int row, int col, bool header, const std::string& tip = "")
            {
                std::string t = header ? ("<b>" + text + "</b>") : text;
                Ptr<TextBoxCommandInput> tb = inputs->addTextBoxCommandInput(id, "", t, 1, true);
                if (tb)
                {
                    if (!tip.empty()) tb->tooltip(tip);
                    table->addCommandInput(tb, row, col);
                }
            };

            textCell("hsm_h0", "",          0, 0, true);
            textCell("hsm_h1", "Parameter", 0, 1, true);
            textCell("hsm_h2", "Old",       0, 2, true);
            textCell("hsm_h3", "New",       0, 3, true);

            for (size_t i = 0; i < g_plan.size(); ++i)
            {
                const PlanItem& it = g_plan[i];
                int r = (int)i + 1;
                Ptr<BoolValueCommandInput> cb =
                    inputs->addBoolValueInput(it.cbId, "", true, "", it.defaultOn);
                if (cb) table->addCommandInput(cb, r, 0);
                std::string pfx = "hsm_c" + std::to_string(i);
                textCell(pfx + "n", it.label, r, 1, false, it.name);    // hover: raw id
                textCell(pfx + "o", it.oldv, r, 2, false, it.oldExpr);  // hover: expression
                textCell(pfx + "w", it.newv, r, 3, false);
            }
        }

        if (!g_planNotes.empty())
        {
            std::string notes = "Not available on this operation: ";
            for (size_t i = 0; i < g_planNotes.size(); ++i)
                notes += (i ? "; " : "") + g_planNotes[i];
            Ptr<TextBoxCommandInput> nb = inputs->addTextBoxCommandInput(
                "hsm_notes", "", notes, 1, true);
            if (nb) nb->isFullWidth(true);
        }

        inputs->addBoolValueInput("hsm_regen", "Regenerate toolpath", true, "", true);
    }
};
static ApplyCreatedHandler g_onApplyCreated;

// Main flow: read the selected operation's tool geometry, then launch the HSMAdvisor
// dialog host without blocking Fusion. The result is applied later, when the host
// exits, via the custom-event handler above.
class OnExecuteHandler : public CommandEventHandler
{
public:
    void notify(const Ptr<CommandEventArgs>& /*eventArgs*/) override
    {
        if (g_hostBusy)
        {
            ui->messageBox(
                "HSMAdvisor is already starting or open.\n"
                "Please wait for the window (first launch can take a few seconds), "
                "or close it before opening another.",
                "HSMAdvisor Plugin");
            return;
        }

        Ptr<Operation> op = selectedOperation();
        if (!op)
        {
            ui->messageBox(
                "Select a CAM operation in the browser first, then click HSMAdvisor.",
                "HSMAdvisor Plugin");
            return;
        }

        Ptr<Tool> tool = op->tool();
        if (!tool)
        {
            ui->messageBox("The selected operation has no tool.", "HSMAdvisor Plugin");
            return;
        }

        // --- read tool geometry (mm) + identity ---
        Ptr<CAMParameters> tp = tool->parameters();

        std::string toolType = readChoice(tp, "tool_type", "flat end mill");
        std::string toolMat  = readChoice(tp, "tool_material", "unspecified");
        std::string toolDesc = readString(tp, "tool_description");
        std::string toolPid  = readString(tp, "tool_productId");
        std::string opStrategy = op->strategy(); //used to pick tool type
        double diameter  = readLenMm(tp, "tool_diameter", 0.0);
        double tipDia    = readLenMm(tp, "tool_tipDiameter", 0.0);
        int    flutes    = readInt(tp, "tool_numberOfFlutes", 0);
        double cornerRad = readLenMm(tp, "tool_cornerRadius", 0.0);
        double taperAngle = readAngleDeg(tp, "tool_taperAngle", 0.0);
        double tipAngle  = readAngleDeg(tp, "tool_tipAngle", 0.0);
        double shoulderDia = readLenMm(tp, "tool_shoulderDiameter", 0.0);
        double threadPitch = readLenMm(tp, "tool_threadPitch", 0.0);
        double fluteLen  = readLenMm(tp, "tool_fluteLength", 0.0);
        double shoulder  = readLenMm(tp, "tool_shoulderLength", 0.0);
        double oal       = readLenMm(tp, "tool_overallLength", 0.0);
        double shaftDia  = readLenMm(tp, "tool_shaftDiameter", 0.0);

        // Stick-out = the fusion tool body length.
        double stickout = readLenMm(tp, "tool_bodyLength", 0.0);
        if (stickout <= 0.0) stickout = oal;

        // Operation engagement (ad/ae) to seed HSMAdvisor
        Ptr<CAMParameters> ops = op->parameters();
        double docIn = readLenMm(ops, "maximumStepdown", 0.0);
        double wocIn = readFirstLenMm(ops, {"optimalLoad", "maximumStepover", "stepover"});

        // Ramp angle (deg) -> HSMAdvisor ramp-angle plunge calculation.
        double rampAngle = readAngleDeg(ops, "rampAngle", 0.0);
        bool isHelixRamp = toLower(readChoice(ops, "rampType", "")) == "helix";

        // Chamfer engagements (2D Chamfer, or 2D Contour with a chamfer tool) don't use
        // stepdown/stepover
        //   WOC = chamfer width
        //   DOC = tip offset + chamfer width / tan(flank angle)
        // The flank angle is measured from the tool axis. Chamfer mills carry it as the
        // taper angle; fall back to half the tip/included angle, else 45 deg.
        if (isChamferEngagement(op))
        {
            double chamferWidth = readLenMm(ops, "chamferWidth", 0.0);
            double tipOffset = readLenMm(ops, "chamferTipOffset", 0.0);
            double t = chamferTan(chamferHalfAngleDeg(tp));
            double chamferDepth = (t > 1e-9) ? chamferWidth / t : 0.0;
            wocIn = chamferWidth;
            docIn = tipOffset + chamferDepth;
        }

        if (diameter <= 0.0)
        {
            ui->messageBox("Could not read a valid tool diameter from the operation.", "HSMAdvisor Plugin");
            return;
        }

        // --- send the tool and cut to HSMAdvisor (non-blocking) ---
        HsmRequest req;
        req.description = toolDesc;
        req.productId = toolPid;
        req.strategy = opStrategy;
        req.toolType = toolType;
        req.toolMaterial = toolMat;
        req.diameter = diameter;
        req.tipDiameter = tipDia;
        req.flutes = flutes;
        req.cornerRadius = cornerRad;
        req.taperAngle = taperAngle;
        req.tipAngle = tipAngle;
        req.shoulderDiameter = shoulderDia;
        req.threadPitch = threadPitch;
        req.fluteLength = fluteLen;
        req.shoulderLength = shoulder;
        req.shaftDiameter = shaftDia;
        req.stickout = stickout;
        req.docIn = docIn;
        req.wocIn = wocIn;
        req.rampAngle = rampAngle;
        req.rampHelix = isHelixRamp;
        req.ownerWindow = fusionMainWindow();

        g_pendingOp = op;
        g_hostBusy = true;
        runHsmAdvisorAsync(req);
    }
};


static OnExecuteHandler g_onExecute;

class OnCommandCreatedHandler : public CommandCreatedEventHandler
{
public:
    void notify(const Ptr<CommandCreatedEventArgs>& eventArgs) override
    {
        Ptr<Command> cmd = eventArgs->command();
        if (!cmd) return;
        cmd->execute()->add(&g_onExecute);
    }
};

static OnCommandCreatedHandler g_onCommandCreated;

// The "Manage" panel is shared across the Milling, Turning,
// Inspection and Utilities tabs of the Manufacture workspace.
static const char* kPanelId = "CAMManagePanel";

extern "C" XI_EXPORT bool run(const char* context)
{
    app = Application::get();
    if (!app)
        return false;

    ui = app->userInterface();
    if (!ui)
        return false;

    loadApplyPrefs();
    startHsmAdvisorClient();

    // Custom event fired when HSMAdvisor has returned a result; its handler applies it
    // on Fusion's main thread.
    Ptr<CustomEvent> ce = app->registerCustomEvent(kHostDoneEventId);
    if (ce)
        ce->add(&g_onHostDone);

    Ptr<CommandDefinitions> cmdDefs = ui->commandDefinitions();
    if (!cmdDefs)
        return false;

    // Reuse an existing definition if the add-in was reloaded without a restart.
    Ptr<CommandDefinition> cmdDef = cmdDefs->itemById(kCmdId);
    if (!cmdDef)
    {
        std::string iconFolder = addinFolder() + "/resources/HSMAdvisorPlugin";
        cmdDef = cmdDefs->addButtonDefinition(
            kCmdId,
            "HSMAdvisor Plugin",
            "Calculate feeds & speeds for the selected operation's tool and apply them.",
            iconFolder);
    }
    if (!cmdDef)
        return false;

    cmdDef->commandCreated()->add(&g_onCommandCreated);

    // The single results dialog shown after HSMAdvisor returns: a checkbox + old + new
    // per parameter; OK writes the checked rows.
    g_applyCmdDef = cmdDefs->itemById(kApplyCmdId);
    if (!g_applyCmdDef)
        g_applyCmdDef = cmdDefs->addButtonDefinition(
            kApplyCmdId, "Apply HSMAdvisor result",
            "Choose which calculated values to apply to the operation.");
    if (g_applyCmdDef)
        g_applyCmdDef->commandCreated()->add(&g_onApplyCreated);

    // Place the button in the Manage panel.
    Ptr<Workspaces> workspaces = ui->workspaces();
    Ptr<Workspace> camWs = workspaces ? workspaces->itemById("CAMEnvironment") : nullptr;
    if (camWs)
    {
        Ptr<ToolbarPanels> panels = camWs->toolbarPanels();
        Ptr<ToolbarPanel> panel = panels ? panels->itemById(kPanelId) : nullptr;
        if (panel)
        {
            Ptr<ToolbarControls> controls = panel->controls();
            if (controls && !controls->itemById(kCmdId))
                controls->addCommand(cmdDef);
        }
    }

    return true;
}

extern "C" XI_EXPORT bool stop(const char* context)
{
    if (ui)
    {
        Ptr<Workspaces> workspaces = ui->workspaces();
        Ptr<Workspace> camWs = workspaces ? workspaces->itemById("CAMEnvironment") : nullptr;
        if (camWs)
        {
            Ptr<ToolbarPanels> panels = camWs->toolbarPanels();
            Ptr<ToolbarPanel> panel = panels ? panels->itemById(kPanelId) : nullptr;
            if (panel)
            {
                Ptr<ToolbarControls> controls = panel->controls();
                Ptr<ToolbarControl> ctrl = controls ? controls->itemById(kCmdId) : nullptr;
                if (ctrl)
                    ctrl->deleteMe();
            }
        }

        Ptr<CommandDefinitions> cmdDefs = ui->commandDefinitions();
        Ptr<CommandDefinition> cmdDef = cmdDefs ? cmdDefs->itemById(kCmdId) : nullptr;
        if (cmdDef)
            cmdDef->deleteMe();
        Ptr<CommandDefinition> applyDef = cmdDefs ? cmdDefs->itemById(kApplyCmdId) : nullptr;
        if (applyDef)
            applyDef->deleteMe();
        g_applyCmdDef = nullptr;

        ui = nullptr;
    }

    stopHsmAdvisorClient(); // closes an open HSMAdvisor dialog of this add-in

    if (app)
        app->unregisterCustomEvent(kHostDoneEventId);

    return true;
}

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
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <thread>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")

using namespace adsk::core;
using namespace adsk::fusion;
using namespace adsk::cam;

Ptr<Application> app;
Ptr<UserInterface> ui;

static const char* kCmdId = "HSMAdvisorTestCmd";

// Parameter helpers. CAM lengths come back in centimeters (*10 -> mm); we write
// values back as expressions with unit tokens (rpm/mmpm/mm) so Fusion converts them
// to the document's display units.
static double readLenMm(const Ptr<CAMParameters>& params, const std::string& name, double fallback)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return fallback;
    Ptr<FloatParameterValue> fv = p->value();
    return fv ? fv->value() * 10.0 : fallback; // cm -> mm
}

// Reads the first candidate length that exists on this strategy.
static double readFirstLenMm(const Ptr<CAMParameters>& params,
                             const std::vector<std::string>& candidates)
{
    for (const std::string& name : candidates)
    {
        Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
        if (!p) continue;
        Ptr<FloatParameterValue> fv = p->value();
        if (fv) return fv->value() * 10.0; // cm -> mm
    }
    return 0.0;
}

static int readInt(const Ptr<CAMParameters>& params, const std::string& name, int fallback)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return fallback;
    Ptr<IntegerParameterValue> iv = p->value();
    return iv ? iv->value() : fallback;
}

// CAM angle parameters return value() already in degrees. So read it straight, no radian conversion.
static double readAngleDeg(const Ptr<CAMParameters>& params, const std::string& name, double fallback)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return fallback;
    Ptr<FloatParameterValue> fv = p->value();
    return fv ? fv->value() : fallback;
}

static bool readBool(const Ptr<CAMParameters>& params, const std::string& name, bool fallback)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return fallback;
    Ptr<BooleanParameterValue> bv = p->value();
    return bv ? bv->value() : fallback;
}

static std::string readChoice(const Ptr<CAMParameters>& params, const std::string& name, const std::string& fallback)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return fallback;
    Ptr<ChoiceParameterValue> cv = p->value();
    return cv ? cv->value() : fallback;
}

static std::string readString(const Ptr<CAMParameters>& params, const std::string& name)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p) return "";
    Ptr<StringParameterValue> sv = p->value();
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

    std::string strat = op->strategy();
    for (char& c : strat) if (c >= 'A' && c <= 'Z') c += 32;

    // 2D Chamfer operation (strategy id contains "chamfer").
    if (strat.find("chamfer") != std::string::npos)
        return true;

    // 2D Contour with a chamfer tool.
    if (strat.find("contour") != std::string::npos)
    {
        Ptr<Tool> tool = op->tool();
        std::string toolType = readChoice(tool ? tool->parameters() : nullptr, "tool_type", "");
        for (char& c : toolType) if (c >= 'A' && c <= 'Z') c += 32;
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
    if (raw == "tool_feedCutting")  return "Cutting feed";
    if (raw == "tool_feedPlunge")   return "Plunge feed";
    if (raw == "peckingDepth")      return "Peck depth";
    if (raw == "maximumStepdown")   return "Depth of cut";
    if (raw == "optimalLoad" || raw == "maximumStepover" || raw == "stepover")
        return "Width of cut";
    if (raw == "chamferWidth")      return "Chamfer width";
    if (raw == "chamferTipOffset")  return "Chamfer tip offset";
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

// HSMAdvisorPluginHost.exe interop. The dialog runs in a separate, DPI-unaware
// process so its WinForms controls don't overlap the way they do inside
// Fusion's DPI-aware process. Tool + result are exchanged as key=value
// text over a named pipe, and the add-in never loads the CLR itself.
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
struct ApplySel { bool all, ad, ae, rpm, feed, plungeRpm, plungeFeed; };
static ApplySel g_apply = { true, true, true, true, true, true, true };

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
      << "plungeFeed=" << (g_apply.plungeFeed ? 1 : 0) << "\n";
}

static const char*    kHostDoneEventId = "HSMAdvisorHostDoneEvent";
static const char*    kPipeShortName   = "HSMAdvisorFusionHost";
static const wchar_t* kPipeFullName    = L"\\\\.\\pipe\\HSMAdvisorFusionHost";

// The host's response text, produced on the worker thread and consumed by the
// custom-event handler on the main thread. Only one request is in flight at a time.
static std::string g_pendingResponse;

// Launches the host in warm "server" mode (loads the HSMAdvisor database once, then
// serves requests over a named pipe). A second launch is harmless, the host uses a
// single-instance mutex and the extra process exits immediately. Returns the process
// handle (caller closes it) or nullptr if the host exe could not be started at all.
static HANDLE launchHostServer()
{
    std::string dir = addinFolder();
    std::string exe = dir + "\\HSMAdvisorPluginHost.exe";
    std::string cmd = "\"" + exe + "\" --server " + kPipeShortName;

    std::wstring wcmd = utf8ToWide(cmd);
    std::wstring wdir = utf8ToWide(dir);
    std::vector<wchar_t> cmdbuf(wcmd.begin(), wcmd.end());
    cmdbuf.push_back(L'\0');

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, wdir.c_str(), &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// Connects to the host pipe, retrying until timeout. If hProc is given, also bails out the moment
// that processexits before serving (e.g. HSMAdvisor not installed -> host exits with code 2),
// so it fail fast instead of waiting the whole timeout. Returns INVALID_HANDLE_VALUE on failure,
// writing the process exit code to procExitCode when the process ended.
static HANDLE connectHostPipe(DWORD timeoutMs, HANDLE hProc = nullptr, DWORD* procExitCode = nullptr)
{
    DWORD start = GetTickCount();
    for (;;)
    {
        HANDLE h = CreateFileW(kPipeFullName, GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeW(kPipeFullName, 1000);
        else
            Sleep(200); // pipe not created yet
        if (hProc && WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0)
        {
            if (procExitCode) GetExitCodeProcess(hProc, procExitCode);
            return INVALID_HANDLE_VALUE; // host died before it could serve
        }
        if (GetTickCount() - start > timeoutMs)
            return INVALID_HANDLE_VALUE;
    }
}

// Sends the request to the warm host over the pipe on a worker thread (launching the
// host if needed), then fires the custom event so the result is applied on Fusion's
// main thread. Never blocks Fusion's UI thread.
static void sendToHostAsync(const std::string& request)
{
    std::thread([request]()
    {
        std::string resp;
        HANDLE h = connectHostPipe(1500);   // quick try
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD exitCode = STILL_ACTIVE;
            HANDLE hProc = launchHostServer();
            if (!hProc)
            {
                resp = "status=error\nerror=The HSMAdvisor host component "
                       "(HSMAdvisorPluginHost.exe) is missing. Reinstall the add-in.\n";
            }
            else
            {
                h = connectHostPipe(30000, hProc, &exitCode); // wait for first-time DB load
                CloseHandle(hProc);
                if (h == INVALID_HANDLE_VALUE)
                {
                    if (exitCode == 0)
                        h = connectHostPipe(30000); // another instance already exist
                    else if (exitCode != STILL_ACTIVE)
                        resp = "status=error\nerror=HSMAdvisor could not be started. Make sure "
                               "HSMAdvisor is installed and licensed (https://hsmadvisor.com/download), "
                               "then try again.\n";
                }
            }
        }

        if (h != INVALID_HANDLE_VALUE)
        {
            std::string req = request + "END\n";
            DWORD written = 0;
            WriteFile(h, req.data(), (DWORD)req.size(), &written, nullptr);
            char buf[4096];
            DWORD rd = 0;
            while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0)
                resp.append(buf, rd);   // closes the pipe after the response
            CloseHandle(h);
        }
        else if (resp.empty())
        {
            resp = "status=error\nerror=Could not reach the HSMAdvisor host process.\n";
        }

        g_pendingResponse = resp;
        if (app)
            app->fireCustomEvent(kHostDoneEventId, "");
    }).detach();
}

// Asks the warm host to exit.
static void quitHost()
{
    HANDLE h = CreateFileW(kPipeFullName, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::string req = "cmd=quit\nEND\n";
    DWORD written = 0;
    WriteFile(h, req.data(), (DWORD)req.size(), &written, nullptr);
    CloseHandle(h);
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
// "what to apply" chooser (all on the main thread).
static bool g_hostBusy = false;
static Ptr<Operation> g_pendingOp;
static std::map<std::string, std::string> g_pendingResult; // last host result, for the chooser
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

    // Plunge RPM/feed and SFM are returned by the host but intentionally not applied for now.
    int    rpm     = (int)strtod(kvGet(out, "rpm", "0").c_str(), nullptr);
    double feedCut = strtod(kvGet(out, "feedCut", "0").c_str(), nullptr);
    double doc     = strtod(kvGet(out, "doc", "0").c_str(), nullptr);
    double woc     = strtod(kvGet(out, "woc", "0").c_str(), nullptr);
    double peck    = strtod(kvGet(out, "peck", "0").c_str(), nullptr);

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
    std::string strat = op->strategy();
    for (char& c : strat) if (c >= 'A' && c <= 'Z') c += 32;
    bool isDrill = (strat == "drill");
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

    // Adds a checkbox row to the plan. defaultOn comes from the saved per-category prefs.
    auto add = [&](const std::string& category, const std::string& name,
                   const std::string& newDisplay, std::vector<PendingWrite> writes, bool defaultOn)
    {
        PlanItem it;
        it.cbId = "hsm_row" + std::to_string(g_plan.size());
        it.category = category;
        it.name = name;
        it.label = friendlyParamName(name);
        it.oldv = readExpr(ops, name);
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
            if      (item.category == "rpm")  g_apply.rpm  = on;
            else if (item.category == "feed") g_apply.feed = on;
            else if (item.category == "ad")   g_apply.ad   = on;
            else if (item.category == "ae")   g_apply.ae   = on;

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
                textCell(pfx + "n", it.label, r, 1, false, it.name); // hover shows raw id
                textCell(pfx + "o", it.oldv, r, 2, false);
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

        // --- build the request and send it to the warm host (non-blocking) ---
        std::ostringstream req;
        req << "showDialog=1\n"
            << "description=" << toolDesc << "\n"
            << "productId=" << toolPid << "\n"
            << "strategy=" << opStrategy << "\n"
            << "toolType=" << toolType << "\n"
            << "toolMaterial=" << toolMat << "\n"
            << "diameter=" << numToStr(diameter, 4) << "\n"
            << "tipDiameter=" << numToStr(tipDia, 4) << "\n"
            << "flutes=" << flutes << "\n"
            << "cornerRadius=" << numToStr(cornerRad, 4) << "\n"
            << "taperAngle=" << numToStr(taperAngle, 4) << "\n"
            << "tipAngle=" << numToStr(tipAngle, 4) << "\n"
            << "shoulderDiameter=" << numToStr(shoulderDia, 4) << "\n"
            << "threadPitch=" << numToStr(threadPitch, 4) << "\n"
            << "fluteLength=" << numToStr(fluteLen, 4) << "\n"
            << "shoulderLength=" << numToStr(shoulder, 4) << "\n"
            << "overallLength=" << numToStr(oal, 4) << "\n"
            << "shaftDiameter=" << numToStr(shaftDia, 4) << "\n"
            << "stickout=" << numToStr(stickout, 4) << "\n"
            << "docIn=" << numToStr(docIn, 4) << "\n"
            << "wocIn=" << numToStr(wocIn, 4) << "\n";

        g_pendingOp = op;
        g_hostBusy = true;
        sendToHostAsync(req.str());
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

    // Custom event fired when the host process exits; its handler applies the result
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

    quitHost(); // shut the warm host process down with the add-in

    if (app)
        app->unregisterCustomEvent(kHostDoneEventId);

    return true;
}

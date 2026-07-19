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
#include <cstdlib>
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

// Sets a parameter's expression; records the name in `failed` if it doesn't stick.
static void writeExpr(const Ptr<CAMParameters>& params, const std::string& name,
                      const std::string& expr, std::vector<std::string>& failed)
{
    Ptr<CAMParameter> p = params ? params->itemByName(name) : nullptr;
    if (!p || !p->expression(expr))
        failed.push_back(name);
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

// Writes the first `candidates` parameter that exists on this strategy; returns its
// name ("" if none). Engagement is named per strategy (adaptive: optimalLoad,
// pocket: maximumStepover, ...), so it try them in order.
static std::string setFirstExisting(const Ptr<CAMParameters>& params,
                                     const std::vector<std::string>& candidates,
                                     const std::string& expr,
                                     std::vector<std::string>& failed)
{
    for (const std::string& name : candidates)
    {
        SetResult r = trySet(params, name, expr);
        if (r == SetResult::Ok) return name;
        if (r == SetResult::Failed) { failed.push_back(name); return ""; }
        // NotPresent -> try next candidate
    }
    return "";
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

// Writes the selected feeds & speeds into the operation. Assumes the result is valid
// (status ok); the caller decides whether to invoke it.
static void applyHostResult(const Ptr<Operation>& op, const std::map<std::string, std::string>& out)
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
    std::vector<std::string> failed;

    // "all" is a master switch; otherwise apply only the individually-selected ones.
    // (Plunge RPM / plunge feed are intentionally not written for now.)
    bool ap_rpm  = g_apply.all || g_apply.rpm;
    bool ap_feed = g_apply.all || g_apply.feed;
    bool ap_ad   = g_apply.all || g_apply.ad;
    bool ap_ae   = g_apply.all || g_apply.ae;

    std::string rpmExpr = numToStr((double)rpm, 0) + "rpm";
    std::string cutExpr = numToStr(feedCut, 1) + "mmpm";

    // If is drilling operations then set feed as plunge feed, otherwise set as cutting feed.
    std::string strat = op->strategy();
    for (char& c : strat) if (c >= 'A' && c <= 'Z') c += 32;
    bool isDrill = (strat == "drill");

    if (ap_rpm)
        writeExpr(ops, "tool_spindleSpeed", rpmExpr, failed);
    if (ap_feed)
    {
        if (isDrill)
        {
            writeExpr(ops, "tool_feedPlunge", cutExpr, failed);
        }
        else
        {
            writeExpr(ops, "tool_feedCutting", cutExpr, failed);
            writeExpr(ops, "tool_feedEntry", cutExpr, failed);
            writeExpr(ops, "tool_feedExit", cutExpr, failed);
            writeExpr(ops, "tool_feedTransition", cutExpr, failed);
        }
    }

    // Depth of cut (ad): for milling write maximumStepdown (leaving doMultipleDepths alone,
    // the user controls that), for drilling write peckingDepth from hsmadvisor peek value.
    std::string docParam, wocParam;
    bool peckWritten = false;
    if (ap_ad && isDrill)
    {
        if (peck > 0.0)
            peckWritten = (trySet(ops, "peckingDepth", numToStr(peck, 3) + "mm") == SetResult::Ok);
    }
    else
    {
        if (ap_ad && doc > 0.0)
        {
            if (trySet(ops, "maximumStepdown", numToStr(doc, 3) + "mm") == SetResult::Ok)
                docParam = "maximumStepdown";
            else
                failed.push_back("maximumStepdown");
        }
        // WOC (ae): parameter name varies by strategy, try each in turn. (temporary solution)
        if (ap_ae && woc > 0.0)
            wocParam = setFirstExisting(
                ops, {"optimalLoad", "maximumStepover", "stepover"},
                numToStr(woc, 3) + "mm", failed);
    }

    // Values are always written in mm (Fusion converts them via the unit tokens), but the
    // summary is shown in the document's own units so it matches what the user sees.
    bool metric = readBool(ops, "metric", true);
    double disp = metric ? 1.0 : 1.0 / 25.4;
    const char* lenUnit  = metric ? "mm" : "in";
    const char* feedUnit = metric ? "mm/min" : "in/min";

    std::ostringstream msg;
    msg.setf(std::ios::fixed);
    msg << "Applied to '" << op->name() << "':\n\n";
    msg.precision(0);
    if (ap_rpm)  msg << "Spindle:  " << (double)rpm << " rpm\n";
    msg.precision(metric ? 1 : 2);
    if (ap_feed) msg << (isDrill ? "Plunge:  " : "Cutting:  ") << feedCut * disp << " " << feedUnit << "\n";
    msg.precision(metric ? 3 : 4);
    if (ap_ad && isDrill)
        msg << "Peck: " << peck * disp << " " << lenUnit
            << (peckWritten ? "  -> peckingDepth" : "  (no peck value)") << "\n";
    else
    {
        if (ap_ad)
            msg << "DOC: " << doc * disp << " " << lenUnit
                << (docParam.empty() ? "  (no depth param on this strategy)" : "  -> " + docParam) << "\n";
        if (ap_ae)
            msg << "WOC: " << woc * disp << " " << lenUnit
                << (wocParam.empty() ? "  (no radial param on this strategy)" : "  -> " + wocParam) << "\n";
    }
    msg << "\nRegenerate the toolpath to see the update.";
    if (!failed.empty())
    {
        msg << "\n\nNote: could not set: ";
        for (size_t i = 0; i < failed.size(); ++i)
            msg << (i ? ", " : "") << failed[i];
    }

    ui->messageBox(msg.str(), "HSMAdvisor Plugin");

    // After the apply, surface any available plugin update (once per session).
    maybeOfferUpdate(out);
}

// Fired when the host has returned a result. On success it opensthe "what to apply" chooser
// (g_hostBusy stays true until the chooser is destroyed).
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
            if (g_applyCmdDef)
                g_applyCmdDef->execute(); // show the chooser
            else
                { applyHostResult(g_pendingOp, out); g_pendingOp = nullptr; g_hostBusy = false; }
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

// --- "what to apply" chooser command (shown after HSMAdvisor returns) --------
class ChooserExecuteHandler : public CommandEventHandler
{
public:
    void notify(const Ptr<CommandEventArgs>& eventArgs) override
    {
        Ptr<Command> cmd = eventArgs ? eventArgs->command() : nullptr;
        Ptr<CommandInputs> inputs = cmd ? cmd->commandInputs() : nullptr;
        if (inputs)
        {
            auto gb = [&](const char* id, bool def)
            {
                Ptr<BoolValueCommandInput> b = inputs->itemById(id);
                return b ? b->value() : def;
            };
            g_apply.all  = gb("hsm_all", g_apply.all);
            g_apply.rpm  = gb("hsm_rpm", g_apply.rpm);
            g_apply.feed = gb("hsm_feed", g_apply.feed);
            g_apply.ad   = gb("hsm_ad", g_apply.ad);
            g_apply.ae   = gb("hsm_ae", g_apply.ae);
            saveApplyPrefs();
        }
        applyHostResult(g_pendingOp, g_pendingResult);
    }
};
static ChooserExecuteHandler g_onChooserExecute;

// Fires on both OK and Cancel of the chooser, always clears the busy state.
class ChooserDestroyHandler : public CommandEventHandler
{
public:
    void notify(const Ptr<CommandEventArgs>& /*args*/) override
    {
        g_pendingOp = nullptr;
        g_pendingResult.clear();
        g_hostBusy = false;
    }
};
static ChooserDestroyHandler g_onChooserDestroy;

class ChooserCreatedHandler : public CommandCreatedEventHandler
{
public:
    void notify(const Ptr<CommandCreatedEventArgs>& eventArgs) override
    {
        Ptr<Command> cmd = eventArgs->command();
        if (!cmd) return;
        cmd->execute()->add(&g_onChooserExecute);
        cmd->destroy()->add(&g_onChooserDestroy);

        Ptr<CommandInputs> inputs = cmd->commandInputs();
        if (inputs)
        {
            inputs->addBoolValueInput("hsm_all",  "Apply all",         true, "", g_apply.all);
            inputs->addBoolValueInput("hsm_rpm",  "Spindle RPM",       true, "", g_apply.rpm);
            inputs->addBoolValueInput("hsm_feed", "Feedrate",          true, "", g_apply.feed);
            inputs->addBoolValueInput("hsm_ad",   "Depth of cut (ad)", true, "", g_apply.ad);
            inputs->addBoolValueInput("hsm_ae",   "Width of cut (ae)", true, "", g_apply.ae);
        }
    }
};
static ChooserCreatedHandler g_onChooserCreated;

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

    // Command that shows the "what to apply" chooser after HSMAdvisor returns.
    g_applyCmdDef = cmdDefs->itemById(kApplyCmdId);
    if (!g_applyCmdDef)
        g_applyCmdDef = cmdDefs->addButtonDefinition(
            kApplyCmdId, "Apply HSMAdvisor result",
            "Choose which calculated values to apply to the operation.");
    if (g_applyCmdDef)
        g_applyCmdDef->commandCreated()->add(&g_onChooserCreated);

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

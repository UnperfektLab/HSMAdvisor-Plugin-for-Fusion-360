// HSMAdvisor Plugin for Fusion 360 - dialog host process.
// Copyright (c) 2026 UnperfektLab. MIT License; see LICENSE.
//
// Unofficial third-party plugin. Not supported by HSMAdvisor / Eldar Gerfanov.
// It drives a separately installed, licensed copy of HSMAdvisor.

// HSMAdvisorPluginHost.exe — shows the HSMAdvisor dialog in its own DPI-unaware
// process (like the standalone app) so its WinForms controls don't overlap the way
// they do inside Fusion. Two modes:
//   1) Single-shot (testing): HSMAdvisorPluginHost.exe <inputFile> <outputFile>
//   2) Warm server:           HSMAdvisorPluginHost.exe --server <pipeName>
//      Loads the database once, then serves requests over a named pipe.
// Wire format: key=value lines; requests end with a line "END", responses end at EOF.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using System.Xml;
using Microsoft.Win32;
using HSMAdvisor;
using HSMAdvisorDatabase.ToolDataBase;

static class Program
{
    // The HSMAdvisor engine DLLs are NOT bundled with the add-in; they are loaded from
    // the user's HSMAdvisor installation. Install the resolver BEFORE any method that
    // references those assemblies is JIT-compiled, hence the thin Main -> Run split.
    [STAThread]
    static int Main(string[] args)
    {
        AppDomain.CurrentDomain.AssemblyResolve += ResolveHsm;
        return Run(args);
    }

    static string _hsmDir; // cached install directory ("" = searched, not found)

    static string HsmInstallDir()
    {
        if (_hsmDir != null) return _hsmDir.Length == 0 ? null : _hsmDir;
        _hsmDir = FindHsmDir() ?? "";
        return _hsmDir.Length == 0 ? null : _hsmDir;
    }

    static string FindHsmDir()
    {
        // 1) Uninstall registry keys (Inno Setup writes InstallLocation).
        string[] roots =
        {
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
            @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
        };
        foreach (var hive in new[] { Registry.CurrentUser, Registry.LocalMachine })
            foreach (var root in roots)
            {
                try
                {
                    using (var k = hive.OpenSubKey(root))
                    {
                        if (k == null) continue;
                        foreach (var sub in k.GetSubKeyNames())
                        {
                            try
                            {
                                using (var sk = k.OpenSubKey(sub))
                                {
                                    var name = sk?.GetValue("DisplayName") as string;
                                    if (name == null || name.IndexOf("HSMAdvisor", StringComparison.OrdinalIgnoreCase) < 0)
                                        continue;
                                    var loc = sk.GetValue("InstallLocation") as string;
                                    if (!string.IsNullOrEmpty(loc) &&
                                        File.Exists(Path.Combine(loc, "HSMAdvisorCore.dll")))
                                        return loc.TrimEnd('\\');
                                }
                            }
                            catch { }
                        }
                    }
                }
                catch { }
            }

        // 2) Common install locations.
        foreach (var c in new[]
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "HSMAdvisor"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "HSMAdvisor"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "HSMAdvisor"),
        })
            if (File.Exists(Path.Combine(c, "HSMAdvisorCore.dll"))) return c;

        return null;
    }

    static Assembly ResolveHsm(object sender, ResolveEventArgs e)
    {
        try
        {
            string name = new AssemblyName(e.Name).Name;
            string dir = HsmInstallDir();
            if (dir == null) return null;
            string p = Path.Combine(dir, name + ".dll");
            return File.Exists(p) ? Assembly.LoadFrom(p) : null;
        }
        catch { return null; }
    }

    static int Run(string[] args)
    {
        if (HsmInstallDir() == null)
        {
            // No HSMAdvisor installation found; report it if we were given an output file.
            if (args.Length >= 2 && args[0] != "--server")
                try { Write(args[1], new Dictionary<string, string> { ["status"] = "error", ["error"] = "HSMAdvisor is not installed." }); } catch { }
            return 2;
        }

        if (args.Length >= 2 && args[0] == "--server")
            return RunServer(args[1]);

        // Single-shot mode.
        string inPath = args.Length > 0 ? args[0] : null;
        string outPath = args.Length > 1 ? args[1] : null;
        try
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Core.init("");
            var inp = ReadKvFile(inPath);
            var outv = ProcessRequest(inp);
            Write(outPath, outv);
            string s = outv.TryGetValue("status", out var v) ? v : "error";
            return s == "ok" ? 0 : (s == "cancel" ? 1 : 2);
        }
        catch (Exception ex)
        {
            try { Write(outPath, new Dictionary<string, string> { ["status"] = "error", ["error"] = ex.Message }); }
            catch { }
            return 2;
        }
    }

    // ---- warm server mode -------------------------------------------------
    static int RunServer(string pipeName)
    {
        bool createdNew;
        using (var mutex = new Mutex(true, "HSMAdvisorFusionHostMutex", out createdNew))
        {
            if (!createdNew) return 0; // another host is already running

            try
            {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Core.init("");
            }
            catch { return 2; }

            const int idleMs = 30 * 60 * 1000; // exit if unused for 30 minutes
            while (true)
            {
                using (var server = new NamedPipeServerStream(
                    pipeName, PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous))
                {
                    var conn = server.WaitForConnectionAsync();
                    if (!conn.Wait(idleMs))
                        return 0; // idle timeout

                    try
                    {
                        var reader = new StreamReader(server, Encoding.UTF8, false, 4096, true);
                        var inp = ReadUntilEnd(reader);
                        if (inp.TryGetValue("cmd", out var cmd) && cmd == "quit")
                            return 0;

                        var outv = ProcessRequest(inp);

                        var writer = new StreamWriter(server, new UTF8Encoding(false), 4096, true) { NewLine = "\n" };
                        foreach (var kv in outv) writer.WriteLine(kv.Key + "=" + kv.Value);
                        writer.Flush();
                        server.WaitForPipeDrain();
                    }
                    catch { /* client vanished; serve the next one */ }
                }
            }
        }
    }

    static Dictionary<string, string> ReadUntilEnd(StreamReader r)
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string line;
        while ((line = r.ReadLine()) != null)
        {
            if (line == "END") break;
            int eq = line.IndexOf('=');
            if (eq > 0) d[line.Substring(0, eq)] = line.Substring(eq + 1);
        }
        return d;
    }

    // The plugin keeps its OWN settings file in the add-in folder (next to this host
    // exe, alongside apply_prefs.txt) Remembers the last material / machine / productivity / HSM.
    static string SettingsPath()
        => Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "settings.xml");

    static string XmlStr(XmlNode parent, string name, string def)
    {
        var n = parent?.SelectSingleNode(name);
        return n != null ? n.InnerText : def;
    }
    static int XmlInt(XmlNode parent, string name, int def)
        => int.TryParse(XmlStr(parent, name, ""), out var r) ? r : def;

    // Seeds material / machine / productivity / HSM from settings file.
    static void ApplyLastSettings(Calculation calc)
    {
        try
        {
            string p = SettingsPath();
            if (!File.Exists(p)) return;
            var doc = new XmlDocument();
            doc.Load(p);
            var root = doc.SelectSingleNode("/PluginSettings");
            if (root == null) return;

            int matId = XmlInt(root, "material_id", 0);
            string machGuid = XmlStr(root, "machine_id", "");
            int prod = XmlInt(root, "productivity", 4);
            bool hsm = XmlStr(root, "hsm", "0") == "1";

            if (matId > 0) { try { calc.SetMaterial(matId); } catch { } }
            if (!string.IsNullOrEmpty(machGuid))
            {
                try
                {
                    foreach (var m in Core.GetMachineList())
                        if (string.Equals(m.GUID, machGuid, StringComparison.OrdinalIgnoreCase))
                        { calc.Machine = m; break; }
                }
                catch { }
            }
            try { calc.Productivity = prod; } catch { }
            try { calc.HSM = hsm; } catch { }
        }
        catch { }
    }

    // mm/inch mode the user last left the dialog (default: metric).
    static bool ReadRememberedMetric()
    {
        try
        {
            string p = SettingsPath();
            if (!File.Exists(p)) return true;
            var doc = new XmlDocument();
            doc.Load(p);
            var root = doc.SelectSingleNode("/PluginSettings");
            return root == null || XmlStr(root, "metric", "1") == "1";
        }
        catch { return true; }
    }

    // Writes the chosen material / machine / productivity / HSM to the plugin's
    // settings file so the next operation remembers them.
    static void SaveLastSettings(Calculation res)
    {
        try
        {
            string machGuid = "";
            try { if (res.Machine != null) machGuid = res.Machine.GUID ?? ""; } catch { }

            var doc = new XmlDocument();
            var root = doc.CreateElement("PluginSettings");
            doc.AppendChild(root);
            void Add(string name, string value)
            {
                var e = doc.CreateElement(name);
                e.InnerText = value;
                root.AppendChild(e);
            }
            if (res.MaterialID > 0)
                Add("material_id", res.MaterialID.ToString(CultureInfo.InvariantCulture));
            if (!string.IsNullOrEmpty(machGuid))
                Add("machine_id", machGuid);
            Add("productivity", res.Productivity.ToString(CultureInfo.InvariantCulture));
            Add("hsm", res.HSM ? "1" : "0");
            try { Add("metric", res.Input_Units_m ? "1" : "0"); } catch { }

            string p = SettingsPath();
            Directory.CreateDirectory(Path.GetDirectoryName(p));
            doc.Save(p);
        }
        catch { }
    }

    // ---- core: build calc, look up in DB, show dialog, return result ------
    static Dictionary<string, string> ProcessRequest(Dictionary<string, string> inp)
    {
        var outv = new Dictionary<string, string> { ["status"] = "error", ["found"] = "0", ["error"] = "" };
        try
        {
            bool showDialog = GetInt(inp, "showDialog", 1) != 0;
            string desc = Get(inp, "description", "");
            string pid = Get(inp, "productId", "");
            string toolType = Get(inp, "toolType", "flat end mill");
            string toolMat = Get(inp, "toolMaterial", "unspecified");
            string strategy = Get(inp, "strategy", "");
            double dia = GetD(inp, "diameter", 0), tipDia = GetD(inp, "tipDiameter", 0),
                   fl = GetD(inp, "fluteLength", 0),
                   sh = GetD(inp, "shoulderLength", 0), oal = GetD(inp, "overallLength", 0),
                   sd = GetD(inp, "shaftDiameter", 0), cr = GetD(inp, "cornerRadius", 0),
                   ta = GetD(inp, "taperAngle", 0), tipAng = GetD(inp, "tipAngle", 0),
                   sdia = GetD(inp, "shoulderDiameter", 0), pitch = GetD(inp, "threadPitch", 0),
                   stickout = GetD(inp, "stickout", 0);
            int flutes = GetInt(inp, "flutes", 0);

            // Build the tool fresh (geometry from Fusion, or the DB tool).
            var calc = new Calculation();
            calc.SetMetric(true);

            Tool found = FindMatchingTool(desc, pid);
            if (found != null) { calc.SetTool(found); outv["found"] = "1"; }
            else { SetGeometry(calc, toolType, toolMat, strategy, dia, tipDia, flutes, cr, ta, tipAng, sdia, pitch, fl, sh, oal, sd); }

            // Seed material / machine / options from settings file so it open
            // where the previous run left off. Fresh tool geometry above is untouched.
            ApplyLastSettings(calc);
            if (calc.MaterialID <= 0)
            {
                calc.SetMaterial(227);   // default until a material is picked
                calc.Productivity = 4;   // productivity slider -> x1.0
            }

            // Stick-out is editable even for a tool loaded from the database, so take
            // Fusion's actual value (body length) and deflection/rigidity reflect the real
            // setup. Skipped for types that don't take it (tap).
            if (stickout > 0 && FieldsFor(MapToolType(toolType, strategy)).HasFlag(G.Stickout))
                calc.Stickout = stickout;

            // Restore the unit mode the user last left the dialog.
            try { calc.SetMetric(ReadRememberedMetric()); } catch { }

            Calculation res = calc;
            if (showDialog)
            {
                if (found == null)
                {
                    MessageBox.Show(
                        "Tool not found in the HSMAdvisor database.\n" +
                        "Description: " + (desc.Length == 0 ? "(empty)" : desc) + "\n" +
                        "Product id: " + (pid.Length == 0 ? "(empty)" : pid) + "\n\n" +
                        "Enter / confirm the tool and cut values in the dialog.",
                        "HSMAdvisor Plugin");
                }
                res = Core.ShowHSMAdvisorDialog(
                    calc, true,
                    "Select workpiece material and machine, then click OK to apply to Fusion.");

                // Remember material / machine / productivity / HSM on ANY exit -- OK,
                // Cancel, or closing the window.
                SaveLastSettings(res ?? calc);

                if (res == null) { outv["status"] = "cancel"; return outv; }
            }
            else if (!calc.Calculate(false))
            {
                outv["error"] = "Calculate() returned false.";
                return outv;
            }

            // Always report millimetres, the add-in writes them back with mm 
            // unit tokens, so normalise first, SetMetric converts the values.
            try { res.SetMetric(true); } catch { }

            outv["status"] = "ok";
            outv["rpm"] = Inv(res.RPM);
            outv["rpmPlunge"] = Inv(res.RPM_Plunge);
            outv["feedCut"] = Inv(res.FEED);
            outv["feedPlunge"] = Inv(res.FEED_Plunge);
            outv["doc"] = Inv(res.DOC);
            outv["woc"] = Inv(res.WOC);
            outv["sfm"] = Inv(res.Real_SFM);
            outv["chipload"] = Inv(res.Real_IPT);
            return outv;
        }
        catch (Exception ex)
        {
            outv["status"] = "error";
            outv["error"] = ex.Message;
            return outv;
        }
    }

    // ---- tool mapping / DB matching ---------------------------------------
    // Maps a Fusion tool type to the nearest HSMAdvisor one.
    // Notes: the chamfer mill is also Fusion's engraving tool, so engraving is told apart
    // by the operation strategy. Types with no equivalent map to Unknown, which writes no geometry at
    // all (see FieldsFor) and leaves the dialog for the user to fill in.
    static Enums.ToolTypes MapToolType(string t, string strategy)
    {
        t = (t ?? "").ToLowerInvariant();
        bool engrave = (strategy ?? "").ToLowerInvariant().Contains("engrave");

        switch (t)
        {
            // --- mills -------------------------------------------------------
            case "flat end mill":
            case "bull nose end mill":
            case "tapered mill":
            case "form mill":                       return Enums.ToolTypes.SolidEndMill;
            case "ball end mill":                   return Enums.ToolTypes.SolidBallMill;
            case "lollipop mill":                   return Enums.ToolTypes.IndexedBallMill;
            case "face mill":                       return Enums.ToolTypes.IndexedFaceMill;
            case "thread mill":                     return Enums.ToolTypes.ThreadMill;
            case "dovetail mill":
            case "slot mill":                       return Enums.ToolTypes.WoodRuff;   // nearest to a T-slot cutter
            case "circle segment taper":            return Enums.ToolTypes.SolidBallMill;

            // Same tool for chamfering and engraving; tell them apart by the operation.
            case "chamfer mill":
            case "corner chamfer end mill":
                return engrave ? Enums.ToolTypes.VbitEngraver : Enums.ToolTypes.ChamferMill;

            // --- drilling ----------------------------------------------------
            case "drill":
            case "block drill":                     return Enums.ToolTypes.JobberTwistDrill;
            case "spot drill":
            case "center drill":                    return Enums.ToolTypes.SpotDrill;
            case "counter bore":                    return Enums.ToolTypes.Counterbore;
            case "counter sink":                    return Enums.ToolTypes.CounterSink;
            case "reamer":                          return Enums.ToolTypes.Reamer;
            case "boring bar":                      return Enums.ToolTypes.BoringHead;
            case "tap right hand":
            case "tap left hand":                   return Enums.ToolTypes.Tap;

            // --- no HSMAdvisor equivalent: write nothing, user fills it in ----
            case "radius mill":
            case "circle segment barrel":
            case "circle segment oval":
            case "circle segment lens":             return Enums.ToolTypes.Unknown;

            // Anything else (turning, jet, probe...) is not supported.
            default:                                return Enums.ToolTypes.Unknown;
        }
    }

    static Enums.ToolMaterials MapToolMaterial(string m)
    {
        m = (m ?? "").ToLowerInvariant();
        if (m.Contains("carbide")) return Enums.ToolMaterials.Carbide;
        if (m.Contains("cobalt")) return Enums.ToolMaterials.HSCobalt;
        if (m.Contains("hss") || m.Contains("high speed steel")) return Enums.ToolMaterials.HSS;
        if (m.Contains("ceramic")) return Enums.ToolMaterials.Ceramic;
        if (m.Contains("pcd")) return Enums.ToolMaterials.PCD;
        return Enums.ToolMaterials.Carbide;
    }

    // Which geometry fields each HSMAdvisor tool type actually exposes.
    [Flags]
    enum G
    {
        None = 0,
        Dia = 1, TipDia = 2, Flutes = 4, FluteLen = 8, ShoulderLen = 16, ShoulderDia = 32,
        Oal = 64, Shank = 128, Corner = 256, Angle = 512, Pitch = 1024, Stickout = 2048,
        All = Dia | TipDia | Flutes | FluteLen | ShoulderLen | ShoulderDia | Oal | Shank
            | Corner | Angle | Pitch | Stickout
    }

    // Helix angle is deliberately never written for any type.
    static G FieldsFor(Enums.ToolTypes tt)
    {
        switch (tt)
        {
            case Enums.ToolTypes.Tap:
                // Nothing is written for a tap for now.
                return G.None;
            case Enums.ToolTypes.Unknown:
                // Unmapped Fusion types (radius mill, circle segment barrel/oval): write nothing.
                return G.None;
            case Enums.ToolTypes.CounterSink:
                return G.Dia | G.TipDia | G.Flutes | G.Shank | G.Angle | G.Stickout;
            case Enums.ToolTypes.ChamferMill:
            case Enums.ToolTypes.VbitEngraver:
                return G.Dia | G.Flutes | G.FluteLen | G.Shank | G.Angle | G.Stickout;
            case Enums.ToolTypes.JobberTwistDrill:
            case Enums.ToolTypes.SpotDrill:
                return G.Dia | G.Flutes | G.FluteLen | G.Shank | G.Angle | G.Stickout;
            case Enums.ToolTypes.Reamer:
            case Enums.ToolTypes.Counterbore:
                return G.Dia | G.Flutes | G.Stickout;
            case Enums.ToolTypes.BoringHead:
                // Turn dia = diameter, insert length = flute length, "holder size" = shank
                // diameter. Corner radius and angle have no Fusion counterpart.
                return G.Dia | G.Flutes | G.FluteLen | G.Shank | G.Stickout;
            case Enums.ToolTypes.ThreadMill:
                return G.Dia | G.Flutes | G.FluteLen | G.ShoulderLen | G.ShoulderDia
                     | G.Shank | G.Pitch | G.Stickout;
            case Enums.ToolTypes.SolidEndMill:
            case Enums.ToolTypes.IndexedFaceMill:
            case Enums.ToolTypes.WoodRuff:   // t-slot cutter - dovetail / slot mill
                return G.Dia | G.Flutes | G.FluteLen | G.ShoulderLen | G.ShoulderDia
                     | G.Shank | G.Corner | G.Angle | G.Stickout;
            case Enums.ToolTypes.SolidBallMill:
            case Enums.ToolTypes.IndexedBallMill:   //lollipop mill
                return G.Dia | G.Flutes | G.FluteLen | G.ShoulderLen | G.ShoulderDia
                     | G.Shank | G.Angle | G.Stickout;
            default:
                return G.All;
        }
    }

    static void SetGeometry(Calculation calc, string toolType, string toolMaterial, string strategy,
        double dia, double tipDia, int flutes, double cr, double taperAngle, double tipAngle,
        double shoulderDia, double threadPitch, double fl, double sh, double oal, double sd)
    {
        var tt = MapToolType(toolType, strategy);
        var has = FieldsFor(tt);
        calc.SetToolType(tt);
        calc.SetToolMaterial(MapToolMaterial(toolMaterial));
        calc.SetToolCoating(Enums.ToolCoatings.None);

        // Helix = -1 is HSMAdvisor "unknown, use the default" convention,
        // the dialog shows the tool type's default helix for it.
        try { calc.Helix = -1; } catch { }

        // Lengths / flutes / taper / diameter.
        if (has.HasFlag(G.Flutes) && flutes > 0) calc.Flute_N = flutes;
        if (has.HasFlag(G.FluteLen) && fl > 0) calc.Flute_Len = fl;
        if (has.HasFlag(G.ShoulderLen) && sh > 0) calc.Shoulder_Len = sh;
        if (has.HasFlag(G.ShoulderDia) && shoulderDia > 0) calc.Shoulder_Dia = shoulderDia;
        if (has.HasFlag(G.Oal) && oal > 0) calc.OAL_Len = oal;
        // Shank_Dia is shown as "holder size" on a boring head, but it still takes Fusion's
        // shaft diameter - only the label differs.
        if (has.HasFlag(G.Shank) && sd > 0) calc.Shank_Dia = sd;
        if (has.HasFlag(G.Corner) && cr > 0) calc.Corner_Rad = cr;
        if (has.HasFlag(G.Pitch) && threadPitch > 0)
        {
            calc.Thread_Pitch_m = true;
            calc.Thread_Pitch = threadPitch;
        }

        string tl = (toolType ?? "").ToLowerInvariant();
        bool isThread = tl.Contains("thread");
        bool isPointTool = tl.Contains("counter sink") || tl.Contains("drill");
        if (!isThread && has.HasFlag(G.Angle))
        {
            // Set the mode first, then the angle. Point tools (countersink, drills) are
            // defined by their tip angle; everything else by the taper, written even when
            // 0 so the tool cannot keep HSMAdvisor's default lead angle.
            if (isPointTool && tipAngle > 0)
            {
                calc.ToolAngleMode = Enums.ToolAngleModes.Tip;
                calc.ToolAngleTip = tipAngle;
            }
            else
            {
                calc.ToolAngleMode = Enums.ToolAngleModes.Taper;
                calc.ToolAngleTaper = tl.Contains("dovetail") ? -taperAngle : taperAngle;
            }
        }

        // Diameters go in straight from Fusion.
        // HSMAdvisor's single diameter field (backed by Diameter) is labelled "tip
        // diameter", but for most types that label is really the tool diameter. 
        // So feed Fusion's diameter by default, and use Fusion's tip diameter
        // only for the genuinely tapered types, where the tip is a distinct dimension.
        bool useTipDia = tt == Enums.ToolTypes.ChamferMill || tt == Enums.ToolTypes.VbitEngraver;
        calc.Diameter = (useTipDia && tipDia > 0) ? tipDia : dia;

        
        if (has.HasFlag(G.TipDia))
        {
            calc.Tip_Diameter_m = true;
            calc.Tip_Diameter = (tipDia > 0 ? tipDia : dia) / 25.4;
        }
    }

    static string Norm(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";
        var sb = new StringBuilder();
        bool prevSpace = false;
        foreach (char c in s)
        {
            if (char.IsWhiteSpace(c)) { if (!prevSpace) { sb.Append(' '); prevSpace = true; } }
            else { sb.Append(c); prevSpace = false; }
        }
        return sb.ToString().Trim().ToLowerInvariant();
    }

    static Tool FindMatchingTool(string desc, string productId)
    {
        string d = Norm(desc), p = Norm(productId);
        if (d.Length == 0 && p.Length == 0) return null;

        for (int mode = 0; mode <= 1; mode++)
        {
            var hits = Core.FindTools(t =>
            {
                if (t == null) return false;
                string comment = Norm(t.Comment), descr = Norm(t.Description);
                if (mode == 0)
                {
                    if (d.Length > 0 && (d == comment || d == descr)) return true;
                    if (p.Length > 0 && (p == comment || p == descr)) return true;
                    return false;
                }
                if (comment.Length >= 3)
                {
                    if (d.Length > 0 && d.Contains(comment)) return true;
                    if (p.Length > 0 && p.Contains(comment)) return true;
                }
                return false;
            });
            if (hits != null && hits.Count > 0) return hits[0];
        }
        return null;
    }

    // ---- key=value helpers ------------------------------------------------
    static Dictionary<string, string> ReadKvFile(string path)
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (path == null || !File.Exists(path)) return d;
        foreach (var line in File.ReadAllLines(path))
        {
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            d[line.Substring(0, eq).Trim()] = line.Substring(eq + 1);
        }
        return d;
    }

    static void Write(string path, Dictionary<string, string> d)
    {
        if (path == null) return;
        var sb = new StringBuilder();
        foreach (var kv in d) sb.Append(kv.Key).Append('=').Append(kv.Value).Append('\n');
        File.WriteAllText(path, sb.ToString());
    }

    static string Get(Dictionary<string, string> d, string k, string def)
        => d.TryGetValue(k, out var v) ? v : def;
    static double GetD(Dictionary<string, string> d, string k, double def)
        => d.TryGetValue(k, out var v) && double.TryParse(v, NumberStyles.Any, CultureInfo.InvariantCulture, out var r) ? r : def;
    static int GetInt(Dictionary<string, string> d, string k, int def)
        => d.TryGetValue(k, out var v) && int.TryParse(v, out var r) ? r : def;
    static string Inv(double v) => v.ToString(CultureInfo.InvariantCulture);
    static string Inv(int v) => v.ToString(CultureInfo.InvariantCulture);
}

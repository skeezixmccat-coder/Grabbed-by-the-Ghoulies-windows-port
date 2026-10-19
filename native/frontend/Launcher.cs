// Copyright (C) 2026 skeezixmccat-coder. SPDX-License-Identifier: GPL-2.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

public sealed class Settings {
    public string IsoPath = "";
    public int RenderScale = 3;
    public int FrameRate = 30;
    public int Anisotropy = 1;
    public void Validate() {
        if (RenderScale < 1 || RenderScale > 4 || (FrameRate != 30 && FrameRate != 60) ||
            (Anisotropy != 1 && Anisotropy != 2 && Anisotropy != 4 && Anisotropy != 8 && Anisotropy != 16))
            throw new InvalidDataException("Invalid graphics settings. Choose 1x-4x, 30/60 fps and Original/2x/4x/8x/16x filtering.");
        if (IsoPath == null) IsoPath = "";
    }
}

public static class LaunchCore {
    public const string XbeHash = "a2a5bbe51ecc11392612bad30ee4238b796d4d0ed7262e7d23dc4120388294b1";
    public static string Hash(byte[] bytes) {
        using (var h = SHA256.Create()) return BitConverter.ToString(h.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
    }
    public static Settings ReadSettings(string file) {
        var s = File.Exists(file) ? new JavaScriptSerializer().Deserialize<Settings>(File.ReadAllText(file)) : new Settings();
        if (s == null) throw new InvalidDataException("The settings file is empty.");
        s.Validate(); return s;
    }
    public static void SaveSettings(string file, Settings s) {
        s.Validate();
        string temp = file + ".tmp-" + Guid.NewGuid().ToString("N");
        try {
            File.WriteAllText(temp, new JavaScriptSerializer().Serialize(s), new UTF8Encoding(false));
            if (File.Exists(file)) File.Replace(temp, file, null); else File.Move(temp, file);
        } finally { if (File.Exists(temp)) File.Delete(temp); }
    }
    static byte[] ReadAt(Stream f, long offset, int count) {
        if (offset < 0 || count < 0 || offset > f.Length - count) throw new InvalidDataException("Disc entry extends beyond the image.");
        f.Position = offset; byte[] b = new byte[count]; int n = 0, got;
        while (n < count && (got = f.Read(b, n, count - n)) != 0) n += got;
        if (n != count) throw new EndOfStreamException("Short disc read."); return b;
    }
    // Reads only the root directory and executable. The original image is read-only.
    // This runtime supports an XDVDFS image whose game partition begins at byte zero.
    public static byte[] ReadExecutable(Stream disc) {
        byte[] d = ReadAt(disc, 32 * 2048, 2048);
        string magic = "MICROSOFT*XBOX*MEDIA";
        // The on-disc signature occupies exactly 20 bytes.
        byte[] signature = new byte[20]; Encoding.ASCII.GetBytes(magic).CopyTo(signature, 0);
        for (int i = 0; i < 20; ++i) if (d[i] != signature[i] || d[0x7ec+i] != signature[i])
            throw new InvalidDataException("Use an extracted Xbox game-partition ISO (XDVDFS at sector 32). Full-disc and compressed images are not supported.");
        uint sector = BitConverter.ToUInt32(d, 20), length = BitConverter.ToUInt32(d, 24);
        if (length < 14 || length > 16 * 1024 * 1024) throw new InvalidDataException("Invalid disc root directory.");
        byte[] dir = ReadAt(disc, (long)sector * 2048, (int)length);
        var pending = new Stack<int>(); var visited = new HashSet<int>(); pending.Push(0);
        while (pending.Count != 0) {
            int p = pending.Pop();
            if (!visited.Add(p)) throw new InvalidDataException("Cyclic disc directory.");
            if (p < 0 || p > dir.Length - 14) throw new InvalidDataException("Invalid disc directory offset.");
            ushort left = BitConverter.ToUInt16(dir,p), right = BitConverter.ToUInt16(dir,p+2);
            if (left == 0xffff) continue;
            int size = dir[p+13];
            if (p + 14 + size > dir.Length) throw new InvalidDataException("Invalid disc filename length.");
            if (String.Equals(Encoding.ASCII.GetString(dir,p+14,size), "default.xbe", StringComparison.OrdinalIgnoreCase)) {
                uint bytes = BitConverter.ToUInt32(dir,p+8);
                if ((dir[p+12] & 0x10) != 0 || bytes == 0 || bytes > 64 * 1024 * 1024) throw new InvalidDataException("Invalid default.xbe entry.");
                return ReadAt(disc, (long)BitConverter.ToUInt32(dir,p+4)*2048, (int)bytes);
            }
            if (right != 0) pending.Push(right*4); if (left != 0) pending.Push(left*4);
        }
        throw new InvalidDataException("This image has no default.xbe in its root directory.");
    }
    public static string PrepareDisc(string iso, string cache) {
        byte[] xbe;
        using (var f = new FileStream(iso,FileMode.Open,FileAccess.Read,FileShare.Read)) xbe = ReadExecutable(f);
        if (Hash(xbe) != XbeHash) throw new InvalidDataException("Unsupported game revision. This preview requires the documented USA release. No game files were changed.");
        Directory.CreateDirectory(cache);
        string path = Path.Combine(cache,"default.xbe");
        if (!File.Exists(path) || Hash(File.ReadAllBytes(path)) != XbeHash) File.WriteAllBytes(path,xbe);
        return path;
    }
    public static async Task<int> RunProcess(ProcessStartInfo info,string logs) {
        using(var stdout=new FileStream(Path.Combine(logs,"stdout.txt"),FileMode.Create,FileAccess.Write,FileShare.Read))
        using(var stderr=new FileStream(Path.Combine(logs,"stderr.txt"),FileMode.Create,FileAccess.Write,FileShare.Read))
        using(var p=Process.Start(info)) {
            // Start both readers immediately; never wait for exit before draining a pipe.
            await Task.WhenAll(p.StandardOutput.BaseStream.CopyToAsync(stdout),
                p.StandardError.BaseStream.CopyToAsync(stderr),Task.Run(()=>p.WaitForExit()));
            return p.ExitCode;
        }
    }
    public static string Quote(string arg) {
        if (arg.IndexOf('\0') >= 0) throw new ArgumentException("NUL in argument");
        var b = new StringBuilder("\""); int slashes=0;
        foreach(char c in arg) {
            if(c=='\\') { ++slashes; continue; }
            if(c=='"') b.Append('\\',slashes*2+1); else b.Append('\\',slashes);
            b.Append(c); slashes=0;
        }
        return b.Append('\\',slashes*2).Append('"').ToString();
    }
    public static ProcessStartInfo StartInfo(string root, Settings s, string xbe, string log, bool test) {
        s.Validate();
        var p = new ProcessStartInfo(Path.Combine(root,"build","native","ghoulies-native.exe"));
        p.UseShellExecute=false; p.CreateNoWindow=true;
        p.RedirectStandardOutput=true; p.RedirectStandardError=true;
        p.WorkingDirectory=Path.GetDirectoryName(p.FileName);
        p.Arguments=Quote(xbe)+" "+Quote(log)+" 43200";
        // Inherited developer flags must never silently change a user's selected mode.
        var remove = new List<string>();
        foreach(string key in p.EnvironmentVariables.Keys) if(key.StartsWith("GH_",StringComparison.OrdinalIgnoreCase)) remove.Add(key);
        foreach(string key in remove) p.EnvironmentVariables.Remove(key);
        p.EnvironmentVariables["GH_ISO_PATH"]=Path.GetFullPath(s.IsoPath);
        p.EnvironmentVariables["GH_RENDER_SCALE"]=s.RenderScale.ToString();
        p.EnvironmentVariables["GH_ANISOTROPY"]=s.Anisotropy.ToString();
        p.EnvironmentVariables["GH_FRAME_RATE"]=s.FrameRate.ToString();
        p.EnvironmentVariables["GH_PRESENT"]="1";
        p.EnvironmentVariables["GH_KEYBOARD"]="1";
        p.EnvironmentVariables["GH_NO_FRAMES"]="1";
        p.EnvironmentVariables["GH_NO_AUDIO_CAPTURE"]="1";
        if(test) p.EnvironmentVariables["GH_STORAGE_NAME"]="test-run";
        return p;
    }
}

public sealed class LauncherWindow : Form {
    readonly string root, config; readonly bool test;
    readonly TextBox iso = new TextBox();
    readonly ComboBox resolution = new ComboBox(), rate = new ComboBox(), filter = new ComboBox();
    readonly Button play = new Button(); readonly Label status = new Label();
    readonly List<Control> inputs = new List<Control>(); bool playing;
    public LauncherWindow(string directory, bool testProfile) {
        root=directory; test=testProfile; config=Path.Combine(root,"launcher-settings.json");
        Text="Grabbed by the Ghoulies | Native Preview";
        ClientSize=new Size(680,650); MinimumSize=Size; MaximumSize=Size;
        FormBorderStyle=FormBorderStyle.FixedDialog; MaximizeBox=false; StartPosition=FormStartPosition.CenterScreen;
        Font=new Font("Segoe UI",10); BackColor=Color.FromArgb(25,23,31); ForeColor=Color.FromArgb(237,232,222);
        AutoScaleMode=AutoScaleMode.Dpi;
        AddLabel("GRABBED BY THE GHOULIES",28,25,624,38,22,Color.FromArgb(226,189,110));
        AddLabel("Native Windows preview",30,70,620,24,11,Color.FromArgb(175,166,190));
        AddLabel("Your game disc",30,117,620,25,11,ForeColor);
        iso.SetBounds(30,149,510,30); Controls.Add(iso); inputs.Add(iso);
        var browse=new Button {Text="Browse…"}; browse.SetBounds(550,147,100,32); Controls.Add(browse); inputs.Add(browse);
        browse.Click += delegate { using(var dialog=new OpenFileDialog {Filter="Xbox game image (*.iso)|*.iso",CheckFileExists=true}) if(dialog.ShowDialog(this)==DialogResult.OK) iso.Text=dialog.FileName; };
        AddLabel("Supply your own supported game-partition ISO. No game files are included.",30,187,620,34,9,Color.Silver);
        AddLabel("Render resolution",30,235,290,25,11,ForeColor);
        AddLabel("Framerate",355,235,295,25,11,ForeColor);
        SetupCombo(resolution,30,268,290,new object[]{"1× — 640 × 480","2× — 1280 × 960","3× — 1920 × 1440","4× — 2560 × 1920"});
        SetupCombo(rate,355,268,295,new object[]{"Locked 30fps","60fps — experimental"});
        AddLabel("3× recommended for most PCs.",30,306,290,40,10,Color.FromArgb(226,189,110));
        AddLabel("30fps preserves original timing.\n60fps can dip in demanding scenes.",355,306,295,48,9,Color.Silver);
        AddLabel("Anisotropic filtering",30,373,290,25,11,ForeColor);
        SetupCombo(filter,30,406,290,new object[]{"Original","2×","4×","8×","16×"});
        AddLabel("Sharper eligible textures viewed at an angle.\nOptional; limited to your GPU's support.",355,402,295,50,9,Color.Silver);
        AddLabel(test ? "TEST PROFILE  •  Uses your retained test-run saves." : "Saves stay in native-storage / native-run alongside this launcher.",30,472,620,36,9,Color.Silver);
        status.SetBounds(30,515,620,42); status.ForeColor=Color.FromArgb(226,189,110); Controls.Add(status);
        play.Text="Save settings & play"; play.UseMnemonic=false; play.SetBounds(370,577,280,44); play.BackColor=Color.FromArgb(226,189,110); play.ForeColor=Color.FromArgb(25,23,31); play.FlatStyle=FlatStyle.Flat; Controls.Add(play);
        var logs=new Button {Text="Open logs"}; logs.SetBounds(30,583,130,34); Controls.Add(logs);
        logs.Click+=delegate {string d=Path.Combine(root,"logs"); Directory.CreateDirectory(d); Process.Start(new ProcessStartInfo(d){UseShellExecute=true});};
        try {var s=LaunchCore.ReadSettings(config); iso.Text=s.IsoPath; resolution.SelectedIndex=s.RenderScale-1; rate.SelectedIndex=s.FrameRate==60?1:0; filter.SelectedIndex=Array.IndexOf(new int[]{1,2,4,8,16},s.Anisotropy);}
        catch(Exception e) {resolution.SelectedIndex=2; rate.SelectedIndex=0; filter.SelectedIndex=0; status.Text="Settings could not be loaded: "+e.Message;}
        resolution.AccessibleName="Render resolution"; rate.AccessibleName="Framerate"; filter.AccessibleName="Anisotropic filtering"; status.AccessibleName="Launcher status";
        play.Click+=async delegate {await Play();};
        FormClosing+=delegate(object sender,FormClosingEventArgs e) {if(playing) {e.Cancel=true; status.Text="Close the game window before closing the launcher.";}};
    }
    void AddLabel(string text,int x,int y,int w,int h,int size,Color color) {Controls.Add(new Label {Text=text,Location=new Point(x,y),Size=new Size(w,h),Font=new Font("Segoe UI",size),ForeColor=color});}
    void SetupCombo(ComboBox c,int x,int y,int w,object[] choices) {c.ForeColor=Color.Black; c.BackColor=Color.White; c.DropDownStyle=ComboBoxStyle.DropDownList; c.SetBounds(x,y,w,32); c.Items.AddRange(choices); Controls.Add(c); inputs.Add(c);}
    async Task Play() {
        play.Enabled=false; foreach(var c in inputs) c.Enabled=false; playing=true;
        try {
            if(Process.GetProcessesByName("ghoulies-native").Length!=0) throw new InvalidOperationException("A game is already running. Close it before starting another.");
            if(File.Exists(Path.Combine(root,"build","native","helper.lock"))) throw new InvalidOperationException("The development build/test helper is busy.");
            var s=new Settings {IsoPath=iso.Text.Trim(),RenderScale=resolution.SelectedIndex+1,FrameRate=rate.SelectedIndex==1?60:30,Anisotropy=new int[]{1,2,4,8,16}[filter.SelectedIndex]};
            s.Validate();
            if(!File.Exists(Path.Combine(root,"build","native","ghoulies-native.exe")) || !File.Exists(Path.Combine(root,"build","native","ghoulies_runtime.dll"))) throw new FileNotFoundException("The native runtime is missing. Extract the complete preview package, or run tools/build_native.ps1 from source.");
            if(!File.Exists(s.IsoPath)) throw new FileNotFoundException("Select your Xbox game-partition ISO first.");
            s.IsoPath=Path.GetFullPath(s.IsoPath);
            status.Text="Checking your game image…";
            string xbe=await Task.Run(()=>LaunchCore.PrepareDisc(s.IsoPath,Path.Combine(root,"native-storage","launcher-cache")));
            LaunchCore.SaveSettings(config,s);
            string logs=Path.Combine(root,"logs",DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff")); Directory.CreateDirectory(logs);
            File.WriteAllText(Path.Combine(logs,"settings.json"),new JavaScriptSerializer().Serialize(new Settings {IsoPath=Path.GetFileName(s.IsoPath),RenderScale=s.RenderScale,FrameRate=s.FrameRate,Anisotropy=s.Anisotropy}));   // shared with bug reports: image file name only, no folder path
            string report=Path.Combine(logs,"report.txt");
            var info=LaunchCore.StartInfo(root,s,xbe,report,test);
            status.Text="Game running. Close its window to return to graphics options.";
            int code=await LaunchCore.RunProcess(info,logs);
            if(code!=0 && code!=6) throw new InvalidOperationException("The game stopped (code "+code+"). Open logs for the report.");
            status.Text="Game closed. Your settings are saved.";
        } catch(Exception e) {status.Text="Could not start or complete the game."; MessageBox.Show(this,e.Message,"Ghoulies preview",MessageBoxButtons.OK,MessageBoxIcon.Information);}
        finally {playing=false; play.Enabled=true; foreach(var c in inputs)c.Enabled=true;}
    }
}

public static class Program {
    [STAThread] public static int Main(string[] args) {
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        try {
            if(args.Length>0 && args[0]=="--self-test") {LauncherTests.Run(args[1]); return 0;}
            if(args.Length>0 && args[0]=="--prepare-disc") {LaunchCore.PrepareDisc(args[1],args[2]); return 0;}
            if(args.Length==2 && args[0]=="--smoke-test") {
                string root=Path.GetDirectoryName(Application.ExecutablePath), logs=Path.GetFullPath(args[1]);
                if(Process.GetProcessesByName("ghoulies-native").Length!=0) throw new InvalidOperationException("Game already running");
                Directory.CreateDirectory(logs);
                var s=LaunchCore.ReadSettings(Path.Combine(root,"launcher-settings.json"));
                string xbe=LaunchCore.PrepareDisc(s.IsoPath,Path.Combine(root,"native-storage","launcher-cache"));
                var info=LaunchCore.StartInfo(root,s,xbe,Path.Combine(logs,"report.txt"),true);
                info.Arguments=LaunchCore.Quote(xbe)+" "+LaunchCore.Quote(Path.Combine(logs,"report.txt"))+" 20";
                info.EnvironmentVariables.Remove("GH_NO_FRAMES"); // private visual evidence for this bounded test
                int code=LaunchCore.RunProcess(info,logs).GetAwaiter().GetResult();
                File.WriteAllText(Path.Combine(logs,"exit.txt"),code.ToString());
                return code==6?0:1;
            }
            if(args.Length>0 && args[0]=="--preview") {
                using(var f=new LauncherWindow(Path.GetDirectoryName(Application.ExecutablePath),false)) {
                    f.Show(); Application.DoEvents(); using(var b=new Bitmap(f.Width,f.Height)) {f.DrawToBitmap(b,new Rectangle(0,0,b.Width,b.Height));b.Save(args[1]);} f.Close();
                } return 0;
            }
            if(args.Length>1 || (args.Length==1 && args[0]!="--test-profile")) throw new ArgumentException("Unknown launcher option.");
            Application.Run(new LauncherWindow(Path.GetDirectoryName(Application.ExecutablePath),args.Length==1)); return 0;
        } catch(Exception e) {if(args.Length>0) {File.WriteAllText(Path.Combine(Path.GetTempPath(),"ghoulies-launcher-error.txt"),e.ToString()); return 1;} MessageBox.Show(e.Message,"Ghoulies preview");return 1;}
    }
}

// Copyright (C) 2026 skeezixmccat-coder. SPDX-License-Identifier: GPL-2.0-only
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;

public static class LauncherTests {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CommandLineToArgvW(string cmd,out int count);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    static void Check(bool b,string s) {if(!b) throw new Exception(s);}
    static void Reject(Action a) {try {a();} catch(InvalidDataException) {return;} throw new Exception("Malformed data accepted");}
    public static void Run(string output) {
        Directory.CreateDirectory(output);
        string config=Path.Combine(output,"synthetic-settings.json");
        var defaults=LaunchCore.ReadSettings(config);
        Check(defaults.FrameRate==30 && defaults.RenderScale==3 && defaults.Anisotropy==1,"defaults");
        var settings=new Settings {IsoPath="C:\\synthetic path Ω\\game.iso",FrameRate=60,RenderScale=4,Anisotropy=16};
        LaunchCore.SaveSettings(config,settings); var loaded=LaunchCore.ReadSettings(config);
        Check(loaded.IsoPath==settings.IsoPath && loaded.FrameRate==60 && loaded.RenderScale==4 && loaded.Anisotropy==16,"round trip");
        settings.FrameRate=30; LaunchCore.SaveSettings(config,settings);
        Check(LaunchCore.ReadSettings(config).FrameRate==30,"replace settings");
        Reject(()=>new Settings {FrameRate=120}.Validate()); Reject(()=>new Settings {RenderScale=0}.Validate()); Reject(()=>new Settings {Anisotropy=3}.Validate());
        foreach(var arg in new[]{"", "C:\\spaces Ω\\", "a\"b", "C:\\test\\file.xbe", "back\\\"quote", "normal"}) {
            int n; IntPtr p=CommandLineToArgvW("program "+LaunchCore.Quote(arg),out n);
            try {Check(n==2 && Marshal.PtrToStringUni(Marshal.ReadIntPtr(p,IntPtr.Size))==arg,"argument quoting");} finally {LocalFree(p);}
        }
        Environment.SetEnvironmentVariable("GH_TIMING_FIELDS","1"); Environment.SetEnvironmentVariable("GH_PRESENT_INTERVAL","1");
        Environment.SetEnvironmentVariable("GH_VBLANK_HZ","119.88"); Environment.SetEnvironmentVariable("GH_AUDIO_TRACE","1");
        foreach(int fps in new[]{30,60}) foreach(int scale in new[]{1,2,3,4}) foreach(int af in new[]{1,2,4,8,16}) {
            var s=new Settings {IsoPath=settings.IsoPath,FrameRate=fps,RenderScale=scale,Anisotropy=af};
            var p=LaunchCore.StartInfo(output,s,"C:\\cache\\default.xbe","C:\\log Ω\\report.txt",false);
            Check(p.EnvironmentVariables["GH_FRAME_RATE"]==fps.ToString() && p.EnvironmentVariables["GH_RENDER_SCALE"]==scale.ToString() && p.EnvironmentVariables["GH_ANISOTROPY"]==af.ToString(),"option mapping");
            Check(p.RedirectStandardOutput && p.RedirectStandardError && p.EnvironmentVariables["GH_NO_AUDIO_CAPTURE"]=="1","stream/capture policy");
            foreach(string k in new[]{"GH_TIMING_FIELDS","GH_PRESENT_INTERVAL","GH_VBLANK_HZ","GH_AUDIO_TRACE","GH_STORAGE_NAME"}) Check(!p.EnvironmentVariables.ContainsKey(k),"inherited override: "+k);
        }
        Check(LaunchCore.StartInfo(output,settings,"x","y",true).EnvironmentVariables["GH_STORAGE_NAME"]=="test-run","test profile");
        byte[] image=new byte[40*2048]; byte[] signature=Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");
        signature.CopyTo(image,32*2048); signature.CopyTo(image,32*2048+0x7ec);
        BitConverter.GetBytes((uint)33).CopyTo(image,32*2048+20); BitConverter.GetBytes((uint)2048).CopyTo(image,32*2048+24);
        int root=33*2048; BitConverter.GetBytes((uint)34).CopyTo(image,root+4); BitConverter.GetBytes((uint)4).CopyTo(image,root+8);
        image[root+13]=11; Encoding.ASCII.GetBytes("default.xbe").CopyTo(image,root+14);
        image[34*2048]=7; image[34*2048+3]=9;
        using(var stream=new MemoryStream(image,false)) {var x=LaunchCore.ReadExecutable(stream);Check(x.Length==4 && x[0]==7 && x[3]==9,"synthetic executable read");}
        byte[] bad=(byte[])image.Clone(); bad[32*2048]=0; Reject(()=>{using(var f=new MemoryStream(bad,false))LaunchCore.ReadExecutable(f);});
        bad=(byte[])image.Clone(); BitConverter.GetBytes(UInt32.MaxValue).CopyTo(bad,root+4); Reject(()=>{using(var f=new MemoryStream(bad,false))LaunchCore.ReadExecutable(f);});
        bad=(byte[])image.Clone(); BitConverter.GetBytes(UInt32.MaxValue).CopyTo(bad,32*2048+24); Reject(()=>{using(var f=new MemoryStream(bad,false))LaunchCore.ReadExecutable(f);});
        bad=(byte[])image.Clone(); bad[root+14]=(byte)'z'; BitConverter.GetBytes((ushort)8).CopyTo(bad,root); BitConverter.GetBytes((ushort)8).CopyTo(bad,root+32); Reject(()=>{using(var f=new MemoryStream(bad,false))LaunchCore.ReadExecutable(f);});
        string fake=Path.Combine(output,"synthetic.iso"); File.WriteAllBytes(fake,image);
        Reject(()=>LaunchCore.PrepareDisc(fake,Path.Combine(output,"rejected-cache")));
        Check(!Directory.Exists(Path.Combine(output,"rejected-cache")),"rejected disc wrote cache"); File.Delete(fake);
        File.Delete(config);
        File.WriteAllText(Path.Combine(output,"launcher-tests.txt"),"PASS: defaults; atomic settings roundtrip; invalid options; Windows argument quoting; 40 option combinations; inherited timing/diagnostic isolation; test-profile confinement; synthetic XDVDFS read; malformed header/bounds/cycle; unsupported revision leaves no cache.\r\n");
    }
}

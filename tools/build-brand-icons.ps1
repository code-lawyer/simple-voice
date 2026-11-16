param([string]$OutputDir = 'assets/brand')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDir))
if (-not $output.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must stay in the project.' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
Add-Type -AssemblyName System.Drawing
# Authored vector geometry, not a tracing or a modification of the concept raster.
# Both ICO and SVG are emitted from the same three path commands below.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Text;
public static class SimpleVoiceIcons {
  static readonly int[] Sizes = {16,20,24,28,32,40,48,64,128,256};
  // M/L/C commands only, with stroke width in the 24-unit coordinate space.
  static readonly string[] Core = {
    "M 17 21 L 9 21 C 5.6863 21 3 18.3137 3 15 L 3 9 C 3 5.6863 5.6863 3 9 3 L 15 3 C 18.3137 3 21 5.6863 21 9 L 21 16",
    "M 12 9.4 L 12 14.6"
  };
  static readonly string[] Chinese = {"M 5 7 L 19 7 L 19 16 L 5 16 L 5 7", "M 12 3 L 12 21"};
  static readonly string[] English = {"M 5 20 L 11.4 4 L 12.6 4 L 19 20", "M 8 14 L 16 14"};
  static GraphicsPath Path(string command) {
    string[] t=command.Split(' '); int i=0; PointF last=new PointF();
    var path=new GraphicsPath();
    Func<float> n=()=>float.Parse(t[i++],CultureInfo.InvariantCulture);
    while(i<t.Length) {
      string op=t[i++];
      if(op=="M") {last=new PointF(n(),n()); path.StartFigure();}
      else if(op=="L") {var end=new PointF(n(),n());path.AddLine(last,end);last=end;}
      else if(op=="C") {var a=new PointF(n(),n());var b=new PointF(n(),n());var end=new PointF(n(),n());path.AddBezier(last,a,b,end);last=end;}
      else throw new InvalidDataException(op);
    }
    return path;
  }
  static Bitmap Render(int size,string[] paths,float[] widths,Color ink,bool badge) {
    const int sample=8;
    using(var large=new Bitmap(size*sample,size*sample,PixelFormat.Format32bppArgb)) {
      using(var g=Graphics.FromImage(large)) {
        g.Clear(Color.Transparent);g.SmoothingMode=SmoothingMode.AntiAlias;
        g.ScaleTransform(size*sample/24f,size*sample/24f);
        if(badge) {
          using(var plate=new GraphicsPath()) {
            plate.AddArc(.5f,.5f,5,5,180,90);plate.AddArc(18.5f,.5f,5,5,270,90);
            plate.AddArc(18.5f,18.5f,5,5,0,90);plate.AddArc(.5f,18.5f,5,5,90,90);plate.CloseFigure();
            g.FillPath(Brushes.White,plate);
            using(var edge=new Pen(Color.FromArgb(128,0,0,0),.8f))g.DrawPath(edge,plate);
          }
          g.TranslateTransform(3,3);g.ScaleTransform(.75f,.75f);
        }
        for(int i=0;i<paths.Length;i++) using(var p=Path(paths[i])) using(var pen=new Pen(ink,widths[i])) {
          pen.StartCap=pen.EndCap=LineCap.Round;pen.LineJoin=LineJoin.Round;g.DrawPath(pen,p);
        }
      }
      var bitmap=new Bitmap(size,size,PixelFormat.Format32bppArgb);
      using(var g=Graphics.FromImage(bitmap)) {
        g.CompositingMode=CompositingMode.SourceCopy;g.InterpolationMode=InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode=PixelOffsetMode.HighQuality;
        g.DrawImage(large,new Rectangle(0,0,size,size),0,0,large.Width,large.Height,GraphicsUnit.Pixel);
      }
      return bitmap;
    }
  }
  static void Save(string dir,string name,string[] paths,float[] widths,Color ink,bool badge) {
    var frames=new List<byte[]>();
    foreach(int size in Sizes) using(var b=Render(size,paths,widths,ink,badge)) using(var m=new MemoryStream()) {
      b.Save(m,ImageFormat.Png);frames.Add(m.ToArray());
    }
    using(var w=new BinaryWriter(File.Create(System.IO.Path.Combine(dir,name+".ico")))) {
      w.Write((ushort)0);w.Write((ushort)1);w.Write((ushort)Sizes.Length);
      uint offset=(uint)(6+16*Sizes.Length);
      for(int i=0;i<Sizes.Length;i++) {
        w.Write((byte)(Sizes[i]==256?0:Sizes[i]));w.Write((byte)(Sizes[i]==256?0:Sizes[i]));w.Write((byte)0);w.Write((byte)0);
        w.Write((ushort)1);w.Write((ushort)32);w.Write((uint)frames[i].Length);w.Write(offset);offset+=(uint)frames[i].Length;
      }
      foreach(var frame in frames)w.Write(frame);
    }
    if(!badge) {
      var svg=new StringBuilder("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n");
      for(int i=0;i<paths.Length;i++)svg.AppendFormat(CultureInfo.InvariantCulture,"  <path d=\"{0}\" stroke=\"{1}\" stroke-width=\"{2}\"/>\n",paths[i],ink==Color.White?"#fff":"#000",widths[i]);
      svg.Append("</svg>\n");File.WriteAllText(System.IO.Path.Combine(dir,name+".svg"),svg.ToString(),new UTF8Encoding(false));
    }
  }
  public static void Build(string dir) {
    Save(dir,"simple-voice",Core,new float[]{3.4f,4.4f},Color.Black,true);
    Save(dir,"sound-core-white",Core,new float[]{3.4f,4.4f},Color.White,false);
    Save(dir,"sound-core-black",Core,new float[]{3.4f,4.4f},Color.Black,false);
    Save(dir,"mode-chinese",Chinese,new float[]{1.8f,1.8f},Color.Black,false);
    Save(dir,"mode-english",English,new float[]{1.8f,1.8f},Color.Black,false);
    Save(dir,"mode-chinese-white",Chinese,new float[]{1.8f,1.8f},Color.White,false);
    Save(dir,"mode-english-white",English,new float[]{1.8f,1.8f},Color.White,false);
  }
}
'@
[SimpleVoiceIcons]::Build($output)
Write-Output "Built 7 icon families, each with 10 native sizes, in $output"

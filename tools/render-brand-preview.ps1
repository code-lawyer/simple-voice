param([string]$BuildDir='build-professional', [string]$Output='docs/design/brand/sound-core-preview.png')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$dll=Join-Path $root "$BuildDir/voiceime_tsf.dll"
$destination=[IO.Path]::GetFullPath((Join-Path $root $Output))
if(-not $destination.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Preview must stay inside the project.'}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class BrandPreview {
  [DllImport("kernel32",CharSet=CharSet.Unicode)]static extern IntPtr LoadLibraryEx(string path,IntPtr file,uint flags);
  [DllImport("kernel32")]static extern bool FreeLibrary(IntPtr module);
  [DllImport("user32",CharSet=CharSet.Unicode)]static extern IntPtr LoadImage(IntPtr module,IntPtr name,uint type,int width,int height,uint flags);
  [DllImport("user32")]static extern bool DrawIconEx(IntPtr dc,int x,int y,IntPtr icon,int w,int h,uint step,IntPtr brush,uint flags);
  [DllImport("user32")]static extern bool DestroyIcon(IntPtr icon);
  static void Icon(Graphics g,IntPtr module,int id,int x,int y,int size) {
    IntPtr icon=LoadImage(module,new IntPtr(id),1,size,size,0);
    if(icon==IntPtr.Zero)throw new Exception("Missing icon resource "+id);
    IntPtr dc=g.GetHdc();
    try {if(!DrawIconEx(dc,x,y,icon,size,size,0,IntPtr.Zero,3))throw new Exception("DrawIconEx failed");}
    finally {g.ReleaseHdc(dc);DestroyIcon(icon);}
  }
  static void Text(Graphics g,string value,float x,float y,float size,Color color,bool bold=false) {
    using(var font=new Font("Microsoft YaHei UI",size,bold?FontStyle.Bold:FontStyle.Regular,GraphicsUnit.Pixel))
    using(var brush=new SolidBrush(color))g.DrawString(value,font,brush,x,y);
  }
  public static void Render(string dll,string output) {
    IntPtr module=LoadLibraryEx(dll,IntPtr.Zero,2);
    if(module==IntPtr.Zero)throw new Exception("Load resources failed");
    try {using(var b=new Bitmap(1040,590))using(var g=Graphics.FromImage(b)) {
      g.Clear(Color.FromArgb(249,249,248));g.TextRenderingHint=System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
      var ink=Color.FromArgb(24,29,33);var secondary=Color.FromArgb(85,96,103);
      Text(g,"\u8f7b\u97f3\u8f93\u5165\u6cd5",40,30,30,ink,true);
      Text(g,"Simple Voice  /  \u58f0\u6838",40,72,19,secondary);
      Icon(g,module,102,66,150,192);
      Text(g,"\u6781\u7b80\u3001\u672c\u5730",66,374,21,ink);
      Text(g,"\u58f0\u97f3\uff0c\u7559\u5728\u672c\u673a",66,410,16,secondary);
      using(var pen=new Pen(Color.FromArgb(217,220,221)))g.DrawLine(pen,322,140,322,455);
      Text(g,"\u771f\u5b9e\u5c3a\u5bf8\u9884\u89c8",362,123,21,ink,true);
      Text(g,"16 / 20 / 24 / 32 / 40 / 48 px",362,158,15,secondary);
      using(var brush=new SolidBrush(Color.FromArgb(32,33,35)))g.FillRectangle(brush,362,204,632,92);
      using(var brush=new SolidBrush(Color.FromArgb(235,237,238)))g.FillRectangle(brush,362,318,632,92);
      int[] sizes={16,20,24,32,40,48};
      for(int i=0;i<sizes.Length;i++) {
        int x=403+i*98;Icon(g,module,101,x,250-sizes[i]/2,sizes[i]);Icon(g,module,102,x,364-sizes[i]/2,sizes[i]);
      }
      Text(g,"\u7cfb\u7edf\u8f93\u5165\u6cd5\u54c1\u724c\u5fbd\u6807",362,437,15,secondary);
      Icon(g,module,100,371,473,24);Icon(g,module,100,415,475,20);Icon(g,module,100,455,477,16);
      Text(g,"\u6a21\u5f0f",600,437,15,secondary);Icon(g,module,103,607,473,24);Icon(g,module,104,651,473,24);
      Text(g,"\u539f\u751f ICO \u8d44\u6e90\u6e32\u67d3 \u00b7 \u975e\u4efb\u52a1\u680f\u5b9e\u622a\u56fe",40,545,14,secondary);
      b.Save(output,ImageFormat.Png);
    }}finally{FreeLibrary(module);}
  }
}
'@
[BrandPreview]::Render($dll,$destination)
Write-Output $destination

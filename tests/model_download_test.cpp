#include "harness.hpp"
#include "model_download.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <chrono>
namespace fs=std::filesystem;
void ModelDownloadUsesCuratedIdAndCanBeCancelled() {
  const auto root=fs::absolute(fs::path(L"build-review/download-tests")/std::to_wstring(GetCurrentProcessId()));
  fs::create_directories(root/L"tools");
  const auto script=root/L"tools/download_models.ps1";
  {std::ofstream out(script);out<<"param([string]$Root,[string]$OnlyModel,[string]$StatusFile)\n[IO.File]::WriteAllText($StatusFile,$OnlyModel)\nexit 0\n";}
  std::atomic<bool> cancel{false};std::wstring message;
  CHECK(DownloadModel(root.wstring(),(root/L"data").wstring(),SpeechModel::SenseVoice,cancel,message));
  CHECK(message==L"sensevoice");
  {std::ofstream out(script);out<<"param([string]$Root,[string]$OnlyModel,[string]$StatusFile)\nStart-Sleep -Seconds 60\n[IO.File]::WriteAllText($StatusFile,'finished')\n";}
  auto result=std::async(std::launch::async,[&]{return DownloadModel(root.wstring(),(root/L"data").wstring(),SpeechModel::Whisper,cancel,message);});
  std::this_thread::sleep_for(std::chrono::milliseconds(150));cancel=true;
  CHECK(result.wait_for(std::chrono::seconds(3))==std::future_status::ready);CHECK(!result.get());
  CHECK(message.find(L"取消")!=std::wstring::npos);

  // Exercise the shipped PowerShell selection/extraction path using tiny,
  // SHA-verified local archives. -Offline makes this test network-independent.
  fs::copy_file(fs::path(VOICEIME_SOURCE_DIR)/L"tools/download_models.ps1",
      root/L"tools/real_download_models.ps1",fs::copy_options::overwrite_existing);
  {std::ofstream out(script);out<<R"PS(param([string]$Root,[string]$OnlyModel,[string]$StatusFile)
$ErrorActionPreference='Stop'
trap {[IO.File]::WriteAllText($StatusFile,$_.Exception.ToString());exit 1}
foreach($module in @('Microsoft.PowerShell.Management','Microsoft.PowerShell.Utility')) {
  Import-Module (Join-Path $PSHOME "Modules/$module/$module.psd1") -ErrorAction Stop
}
$artifacts=@()
foreach($id in @('runtime','streaming-bilingual','final-sense','final-qwen','punct')) {
  $folder='fixture-'+$id
  $source=Join-Path (Join-Path $Root 'source') $folder
  [IO.Directory]::CreateDirectory($source) | Out-Null
  $file=Join-Path $source 'weight.txt'
  [IO.File]::WriteAllText($file,'verified-'+$id)
  $cache=Join-Path $Root '.downloads'
  [IO.Directory]::CreateDirectory($cache) | Out-Null
  $archive=Join-Path $cache ($folder+'.tar.bz2')
  & tar.exe -cjf $archive -C (Join-Path $Root 'source') $folder
  if($LASTEXITCODE) {throw 'fixture archive creation failed'}
  $artifacts+=@{Id=$id;Folder=$folder;Destination=('models/'+$id);Url='https://invalid.invalid/no-network';
    Sha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash;
    Files=@{'weight.txt'=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}}
}
@{artifacts=$artifacts} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Encoding UTF8
$broken=Join-Path $Root 'models/streaming-bilingual'
[IO.Directory]::CreateDirectory($broken) | Out-Null
[IO.File]::WriteAllText((Join-Path $broken 'weight.txt'),'incomplete shared model')
$actual=Join-Path $PSScriptRoot 'real_download_models.ps1'
$env:PSModulePath=Join-Path $Root 'unavailable-modules'
Remove-Module Microsoft.PowerShell.Management,Microsoft.PowerShell.Utility -Force
& $actual -Root $Root -OnlyModel $OnlyModel -Offline -StatusFile $StatusFile
)PS";}
  cancel=false;
  const auto offline=root/L"offline-catalog";
  CHECK(DownloadModel(root.wstring(),offline.wstring(),SpeechModel::SenseVoice,cancel,message));
  std::ifstream shared(offline/L"models/streaming-bilingual/weight.txt");std::string shared_text;std::getline(shared,shared_text);
  CHECK(shared_text=="verified-streaming-bilingual");
  CHECK(fs::exists(offline/L"models/final-sense/weight.txt"));
  CHECK(!fs::exists(offline/L"models/runtime"));
  CHECK(!fs::exists(offline/L"models/final-qwen"));
  CHECK(!fs::exists(offline/L"models/punct"));
}

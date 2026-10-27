param([int]$GameProcessId = 0, [switch]$Navigation)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class MediconMenuProbe {
 [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] output, UIntPtr size, out UIntPtr copied);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
"@
$gameProcess = if ($GameProcessId) { Get-Process -Id $GameProcessId } else { Get-Process Minecraft.Windows -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (!$gameProcess) { throw 'Minecraft.Windows.exe is not running. This tool only reads a running game; it never launches or injects it.' }
$gameVersion = $gameProcess.MainModule.FileVersionInfo
if ($gameVersion.FileMajorPart -ne 1 -or $gameVersion.FileMinorPart -ne 26 -or $gameVersion.FileBuildPart -ne 52 -or $gameVersion.FilePrivatePart -ne 3) {
 throw 'This read-only probe is limited to the inspected Minecraft 1.26.52.3 profile.'
}
$readerHandle = [MediconMenuProbe]::OpenProcess(0x1010, $false, $gameProcess.Id)
if ($readerHandle -eq [IntPtr]::Zero) { throw "OpenProcess read-only failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
function Read-Bytes([UInt64]$Address, [int]$Count) {
 $buffer = [byte[]]::new($Count); $copied = [UIntPtr]::Zero
 if (!$Address -or ![MediconMenuProbe]::ReadProcessMemory($readerHandle, [IntPtr]::new([Int64]$Address), $buffer, [UIntPtr]::new([UInt64]$Count), [ref]$copied) -or $copied.ToUInt64() -ne $Count) { throw ('Unreadable 0x{0:X} ({1} bytes)' -f $Address,$Count) }
 return ,$buffer
}
function Read-Ptr([UInt64]$Address) { [BitConverter]::ToUInt64((Read-Bytes $Address 8),0) }
function Read-Byte([UInt64]$Address) { (Read-Bytes $Address 1)[0] }
function Read-Name([UInt64]$Address) {
 $raw = Read-Bytes $Address 32; $length = [BitConverter]::ToUInt64($raw,16); $capacity = [BitConverter]::ToUInt64($raw,24)
 if ($length -gt 96 -or $length -gt $capacity -or $capacity -gt 1024) { throw 'Invalid MSVC screen string' }
 $chars = if ($capacity -lt 16) { $raw } else { Read-Bytes ([BitConverter]::ToUInt64($raw,0)) ([int]$length + 1) }
 [Text.Encoding]::UTF8.GetString($chars,0,[int]$length)
}
$result = [ordered]@{ processId=$gameProcess.Id; executable=$gameProcess.Path; loadedDlls=@($gameProcess.Modules | Where-Object ModuleName -eq 'Medicon.dll' | ForEach-Object FileName) }
try {
 $imageBase = [UInt64]$gameProcess.MainModule.BaseAddress.ToInt64(); $result.imageBase=('0x{0:X}' -f $imageBase)
 # This site is from the existing solstice MinecraftGame_instance signature,
 # independently matched on the inspected 1.26.52.3 PE; reject a mismatch.
 $site = $imageBase + 0x61A4FCA; $instruction = Read-Bytes $site 10
 if ($instruction[0] -ne 0x48 -or $instruction[1] -ne 0x89 -or $instruction[2] -ne 0x15 -or $instruction[7] -ne 0x49 -or $instruction[8] -ne 0x8B -or $instruction[9] -ne 0x40) { throw 'Existing MinecraftGame_instance profile mismatch' }
 $global = [UInt64]([Int64]$site + 7 + [BitConverter]::ToInt32($instruction,3)); $game = Read-Ptr $global
 $result.game=('0x{0:X}' -f $game); $head=Read-Ptr ($game+0x970); $node=Read-Ptr $head
 $result.clientCount=Read-Ptr ($game+0x978); $result.node=('0x{0:X}' -f $node)
 $candidates=@()
 foreach ($nodeOffset in @(0x28,0x30)) {
  $candidate=[ordered]@{nodeOffset=('0x{0:X}' -f $nodeOffset)}
  try {
   $client=Read-Ptr ($node+$nodeOffset); $candidate.client=('0x{0:X}' -f $client); $table=Read-Ptr $client
   $candidate.methods=@{}; foreach($slot in @(162,229,238,240,246,310,311)){ $candidate.methods["$slot"] = ('0x{0:X}' -f ((Read-Ptr ($table+8*$slot))-$imageBase)) }
   $candidate.associatedGame=('0x{0:X}' -f (Read-Ptr ($client+0x1A8))); $candidate.nonprimary=Read-Byte ($client+0x678)
   $model=Read-Ptr ($client+0x208); $candidate.model=('0x{0:X}' -f $model); $candidate.modelValid=Read-Byte (Read-Ptr ($model+8)); $candidate.modelClient=('0x{0:X}' -f (Read-Ptr ($model+0x18))); $candidate.alternate=Read-Byte ($model+0x40)
   $stack=Read-Ptr ($game+0x1D70); $candidate.stack=('0x{0:X}' -f $stack); $candidate.stackValid=Read-Byte (Read-Ptr ($stack+8)); $candidate.stackMethod44=('0x{0:X}' -f ((Read-Ptr ((Read-Ptr $stack)+44*8))-$imageBase))
   $begin=Read-Ptr ($stack+0x18); $end=Read-Ptr ($stack+0x20); $candidate.sceneBytes=$end-$begin; $candidate.begin=('0x{0:X}' -f $begin); $candidate.end=('0x{0:X}' -f $end); $candidate.vectorWords=@(for($word=$begin; $word -lt $end -and $word -lt $begin+256; $word+=8) { '0x{0:X}' -f (Read-Ptr $word) })
   $scenes=@(); for($entry=$end-24; $entry -eq $end-24; $entry+=24){
    $scene=[ordered]@{address=('0x{0:X}' -f (Read-Ptr $entry))}
    try {
     $top=Read-Ptr $entry; $scene.method55=('0x{0:X}' -f ((Read-Ptr ((Read-Ptr $top)+55*8))-$imageBase))
     $view=Read-Ptr ($top+0x40); $scene.view=('0x{0:X}' -f $view); $definition=Read-Ptr ((Read-Ptr ($view+0x50))+8); $scene.name=Read-Name ($definition+0x20)
     $controller=Read-Ptr ($view+0x40); $scene.controller=('0x{0:X}' -f $controller); $scene.suffixMethod27=('0x{0:X}' -f ((Read-Ptr ((Read-Ptr $controller)+27*8))-$imageBase))
     if ($Navigation) {
      $scene.sceneTableRva=('0x{0:X}' -f ((Read-Ptr $top)-$imageBase))
      $scene.controllerTableRva=('0x{0:X}' -f ((Read-Ptr $controller)-$imageBase))
      if ((Read-Ptr $controller) -eq $imageBase+0xE900070 -and (Read-Ptr $top) -eq $imageBase+0xE8E9080) {
       $scene.nativeUpdateRva=('0x{0:X}' -f ((Read-Ptr ((Read-Ptr $controller)+0x10))-$imageBase))
       $scene.inputBlocked=Read-Byte ($view+0x460); $scene.inputClosing=Read-Byte ($view+0x465)
       $scene.quitRequested=Read-Byte ($game+0xB1)
       $scene.modelPrimary=('0x{0:X}' -f (Read-Ptr ($controller+0xD48)))
       $scene.sameModelPair=((Read-Ptr ($controller+0xD48)) -eq (Read-Ptr ($controller+0xBF0)) -and (Read-Ptr ($controller+0xD50)) -eq (Read-Ptr ($controller+0xBF8)))
       $eventSentinel=Read-Ptr ($controller+0x828); $eventCount=Read-Ptr ($controller+0x830)
       if ($eventCount -gt 1024) { throw 'Unverified native registration count' }
       $eventNode=Read-Ptr $eventSentinel; $bindings=@()
       for ($eventIndex=0; $eventIndex -lt $eventCount -and $eventNode -ne $eventSentinel; $eventIndex++) {
        $eventKey=[BitConverter]::ToUInt32((Read-Bytes ($eventNode+0x10) 4),0)
        if ($eventKey -in @([UInt32]3372022197,[UInt32]4202751130,[UInt32]116523271)) {
         $callbackBegin=Read-Ptr ($eventNode+0x18); $callbackEnd=Read-Ptr ($eventNode+0x20)
         if ($callbackEnd -lt $callbackBegin -or $callbackEnd-$callbackBegin -gt 64*256 -or ($callbackEnd-$callbackBegin)%64) { throw 'Unverified native callable range' }
         $callbacks=@(for ($callbackAt=$callbackBegin; $callbackAt -lt $callbackEnd -and $callbackAt -lt $callbackBegin+64*4; $callbackAt+=64) {
          $operationTable=Read-Ptr $callbackAt
          [ordered]@{tableRva=('0x{0:X}' -f ($operationTable-$imageBase)); invokeRva=('0x{0:X}' -f ((Read-Ptr ($operationTable+0x10))-$imageBase)); captureMatches=((Read-Ptr ($callbackAt+8)) -eq $controller)}
         })
         $bindings+=[ordered]@{key=$eventKey; count=($callbackEnd-$callbackBegin)/64; callbacks=$callbacks}
        }
        $eventNode=Read-Ptr $eventNode
       }
       $scene.registrations=$bindings
      }
     }
    } catch { $scene.error=$_.Exception.Message }
    $scenes+=$scene
   }
   $candidate.scenes=$scenes
  } catch { $candidate.error=$_.Exception.Message }
  $candidates+=$candidate
 }
 $result.candidates=$candidates
} catch { $result.error=$_.Exception.Message }
finally { [void][MediconMenuProbe]::CloseHandle($readerHandle) }
$result | ConvertTo-Json -Depth 8


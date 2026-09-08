# Save transfer between Quest standalone and PCVR

Use **TRANSFER_QUEST_SAVES_TO_PC.bat** for standalone Quest to Windows PCVR,
or **TRANSFER_PC_SAVES_TO_QUEST.bat** for the reverse direction. Keep the BAT
files and the complete `tools` folder together; a BAT downloaded by itself
will not work.

1. Save your progress, then close Vice City VR on both devices.
2. Enable developer mode / USB debugging on the Quest, connect it by USB,
   and accept **Allow USB debugging** in the headset. A Link session is not
   required. The standalone app must be installed and support its save provider.
3. Run the BAT for the direction you want. The wizard finds an existing ADB
   installation or offers to download Google's official platform-tools.
4. Select your installed PCVR folder containing `reVC.exe`, or the actual save
   folder. For Quest to PC, a fresh `userfiles` folder can be created beside
   `reVC.exe`; you do not need an existing PC save.
5. Check the displayed destination and slots, then type **Y**. All occupied
   source slots are transferred to the same slot numbers. Empty source slots
   do not erase destination saves.
6. Wait for **SUCCESS**, start the destination game, and load the slot.

If PCVR uses the Documents fallback instead of its local `userfiles` folder,
select `Documents\GTA Vice City User Files` as the destination explicitly.
Do not run as administrator to bypass an unwritable game folder.

The converter handles the different garage/car layouts; do not simply rename
Quest `1.b` to PC `GTAVCsf1.b`. This tool targets compatible Vice City VR saves,
not original retail/x86 or Definitive Edition saves. Invalid size/checksum
inputs are rejected. Actual loading of a transferred save still needs checking
in the destination game, especially when transferring between different versions.

Existing destination saves are copied into timestamped folders under
`<PC save folder>\MiamiVR-save-backups` before replacement. Quest to PC leaves
the source Quest saves unchanged. A failed later slot does not roll back earlier
successful slots; read the reported error and keep the backups. To restore a PC
slot, close PCVR and copy the backed-up `GTAVCsfN.b` into the PC save folder.

Logs are written to `%TEMP%\ViceCityVR-QuestToPC.log` or
`%TEMP%\ViceCityVR-PCToQuest.log`.

Advanced examples (PowerShell, from the extracted kit directory):

```powershell
.\TRANSFER_QUEST_SAVES_TO_PC.bat -PcGameDirectory "D:\Games\Vice City VR" -AdbPath "C:\platform-tools\adb.exe"
.\TRANSFER_QUEST_SAVES_TO_PC.bat -PcGameDirectory "D:\Games\Vice City VR" -Serial YOUR_QUEST_SERIAL
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\saves\transfer-vr-save.ps1 -Mode Export -Slot 1 -PcSaveDirectory "D:\Games\Vice City VR\userfiles" -AdbPath "C:\platform-tools\adb.exe"
```

The single-slot command uses an existing PC save directory and does not offer
the all-slots wizard confirmation. Keep both games closed before using it.

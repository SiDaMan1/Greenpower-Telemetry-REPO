' Hidden-window launcher for the agent, used by the Startup-folder shortcut
' both installers (Inno Setup .exe AND this WiX .msi) register. Unlike the
' Inno Setup version (which generates this file at install time with the
' install path baked in, via a [Code] section — see GreenpowerAgent.iss),
' this one is SELF-LOCATING: it finds its own directory at runtime via
' WScript.ScriptFullName, so the exact same static file works no matter
' where it ends up installed. WiX's <Shortcut> element can point straight
' at this file as installed (no per-install-path templating needed on the
' WiX side), which is simpler to author than the Inno Setup equivalent.
' The ", 0, False" is what makes it run with no visible console window —
' same as the Inno Setup version, same as the original hand-written
' setup.bat launcher this whole thing traces back to.
Set fso = CreateObject("Scripting.FileSystemObject")
scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
Set WshShell = CreateObject("WScript.Shell")
WshShell.CurrentDirectory = scriptDir
WshShell.Run "node agent.js", 0, False

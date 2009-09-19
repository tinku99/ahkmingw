start:
ahkdll := DllCall("LoadLibrary", "str", A_ScriptDir . "\AutoHotkey.dll")
msgbox % "loading dll" . ErrorLevel
result := DllCall(A_ScriptDir . "\AutoHotkey.dll\ahkdll", "str", "msgbox.ahk", "str"
, "", "str", "", "cdecl") 
return


q::ExitApp

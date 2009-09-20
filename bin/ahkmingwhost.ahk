ahkdll := DllCall("LoadLibrary", "str", A_ScriptDir . "\ahkmingw.dll")
msgbox % "loading dll" . ErrorLevel
result := DllCall(A_ScriptDir . "\ahkmingw.dll\ahkdll", "str", "send.ahk", "str"
, "", "str", "", "cdecl") 
return

q::ExitApp


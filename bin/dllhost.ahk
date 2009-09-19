start:
ahkdll := DllCall("LoadLibrary", "str", "ahkx.dll")
sleep, 500
msgbox % ErrorLevel
threadH := DllCall("ahkx.dll\ahkdll", "str", "dllclient.ahk", "str"
, "", "str", "parameter1 parameter2", "Cdecl Int") 
msgbox % ErrorLevel
return

!r::
Reload

!q::
ExitApp

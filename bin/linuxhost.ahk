start:
ahkdll := DllCall("LoadLibrary", "str", A_ScriptDir . "/MyWin.dll")
result := DllCall(A_ScriptDir . "/MyWin.dll/MyProxyWinFunc", "short") 
msgbox % ErrorLevel 
msgbox % result
return


#InstallKeybdHook
#UseHook
#Persistent
ahkdll := DllCall("LoadLibrary", "str", A_ScriptDir . "\xdotool.dll.so")
DllCall(A_ScriptDir . "\xdotool.dll.so\xinit", "cdecl") 
return

q::exitapp

::abc::
msgbox f4
return

8::
sendevent {F3}
return

F3::
msgbox shift8
return


a::
inputbox x
; sendahk(x)
return


b::
; sendahk("abc ")
return

i:: 
sendevent, a
DllCall(A_ScriptDir . "\xdotool.dll.so\xmousemove"
, "int", 0, "int", -20, "int", 1, "cdecl") 
return


k:: 
DllCall(A_ScriptDir . "\xdotool.dll.so\xmousemove"
, "int", 0, "int", 20, "int", 1, "cdecl") 
return

j:: 
DllCall(A_ScriptDir . "\xdotool.dll.so\xmousemove"
, "int", -20, "int", 0, "int", 1, "cdecl") 
return


l:: 
DllCall(A_ScriptDir . "\xdotool.dll.so\xmousemove"
, "int", 20, "int", 0, "int", 1, "cdecl") 
return

n::
controlsend, , hello, naveen
msgbox xsend complete
return
#ifwinexist

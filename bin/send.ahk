#InstallKeybdHook
#UseHook 
; SendMode Input

#IfWinActive Z
a::
msgbox test
return

#IfWinActive 

F4::
sendevent, a
return


F5::
send, a
return

F6::
sendplay, a
return




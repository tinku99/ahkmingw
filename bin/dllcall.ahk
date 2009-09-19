msgbox testing
WhichButton := DllCall("MessageBox", "int", "0", "str", "Press Yes or No", "str", "Title of box", "int", 4)
MsgBox You pressed button #%WhichButton%.

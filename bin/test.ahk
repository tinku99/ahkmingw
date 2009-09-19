fx()
return


F1::
msgbox hello
return

F2::
var = hello again
msgbox % var . "bla"
return

fx()
{
loop, 5
{
x += 3 + 3
FileAppend, %x%, *
WinMinimize
msgbox % x

}
}


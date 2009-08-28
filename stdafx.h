/*
AutoHotkey

Copyright 2003-2008 Chris Mallett (support@autohotkey.com)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#define _CRT_SECURE_NO_DEPRECATE // Avoid compiler warnings in VC++ 8.x/2005 that urge the use of lower-performing C library functions that protect against buffer overruns.
#define WIN32_LEAN_AND_MEAN		 // Exclude rarely-used stuff from Windows headers

// Windows Header Files:
// Necessary to do this prior to including windows.h so that NT functions are unlocked:
// UPDATE: Using 0x0500 now so that VK_XBUTTON1 and 2 can be supported:
// UPDATE v1.0.36.03: Using 0x0501 now so that various ListView constants and other things can be used.
// UPDATE v1.0.36.05: 0x0501 broke the Tooltip cmd on on Win9x/NT4/2000 by increasing the size of the TOOLINFO
// struct by 4 bytes.  However, rather than forever go without 0x501 and the many upgrades and constants
// it makes available in the code, it seems best to stick with it and instead patch anything that needs it
// (such as ToolTip).  Hopefully, ToolTip is the only thing in the current code base that needs patching
// (perhaps the only reason it was broken in the first place was a bug or oversight by MS).
#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0501  // Added for v1.0.35 to have MCS_NOTODAY resolve as expected, and possibly solve other problems on newer systems.

//#ifdef _MSC_VER
	// C RunTime Header Files
	#include <stdio.h>
	#include <stdlib.h>
	#include <stdarg.h> // used by snprintfcat()
	#include <limits.h> // for UINT_MAX, UCHAR_MAX, etc.
	#include <malloc.h> // For _alloca()
	//#include <memory.h>
	//#include <tchar.h>

	#include <windows.h>
	#include <commctrl.h> // for status bar functions. Must be included after <windows.h>.
	#include <shellapi.h>  // for ShellExecute()
	#include <shlobj.h>  // for SHGetMalloc()
	#include <mmsystem.h> // for mciSendString() and waveOutSetVolume()
	#include <commdlg.h> // for OPENFILENAME

	// It's probably best not to do these, because I think they would then be included
	// for everything, even modules that don't need it, which might result in undesired
	// dependencies sneaking in, or subtle naming conflicts:
	// ...
	//#include "defines.h"
	//#include "application.h"
	//#include "globaldata.h"
	//#include "window.h"  // Not to be confused with "windows.h"
	//#include "util.h"
	//#include "SimpleHeap.h"
//#endif

#include <ctype.h>
#include <olectl.h>

#define __forceinline
#define LLMHF_INJECTED 0x00000001
#define IDCONTINUE 11
#define IDTRYAGAIN 10
#define MIXERCONTROL_CONTROLTYPE_BASS_BOOST     (MIXERCONTROL_CONTROLTYPE_BOOLEAN + 0x00002277)
#define LPLVITEM LPLVITEMW
#define CF_DIBV5 17
#define TreeView_GetCheckState(hwndTV, hti) \
   ((((UINT)(SendMessageA((hwndTV), TVM_GETITEMSTATE, (WPARAM)(hti),  \
                     TVIS_STATEIMAGEMASK))) >> 12) -1)
#define SBARS_TOOLTIPS 0x0800
#define BS_TYPEMASK 0x0000000FL
#define LV_VIEW_TILE 0x0004
#define LVN_HOTTRACK (LVN_FIRST-21)
#define LVN_BEGINSCROLL (LVN_FIRST-80)
#define LVN_ENDSCROLL (LVN_FIRST-81)
#define LVN_MARQUEEBEGIN (LVN_FIRST-56)
#define MIM_BACKGROUND 2
#define MIM_APPLYTOSUBMENUS 0x80000000L

typedef struct tagTVKEYDOWN {
    NMHDR hdr;
    WORD wVKey;
    UINT flags;
} NMTVKEYDOWN, *LPNMTVKEYDOWN;


typedef DWORD *LPCOLORREF;

extern "C" __int64 _strtoi64(const char*, char**, int);
extern "C" __int64 _strtoui64(const char*, char**, int);

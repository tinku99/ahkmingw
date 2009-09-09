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

#include "stdafx.h" // pre-compiled headers
#include "script.h"
#include "globaldata.h" // for a lot of things
#include "util.h" // for strlcpy() etc.
#include "mt19937ar-cok.h" // for random number generator
#include "window.h" // for a lot of things
#include "application.h" // for MsgSleep()
#include "exports.h" // Naveen v8
// Globals that are for only this module:
#define MAX_COMMENT_FLAG_LENGTH 15
static char g_CommentFlag[MAX_COMMENT_FLAG_LENGTH + 1] = ";"; // Adjust the below for any changes.
static size_t g_CommentFlagLength = 1; // pre-calculated for performance

// General note about the methods in here:
// Want to be able to support multiple simultaneous points of execution
// because more than one subroutine can be executing simultaneously
// (well, more precisely, there can be more than one script subroutine
// that's in a "currently running" state, even though all such subroutines,
// except for the most recent one, are suspended.  So keep this in mind when
// using things such as static data members or static local variables.


Script::Script()
	: mFirstLine(NULL), mLastLine(NULL), mCurrLine(NULL), mPlaceholderLabel(NULL), mLineCount(0)
	, mThisHotkeyName(""), mPriorHotkeyName(""), mThisHotkeyStartTime(0), mPriorHotkeyStartTime(0)
	, mEndChar(0), mThisHotkeyModifiersLR(0)
	, mNextClipboardViewer(NULL), mOnClipboardChangeIsRunning(false), mOnClipboardChangeLabel(NULL)
	, mOnExitLabel(NULL), mExitReason(EXIT_NONE)
	, mFirstLabel(NULL), mLastLabel(NULL)
	, mFirstFunc(NULL), mLastFunc(NULL)
	, mFirstTimer(NULL), mLastTimer(NULL), mTimerEnabledCount(0), mTimerCount(0)
	, mFirstMenu(NULL), mLastMenu(NULL), mMenuCount(0)
	, mVar(NULL), mVarCount(0), mVarCountMax(0), mLazyVar(NULL), mLazyVarCount(0)
	, mOpenBlockCount(0), mNextLineIsFunctionBody(false)
	, mFuncExceptionVar(NULL), mFuncExceptionVarCount(0)
	, mCurrFileIndex(0), mCombinedLineNumber(0), mNoHotkeyLabels(true), mMenuUseErrorLevel(false)
	, mFileSpec(""), mFileDir(""), mFileName(""), mOurEXE(""), mOurEXEDir(""), mMainWindowTitle("")
	, mIsReadyToExecute(false), AutoExecSectionIsRunning(false)
	, mIsRestart(false), mIsAutoIt2(false), mErrorStdOut(false)
#ifdef AUTOHOTKEYSC
	, mCompiledHasCustomIcon(false)
#else
	, mIncludeLibraryFunctionsThenExit(NULL)
#endif
	, mLinesExecutedThisCycle(0), mUninterruptedLineCountMax(1000), mUninterruptibleTime(15)
	, mRunAsUser(NULL), mRunAsPass(NULL), mRunAsDomain(NULL)
	, mCustomIcon(NULL) // Normally NULL unless there's a custom tray icon loaded dynamically.
	, mCustomIconFile(NULL), mIconFrozen(false), mTrayIconTip(NULL) // Allocated on first use.
	, mCustomIconNumber(0)
{
	// v1.0.25: mLastScriptRest and mLastPeekTime are now initialized right before the auto-exec
	// section of the script is launched, which avoids an initial Sleep(10) in ExecUntil
	// that would otherwise occur.
	*mThisMenuItemName = *mThisMenuName = '\0';
	ZeroMemory(&mNIC, sizeof(mNIC));  // Constructor initializes this, to be safe.
	mNIC.hWnd = NULL;  // Set this as an indicator that it tray icon is not installed.

	// Lastly (after the above have been initialized), anything that can fail:
	if (   !(mTrayMenu = AddMenu("Tray"))   ) // realistically never happens
	{
		ScriptError("No tray mem");
		ExitApp(EXIT_CRITICAL);
	}
	else
		mTrayMenu->mIncludeStandardItems = true;

#ifdef _DEBUG
	if (ID_FILE_EXIT < ID_MAIN_FIRST) // Not a very thorough check.
		ScriptError("DEBUG: ID_FILE_EXIT is too large (conflicts with IDs reserved via ID_USER_FIRST).");
	if (MAX_CONTROLS_PER_GUI > ID_USER_FIRST - 3)
		ScriptError("DEBUG: MAX_CONTROLS_PER_GUI is too large (conflicts with IDs reserved via ID_USER_FIRST).");
	int LargestMaxParams, i, j;
	ActionTypeType *np;
	// Find the Largest value of MaxParams used by any command and make sure it
	// isn't something larger than expected by the parsing routines:
	for (LargestMaxParams = i = 0; i < g_ActionCount; ++i)
	{
		if (g_act[i].MaxParams > LargestMaxParams)
			LargestMaxParams = g_act[i].MaxParams;
		// This next part has been tested and it does work, but only if one of the arrays
		// contains exactly MAX_NUMERIC_PARAMS number of elements and isn't zero terminated.
		// Relies on short-circuit boolean order:
		for (np = g_act[i].NumericParams, j = 0; j < MAX_NUMERIC_PARAMS && *np; ++j, ++np);
		if (j >= MAX_NUMERIC_PARAMS)
		{
			ScriptError("DEBUG: At least one command has a NumericParams array that isn't zero-terminated."
				"  This would result in reading beyond the bounds of the array.");
			return;
		}
	}
	if (LargestMaxParams > MAX_ARGS)
		ScriptError("DEBUG: At least one command supports more arguments than allowed.");
	if (sizeof(ActionTypeType) == 1 && g_ActionCount > 256)
		ScriptError("DEBUG: Since there are now more than 256 Action Types, the ActionTypeType"
			" typedef must be changed.");
#endif
}



Script::~Script() // Destructor.
{
	// MSDN: "Before terminating, an application must call the UnhookWindowsHookEx function to free
	// system resources associated with the hook."
	AddRemoveHooks(0); // Remove all hooks.
	if (mNIC.hWnd) // Tray icon is installed.
		Shell_NotifyIcon(NIM_DELETE, &mNIC); // Remove it.
	// Destroy any Progress/SplashImage windows that haven't already been destroyed.  This is necessary
	// because sometimes these windows aren't owned by the main window:
	int i;
	for (i = 0; i < MAX_PROGRESS_WINDOWS; ++i)
	{
		if (g_Progress[i].hwnd && IsWindow(g_Progress[i].hwnd))
			DestroyWindow(g_Progress[i].hwnd);
		if (g_Progress[i].hfont1) // Destroy font only after destroying the window that uses it.
			DeleteObject(g_Progress[i].hfont1);
		if (g_Progress[i].hfont2) // Destroy font only after destroying the window that uses it.
			DeleteObject(g_Progress[i].hfont2);
		if (g_Progress[i].hbrush)
			DeleteObject(g_Progress[i].hbrush);
	}
	for (i = 0; i < MAX_SPLASHIMAGE_WINDOWS; ++i)
	{
		if (g_SplashImage[i].pic)
			g_SplashImage[i].pic->Release();
		if (g_SplashImage[i].hwnd && IsWindow(g_SplashImage[i].hwnd))
			DestroyWindow(g_SplashImage[i].hwnd);
		if (g_SplashImage[i].hfont1) // Destroy font only after destroying the window that uses it.
			DeleteObject(g_SplashImage[i].hfont1);
		if (g_SplashImage[i].hfont2) // Destroy font only after destroying the window that uses it.
			DeleteObject(g_SplashImage[i].hfont2);
		if (g_SplashImage[i].hbrush)
			DeleteObject(g_SplashImage[i].hbrush);
	}

	// It is safer/easier to destroy the GUI windows prior to the menus (especially the menu bars).
	// This is because one GUI window might get destroyed and take with it a menu bar that is still
	// in use by an existing GUI window.  GuiType::Destroy() adheres to this philosophy by detaching
	// its menu bar prior to destroying its window:
	for (i = 0; i < MAX_GUI_WINDOWS; ++i)
		GuiType::Destroy(i); // Static method to avoid problems with object destroying itself.
	for (i = 0; i < GuiType::sFontCount; ++i) // Now that GUI windows are gone, delete all GUI fonts.
		if (GuiType::sFont[i].hfont)
			DeleteObject(GuiType::sFont[i].hfont);
	// The above might attempt to delete an HFONT from GetStockObject(DEFAULT_GUI_FONT), etc.
	// But that should be harmless:
	// MSDN: "It is not necessary (but it is not harmful) to delete stock objects by calling DeleteObject."

	// Above: Probably best to have removed icon from tray and destroyed any Gui/Splash windows that were
	// using it prior to getting rid of the script's custom icon below:
	if (mCustomIcon)
		DestroyIcon(mCustomIcon);

	// Since they're not associated with a window, we must free the resources for all popup menus.
	// Update: Even if a menu is being used as a GUI window's menu bar, see note above for why menu
	// destruction is done AFTER the GUI windows are destroyed:
	UserMenu *menu_to_delete;
	for (UserMenu *m = mFirstMenu; m;)
	{
		menu_to_delete = m;
		m = m->mNextMenu;
		ScriptDeleteMenu(menu_to_delete);
		// Above call should not return FAIL, since the only way FAIL can realistically happen is
		// when a GUI window is still using the menu as its menu bar.  But all GUI windows are gone now.
	}

	// Since tooltip windows are unowned, they should be destroyed to avoid resource leak:
	for (i = 0; i < MAX_TOOLTIPS; ++i)
		if (g_hWndToolTip[i] && IsWindow(g_hWndToolTip[i]))
			DestroyWindow(g_hWndToolTip[i]);

	if (g_hFontSplash) // The splash window itself should auto-destroyed, since it's owned by main.
		DeleteObject(g_hFontSplash);

	if (mOnClipboardChangeLabel) // Remove from viewer chain.
		ChangeClipboardChain(g_hWnd, mNextClipboardViewer);

	// Close any open sound item to prevent hang-on-exit in certain operating systems or conditions.
	// If there's any chance that a sound was played and not closed out, or that it is still playing,
	// this check is done.  Otherwise, the check is avoided since it might be a high overhead call,
	// especially if the sound subsystem part of the OS is currently swapped out or something:
	if (g_SoundWasPlayed)
	{
		char buf[MAX_PATH * 2];
		mciSendString("status " SOUNDPLAY_ALIAS " mode", buf, sizeof(buf), NULL);
		if (*buf) // "playing" or "stopped"
			mciSendString("close " SOUNDPLAY_ALIAS, NULL, 0, NULL);
	}

#ifdef ENABLE_KEY_HISTORY_FILE
	KeyHistoryToFile();  // Close the KeyHistory file if it's open.
#endif

	DeleteCriticalSection(&g_CriticalRegExCache); // g_CriticalRegExCache is used elsewhere for thread-safety.
}



ResultType Script::Init(char *aScriptFilename, bool aIsRestart)
// Returns OK or FAIL.
// Caller has provided an empty string for aScriptFilename if this is a compiled script.
// Otherwise, aScriptFilename can be NULL if caller hasn't determined the filename of the script yet.
{
	mIsRestart = aIsRestart;
	char buf[2048]; // Just to make sure we have plenty of room to do things with.
#ifdef AUTOHOTKEYSC
	// Fix for v1.0.29: Override the caller's use of __argv[0] by using GetModuleFileName(),
	// so that when the script is started from the command line but the user didn't type the
	// extension, the extension will be included.  This necessary because otherwise
	// #SingleInstance wouldn't be able to detect duplicate versions in every case.
	// It also provides more consistency.
	GetModuleFileName(NULL, buf, sizeof(buf));
#else
	if (!aScriptFilename) // v1.0.46.08: Change in policy: store the default script in the My Documents directory rather than in Program Files.  It's more correct and solves issues that occur due to Vista's file-protection scheme.
	{
		// Since no script-file was specified on the command line, use the default name.
		// For backward compatibility, FIRST check if there's an AutoHotkey.ini file in the current
		// directory.  If there is, that needs to be used to retain compatibility.
		aScriptFilename = NAME_P ".ini";
		if (GetFileAttributes(aScriptFilename) == 0xFFFFFFFF) // File doesn't exist, so fall back to new method.
		{
			aScriptFilename = buf;
			VarSizeType filespec_length = BIV_MyDocuments(aScriptFilename, ""); // e.g. C:\Documents and Settings\Home\My Documents
			if (filespec_length	> sizeof(buf)-16) // Need room for 16 characters ('\\' + "AutoHotkey.ahk" + terminator).
				return FAIL; // Very rare, so for simplicity just abort.
			strcpy(aScriptFilename + filespec_length, "\\AutoHotkey.ahk"); // Append the filename: .ahk vs. .ini seems slightly better in terms of clarity and usefulness (e.g. the ability to double click the default script to launch it).
			// Now everything is set up right because even if aScriptFilename is a nonexistent file, the
			// user will be prompted to create it by a stage further below.
		}
		//else since the legacy .ini file exists, everything is now set up right. (The file might be a directory, but that isn't checked due to rarity.)
	}
	// In case the script is a relative filespec (relative to current working dir):
	char *unused;
	if (!GetFullPathName(aScriptFilename, sizeof(buf), buf, &unused)) // This is also relied upon by mIncludeLibraryFunctionsThenExit.  Succeeds even on nonexistent files.
		return FAIL; // Due to rarity, no error msg, just abort.
#endif
	// Using the correct case not only makes it look better in title bar & tray tool tip,
	// it also helps with the detection of "this script already running" since otherwise
	// it might not find the dupe if the same script name is launched with different
	// lowercase/uppercase letters:
	ConvertFilespecToCorrectCase(buf); // This might change the length, e.g. due to expansion of 8.3 filename.
	char *filename_marker;
	if (   !(filename_marker = strrchr(buf, '\\'))   )
		filename_marker = buf;
	else
		++filename_marker;
	if (   !(mFileSpec = SimpleHeap::Malloc(buf))   )  // The full spec is stored for convenience, and it's relied upon by mIncludeLibraryFunctionsThenExit.
		return FAIL;  // It already displayed the error for us.
	filename_marker[-1] = '\0'; // Terminate buf in this position to divide the string.
	size_t filename_length = strlen(filename_marker);
	if (   mIsAutoIt2 = (filename_length >= 4 && !stricmp(filename_marker + filename_length - 4, EXT_AUTOIT2))   )
	{
		// Set the old/AutoIt2 defaults for maximum safety and compatibilility.
		// Standalone EXEs (compiled scripts) are always considered to be non-AutoIt2 (otherwise,
		// the user should probably be using the AutoIt2 compiler).
		g_AllowSameLineComments = false;
		g_EscapeChar = '\\';
		g.TitleFindFast = true; // In case the normal default is false.
		g.DetectHiddenText = false;
		// Make the mouse fast like AutoIt2, but not quite insta-move.  2 is expected to be more
		// reliable than 1 since the AutoIt author said that values less than 2 might cause the
		// drag to fail (perhaps just for specific apps, such as games):
		g.DefaultMouseSpeed = 2;
		g.KeyDelay = 20;
		g.WinDelay = 500;
		g.LinesPerCycle = 1;
		g.IntervalBeforeRest = -1;  // i.e. this method is disabled by default for AutoIt2 scripts.
		// Reduce max params so that any non escaped delimiters the user may be using literally
		// in "window text" will still be considered literal, rather than as delimiters for
		// args that are not supported by AutoIt2, such as exclude-title, exclude-text, MsgBox
		// timeout, etc.  Note: Don't need to change IfWinExist and such because those already
		// have special handling to recognize whether exclude-title is really a valid command
		// instead (e.g. IfWinExist, title, text, Gosub, something).

		// NOTE: DO NOT ADD the IfWin command series to this section, since there is special handling
		// for parsing those commands to figure out whether they're being used in the old AutoIt2
		// style or the new Exclude Title/Text mode.

		// v1.0.40.02: The following is no longer done because a different mechanism is required now
		// that the ARGn macros do not check whether mArgc is too small and substitute an empty string
		// (instead, there is a loop in ExpandArgs that puts an empty string in each sArgDeref entry
		// for which the script omitted a parameter [and that loop relies on MaxParams being absolutely
		// accurate rather than conditional upon whether the script is of type ".aut"]).
		//g_act[ACT_FILESELECTFILE].MaxParams -= 2;
		//g_act[ACT_FILEREMOVEDIR].MaxParams -= 1;
		//g_act[ACT_MSGBOX].MaxParams -= 1;
		//g_act[ACT_INIREAD].MaxParams -= 1;
		//g_act[ACT_STRINGREPLACE].MaxParams -= 1;
		//g_act[ACT_STRINGGETPOS].MaxParams -= 2;
		//g_act[ACT_WINCLOSE].MaxParams -= 3;  // -3 for these two, -2 for the others.
		//g_act[ACT_WINKILL].MaxParams -= 3;
		//g_act[ACT_WINACTIVATE].MaxParams -= 2;
		//g_act[ACT_WINMINIMIZE].MaxParams -= 2;
		//g_act[ACT_WINMAXIMIZE].MaxParams -= 2;
		//g_act[ACT_WINRESTORE].MaxParams -= 2;
		//g_act[ACT_WINHIDE].MaxParams -= 2;
		//g_act[ACT_WINSHOW].MaxParams -= 2;
		//g_act[ACT_WINSETTITLE].MaxParams -= 2;
		//g_act[ACT_WINGETTITLE].MaxParams -= 2;
	}
	if (   !(mFileDir = SimpleHeap::Malloc(buf))   )
		return FAIL;  // It already displayed the error for us.
	if (   !(mFileName = SimpleHeap::Malloc(filename_marker))   )
		return FAIL;  // It already displayed the error for us.
#ifdef AUTOHOTKEYSC
	// Omit AutoHotkey from the window title, like AutoIt3 does for its compiled scripts.
	// One reason for this is to reduce backlash if evil-doers create viruses and such
	// with the program:
	snprintf(buf, sizeof(buf), "%s\\%s", mFileDir, mFileName);
#else
	snprintf(buf, sizeof(buf), "%s\\%s - %s", mFileDir, mFileName, NAME_PV);
#endif
	if (   !(mMainWindowTitle = SimpleHeap::Malloc(buf))   )
		return FAIL;  // It already displayed the error for us.

	// It may be better to get the module name this way rather than reading it from the registry
	// (though it might be more proper to parse it out of the command line args or something),
	// in case the user has moved it to a folder other than the install folder, hasn't installed it,
	// or has renamed the EXE file itself.  Also, enclose the full filespec of the module in double
	// quotes since that's how callers usually want it because ActionExec() currently needs it that way:
	*buf = '"';
	if (GetModuleFileName(NULL, buf + 1, sizeof(buf) - 2)) // -2 to leave room for the enclosing double quotes.
	{
		size_t buf_length = strlen(buf);
		buf[buf_length++] = '"';
		buf[buf_length] = '\0';
		if (   !(mOurEXE = SimpleHeap::Malloc(buf))   )
			return FAIL;  // It already displayed the error for us.
		else
		{
			char *last_backslash = strrchr(buf, '\\');
			if (!last_backslash) // probably can't happen due to the nature of GetModuleFileName().
				mOurEXEDir = "";
			last_backslash[1] = '\0'; // i.e. keep the trailing backslash for convenience.
			if (   !(mOurEXEDir = SimpleHeap::Malloc(buf + 1))   ) // +1 to omit the leading double-quote.
				return FAIL;  // It already displayed the error for us.
		}
	}
	return OK;
}



ResultType Script::CreateWindows()
// Returns OK or FAIL.
{
	if (!mMainWindowTitle || !*mMainWindowTitle) return FAIL;  // Init() must be called before this function.
	// Register a window class for the main window:
	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpszClassName = WINDOW_CLASS_MAIN;
	wc.hInstance = g_hInstance;
	wc.lpfnWndProc = MainWindowProc;
	// The following are left at the default of NULL/0 set higher above:
	//wc.style = 0;  // CS_HREDRAW | CS_VREDRAW
	//wc.cbClsExtra = 0;
	//wc.cbWndExtra = 0;
	wc.hIcon = wc.hIconSm = (HICON)LoadImage(g_hInstance, MAKEINTRESOURCE(IDI_MAIN), IMAGE_ICON, 0, 0, LR_SHARED); // Use LR_SHARED to conserve memory (since the main icon is loaded for so many purposes).
	wc.hCursor = LoadCursor((HINSTANCE) NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);  // Needed for ProgressBar. Old: (HBRUSH)GetStockObject(WHITE_BRUSH);
	wc.lpszMenuName = MAKEINTRESOURCE(IDR_MENU_MAIN); // NULL; // "MainMenu";
	if (!RegisterClassEx(&wc))
	{
		MsgBox("RegClass"); // Short/generic msg since so rare.
		return FAIL;
	}

	// Register a second class for the splash window.  The only difference is that
	// it doesn't have the menu bar:
	wc.lpszClassName = WINDOW_CLASS_SPLASH;
	wc.lpszMenuName = NULL; // Override the non-NULL value set higher above.
	if (!RegisterClassEx(&wc))
	{
		MsgBox("RegClass"); // Short/generic msg since so rare.
		return FAIL;
	}

	char class_name[64];
	HWND fore_win = GetForegroundWindow();
	bool do_minimize = !fore_win || (GetClassName(fore_win, class_name, sizeof(class_name))
		&& !stricmp(class_name, "Shell_TrayWnd")); // Shell_TrayWnd is the taskbar's class on Win98/XP and probably the others too.

	// Note: the title below must be constructed the same was as is done by our
	// WinMain() (so that we can detect whether this script is already running)
	// which is why it's standardized in g_script.mMainWindowTitle.
	// Create the main window.  Prevent momentary disruption of Start Menu, which
	// some users understandably don't like, by omitting the taskbar button temporarily.
	// This is done because testing shows that minimizing the window further below, even
	// though the window is hidden, would otherwise briefly show the taskbar button (or
	// at least redraw the taskbar).  Sometimes this isn't noticeable, but other times
	// (such as when the system is under heavy load) a user reported that it is quite
	// noticeable. WS_EX_TOOLWINDOW is used instead of WS_EX_NOACTIVATE because
	// WS_EX_NOACTIVATE is available only on 2000/XP.
	if (   !(g_hWnd = CreateWindowEx(do_minimize ? WS_EX_TOOLWINDOW : 0
		, WINDOW_CLASS_MAIN
		, mMainWindowTitle
		, WS_OVERLAPPEDWINDOW // Style.  Alt: WS_POPUP or maybe 0.
		, CW_USEDEFAULT // xpos
		, CW_USEDEFAULT // ypos
		, CW_USEDEFAULT // width
		, CW_USEDEFAULT // height
		, NULL // parent window
		, NULL // Identifies a menu, or specifies a child-window identifier depending on the window style
		, g_hInstance // passed into WinMain
		, NULL))   ) // lpParam
	{
		MsgBox("CreateWindow"); // Short msg since so rare.
		return FAIL;
	}
#ifdef AUTOHOTKEYSC
	HMENU menu = GetMenu(g_hWnd);
	// Disable the Edit menu item, since it does nothing for a compiled script:
	EnableMenuItem(menu, ID_FILE_EDITSCRIPT, MF_DISABLED | MF_GRAYED);
	EnableOrDisableViewMenuItems(menu, MF_DISABLED | MF_GRAYED); // Fix for v1.0.47.06: No point in checking g_AllowMainWindow because the script hasn't starting running yet, so it will always be false.
	// But leave the ID_VIEW_REFRESH menu item enabled because if the script contains a
	// command such as ListLines in it, Refresh can be validly used.
#endif

	if (    !(g_hWndEdit = CreateWindow("edit", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER
		| ES_LEFT | ES_MULTILINE | ES_READONLY | WS_VSCROLL // | WS_HSCROLL (saves space)
		, 0, 0, 0, 0, g_hWnd, (HMENU)1, g_hInstance, NULL))   )
	{
		MsgBox("CreateWindow"); // Short msg since so rare.
		return FAIL;
	}
	// FONTS: The font used by default, at least on XP, is GetStockObject(SYSTEM_FONT).
	// It seems preferable to smaller fonts such DEFAULT_GUI_FONT(DEFAULT_GUI_FONT).
	// For more info on pre-loaded fonts (not too many choices), see MSDN's GetStockObject().
	//SendMessage(g_hWndEdit, WM_SETFONT, (WPARAM)GetStockObject(SYSTEM_FONT), 0);

	// v1.0.30.05: Specifying a limit of zero opens the control to its maximum text capacity,
	// which removes the 32K size restriction.  Testing shows that this does not increase the actual
	// amount of memory used for controls containing small amounts of text.  All it does is allow
	// the control to allocate more memory as needed.  By specifying zero, a max
	// of 64K becomes available on Windows 9x, and perhaps as much as 4 GB on NT/2k/XP.
	SendMessage(g_hWndEdit, EM_LIMITTEXT, 0, 0);

	// Some of the MSDN docs mention that an app's very first call to ShowWindow() makes that
	// function operate in a special mode. Therefore, it seems best to get that first call out
	// of the way to avoid the possibility that the first-call behavior will cause problems with
	// our normal use of ShowWindow() below and other places.  Also, decided to ignore nCmdShow,
    // to avoid any momentary visual effects on startup.
	// Update: It's done a second time because the main window might now be visible if the process
	// that launched ours specified that.  It seems best to override the requested state because
	// some calling processes might specify "maximize" or "shownormal" as generic launch method.
	// The script can display it's own main window with ListLines, etc.
	// MSDN: "the nCmdShow value is ignored in the first call to ShowWindow if the program that
	// launched the application specifies startup information in the structure. In this case,
	// ShowWindow uses the information specified in the STARTUPINFO structure to show the window.
	// On subsequent calls, the application must call ShowWindow with nCmdShow set to SW_SHOWDEFAULT
	// to use the startup information provided by the program that launched the application."
	ShowWindow(g_hWnd, SW_HIDE);
	ShowWindow(g_hWnd, SW_HIDE);

	// Now that the first call to ShowWindow() is out of the way, minimize the main window so that
	// if the script is launched from the Start Menu (and perhaps other places such as the
	// Quick-launch toolbar), the window that was active before the Start Menu was displayed will
	// become active again.  But as of v1.0.25.09, this minimize is done more selectively to prevent
	// the launch of a script from knocking the user out of a full-screen game or other application
	// that would be disrupted by an SW_MINIMIZE:
	if (do_minimize)
	{
		ShowWindow(g_hWnd, SW_MINIMIZE);
		SetWindowLong(g_hWnd, GWL_EXSTYLE, 0); // Give the main window back its taskbar button.
	}
	// Note: When the window is not minimized, task manager reports that a simple script (such as
	// one consisting only of the single line "#Persistent") uses 2600 KB of memory vs. ~452 KB if
	// it were immediately minimized.  That is probably just due to the vagaries of how the OS
	// manages windows and memory and probably doesn't actually impact system performance to the
	// degree indicated.  In other words, it's hard to imagine that the failure to do
	// ShowWidnow(g_hWnd, SW_MINIMIZE) unconditionally upon startup (which causes the side effects
	// discussed further above) significantly increases the actual memory load on the system.

	g_hAccelTable = LoadAccelerators(g_hInstance, MAKEINTRESOURCE(IDR_ACCELERATOR1));

	if (g_NoTrayIcon)
		mNIC.hWnd = NULL;  // Set this as an indicator that tray icon is not installed.
	else
		// Even if the below fails, don't return FAIL in case the user is using a different shell
		// or something.  In other words, it is expected to fail under certain circumstances and
		// we want to tolerate that:
		CreateTrayIcon();

	if (mOnClipboardChangeLabel)
		mNextClipboardViewer = SetClipboardViewer(g_hWnd);

	return OK;
}



void Script::EnableOrDisableViewMenuItems(HMENU aMenu, UINT aFlags)
{
	EnableMenuItem(aMenu, ID_VIEW_KEYHISTORY, aFlags);
	EnableMenuItem(aMenu, ID_VIEW_LINES, aFlags);
	EnableMenuItem(aMenu, ID_VIEW_VARIABLES, aFlags);
	EnableMenuItem(aMenu, ID_VIEW_HOTKEYS, aFlags);
}



void Script::CreateTrayIcon()
// It is the caller's responsibility to ensure that the previous icon is first freed/destroyed
// before calling us to install a new one.  However, that is probably not needed if the Explorer
// crashed, since the memory used by the tray icon was probably destroyed along with it.
{
	ZeroMemory(&mNIC, sizeof(mNIC));  // To be safe.
	// Using NOTIFYICONDATA_V2_SIZE vs. sizeof(NOTIFYICONDATA) improves compatibility with Win9x maybe.
	// MSDN: "Using [NOTIFYICONDATA_V2_SIZE] for cbSize will allow your application to use NOTIFYICONDATA
	// with earlier Shell32.dll versions, although without the version 6.0 enhancements."
	// Update: Using V2 gives an compile error so trying V1.  Update: Trying sizeof(NOTIFYICONDATA)
	// for compatibility with VC++ 6.x.  This is also what AutoIt3 uses:
	mNIC.cbSize = sizeof(NOTIFYICONDATA);  // NOTIFYICONDATA_V1_SIZE
	mNIC.hWnd = g_hWnd;
	mNIC.uID = AHK_NOTIFYICON; // This is also used for the ID, see TRANSLATE_AHK_MSG for details.
	mNIC.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
	mNIC.uCallbackMessage = AHK_NOTIFYICON;
#ifdef AUTOHOTKEYSC
	// i.e. don't override the user's custom icon:
	mNIC.hIcon = mCustomIcon ? mCustomIcon : (HICON)LoadImage(g_hInstance, MAKEINTRESOURCE(mCompiledHasCustomIcon ? IDI_MAIN : g_IconTray), IMAGE_ICON, 0, 0, LR_SHARED);
#else
	mNIC.hIcon = mCustomIcon ? mCustomIcon : (HICON)LoadImage(g_hInstance, MAKEINTRESOURCE(g_IconTray), IMAGE_ICON, 0, 0, LR_SHARED); // Use LR_SHARED to conserve memory (since the main icon is loaded for so many purposes).
#endif
	UPDATE_TIP_FIELD
	// If we were called due to an Explorer crash, I don't think it's necessary to call
	// Shell_NotifyIcon() to remove the old tray icon because it was likely destroyed
	// along with Explorer.  So just add it unconditionally:
	if (!Shell_NotifyIcon(NIM_ADD, &mNIC))
		mNIC.hWnd = NULL;  // Set this as an indicator that tray icon is not installed.
}



void Script::UpdateTrayIcon(bool aForceUpdate)
{
	if (!mNIC.hWnd) // tray icon is not installed
		return;
	static bool icon_shows_paused = false;
	static bool icon_shows_suspended = false;
	if (!aForceUpdate && (mIconFrozen || (g.IsPaused == icon_shows_paused && g_IsSuspended == icon_shows_suspended)))
		return; // it's already in the right state
	int icon;
	if (g.IsPaused && g_IsSuspended)
		icon = IDI_PAUSE_SUSPEND;
	else if (g.IsPaused)
		icon = IDI_PAUSE;
	else if (g_IsSuspended)
		icon = g_IconTraySuspend;
	else
#ifdef AUTOHOTKEYSC
		icon = mCompiledHasCustomIcon ? IDI_MAIN : g_IconTray;  // i.e. don't override the user's custom icon.
#else
		icon = g_IconTray;
#endif
	// Use the custom tray icon if the icon is normal (non-paused & non-suspended):
	mNIC.hIcon = (mCustomIcon && (mIconFrozen || (!g.IsPaused && !g_IsSuspended))) ? mCustomIcon
		: (HICON)LoadImage(g_hInstance, MAKEINTRESOURCE(icon), IMAGE_ICON, 0, 0, LR_SHARED); // Use LR_SHARED for simplicity and performance more than to conserve memory in this case.
	if (Shell_NotifyIcon(NIM_MODIFY, &mNIC))
	{
		icon_shows_paused = g.IsPaused;
		icon_shows_suspended = g_IsSuspended;
	}
	// else do nothing, just leave it in the same state.
}



ResultType Script::AutoExecSection()
{
	if (!mIsReadyToExecute)
		return FAIL;
	if (mFirstLine != NULL)
	{
		// Choose a timeout that's a reasonable compromise between the following competing priorities:
		// 1) That we want hotkeys to be responsive as soon as possible after the program launches
		//    in case the user launches by pressing ENTER on a script, for example, and then immediately
		//    tries to use a hotkey.  In addition, we want any timed subroutines to start running ASAP
		//    because in rare cases the user might rely upon that happening.
		// 2) To support the case when the auto-execute section never finishes (such as when it contains
		//    an infinite loop to do background processing), yet we still want to allow the script
		//    to put custom defaults into effect globally (for things such as KeyDelay).
		// Obviously, the above approach has its flaws; there are ways to construct a script that would
		// result in unexpected behavior.  However, the combination of this approach with the fact that
		// the global defaults are updated *again* when/if the auto-execute section finally completes
		// raises the expectation of proper behavior to a very high level.  In any case, I'm not sure there
		// is any better approach that wouldn't break existing scripts or require a redesign of some kind.
		// If this method proves unreliable due to disk activity slowing the program down to a crawl during
		// the critical milliseconds after launch, one thing that might fix that is to have ExecUntil()
		// be forced to run a minimum of, say, 100 lines (if there are that many) before allowing the
		// timer expiration to have its effect.  But that's getting complicated and I'd rather not do it
		// unless someone actually reports that such a thing ever happens.  Still, to reduce the chance
		// of such a thing ever happening, it seems best to boost the timeout from 50 up to 100:
		SET_AUTOEXEC_TIMER(100);
		AutoExecSectionIsRunning = true;

		// v1.0.25: This is now done here, closer to the actual execution of the first line in the script,
		// to avoid an unnecessary Sleep(10) that would otherwise occur in ExecUntil:
		mLastScriptRest = mLastPeekTime = GetTickCount();

		++g_nThreads;
		ResultType result = mFirstLine->ExecUntil(UNTIL_RETURN);
		--g_nThreads;

		KILL_AUTOEXEC_TIMER  // This also does "g.AllowThreadToBeInterrupted = true"
		AutoExecSectionIsRunning = false;

		return result;
	}
	return OK;
}



ResultType Script::Edit()
{
#ifdef AUTOHOTKEYSC
	return OK; // Do nothing.
#else
	// This is here in case a compiled script ever uses the Edit command.  Since the "Edit This
	// Script" menu item is not available for compiled scripts, it can't be called from there.
	TitleMatchModes old_mode = g.TitleMatchMode;
	g.TitleMatchMode = FIND_ANYWHERE;
	HWND hwnd = WinExist(g, mFileName, "", mMainWindowTitle, ""); // Exclude our own main window.
	g.TitleMatchMode = old_mode;
	if (hwnd)
	{
		char class_name[32];
		GetClassName(hwnd, class_name, sizeof(class_name));
		if (!strcmp(class_name, "#32770") || !strnicmp(class_name, "AutoHotkey", 10)) // MessageBox(), InputBox(), FileSelectFile(), or GUI/script-owned window.
			hwnd = NULL;  // Exclude it from consideration.
	}
	if (hwnd)  // File appears to already be open for editing, so use the current window.
		SetForegroundWindowEx(hwnd);
	else
	{
		char buf[MAX_PATH * 2];
		// Enclose in double quotes anything that might contain spaces since the CreateProcess()
		// method, which is attempted first, is more likely to succeed.  This is because it uses
		// the command line method of creating the process, with everything all lumped together:
		snprintf(buf, sizeof(buf), "\"%s\"", mFileSpec);
		if (!ActionExec("edit", buf, mFileDir, false))  // Since this didn't work, try notepad.
		{
			// v1.0.40.06: Try to open .ini files first with their associated editor rather than trying the
			// "edit" verb on them:
			char *file_ext;
			if (   !(file_ext = strrchr(mFileName, '.')) || stricmp(file_ext, ".ini")
				|| !ActionExec("open", buf, mFileDir, false)   ) // Relies on short-circuit boolean order.
			{
				// Even though notepad properly handles filenames with spaces in them under WinXP,
				// even without double quotes around them, it seems safer and more correct to always
				// enclose the filename in double quotes for maximum compatibility with all OSes:
				if (!ActionExec("notepad.exe", buf, mFileDir, false))
					MsgBox("Could not open script."); // Short message since so rare.
			}
		}
	}
	return OK;
#endif
}



ResultType Script::Reload(bool aDisplayErrors)
{
	// The new instance we're about to start will tell our process to stop, or it will display
	// a syntax error or some other error, in which case our process will still be running:
#ifdef AUTOHOTKEYSC
	// This is here in case a compiled script ever uses the Reload command.  Since the "Reload This
	// Script" menu item is not available for compiled scripts, it can't be called from there.
	return g_script.ActionExec(mOurEXE, "/restart", g_WorkingDirOrig, aDisplayErrors);
#else
	char arg_string[MAX_PATH + 512];
	snprintf(arg_string, sizeof(arg_string), "/restart \"%s\"", mFileSpec);
	return g_script.ActionExec(mOurEXE, arg_string, g_WorkingDirOrig, aDisplayErrors);
#endif
}



ResultType Script::ExitApp(ExitReasons aExitReason, char *aBuf, int aExitCode)
// Normal exit (if aBuf is NULL), or a way to exit immediately on error (which is mostly
// for times when it would be unsafe to call MsgBox() due to the possibility that it would
// make the situation even worse).
{
	mExitReason = aExitReason;
	bool terminate_afterward = aBuf && !*aBuf;
	if (aBuf && *aBuf)
	{
		char buf[1024];
		// No more than size-1 chars will be written and string will be terminated:
		snprintf(buf, sizeof(buf), "Critical Error: %s\n\n" WILL_EXIT, aBuf);
		// To avoid chance of more errors, don't use MsgBox():
		MessageBox(g_hWnd, buf, g_script.mFileSpec, MB_OK | MB_SETFOREGROUND | MB_APPLMODAL);
		TerminateApp(CRITICAL_ERROR); // Only after the above.
	}

	// Otherwise, it's not a critical error.  Note that currently, mOnExitLabel can only be
	// non-NULL if the script is in a runnable state (since registering an OnExit label requires
	// that a script command has executed to do it).  If this ever changes, the !mIsReadyToExecute
	// condition should be added to the below if statement:
	static bool sExitLabelIsRunning = false;
	if (!mOnExitLabel || sExitLabelIsRunning)  // || !mIsReadyToExecute
		// In the case of sExitLabelIsRunning == true:
		// There is another instance of this function beneath us on the stack.  Since we have
		// been called, this is a true exit condition and we exit immediately:
		TerminateApp(aExitCode);

	// Otherwise, the script contains the special RunOnExit label that we will run here instead
	// of exiting.  And since it does, we know that the script is in a ready-to-execute state
	// because that is the only way an OnExit label could have been defined in the first place.
	// Usually, the RunOnExit subroutine will contain an Exit or ExitApp statement
	// which results in a recursive call to this function, but this is not required (e.g. the
	// Exit subroutine could display an "Are you sure?" prompt, and if the user chooses "No",
	// the Exit sequence can be aborted by simply not calling ExitApp and letting the thread
	// we create below end normally).

	// Next, save the current state of the globals so that they can be restored just prior
	// to returning to our caller:
	char ErrorLevel_saved[ERRORLEVEL_SAVED_SIZE];
	strlcpy(ErrorLevel_saved, g_ErrorLevel->Contents(), sizeof(ErrorLevel_saved)); // Save caller's errorlevel.
	global_struct global_saved;
	CopyMemory(&global_saved, &g, sizeof(global_struct));
	InitNewThread(0, true, true, ACT_INVALID); // Since this special thread should always run, no checking of g_MaxThreadsTotal is done before calling this.

	if (g_nFileDialogs) // See MsgSleep() for comments on this.
		SetCurrentDirectory(g_WorkingDir);

	// Use g.AllowThreadToBeInterrupted to forbid any hotkeys, timers, or user defined menu items
	// to interrupt.  This is mainly done for peace-of-mind (since possible interactions due to
	// interruptions have not been studied) and the fact that this most users would not want this
	// subroutine to be interruptible (it usually runs quickly anyway).  Another reason to make
	// it non-interruptible is that some OnExit subroutines might destruct things used by the
	// script's hotkeys/timers/menu items, and activating these items during the deconstruction
	// would not be safe.  Finally, if a logoff or shutdown is occurring, it seems best to prevent
	// timed subroutines from running -- which might take too much time and prevent the exit from
	// occurring in a timely fashion.  An option can be added via the FutureUse param to make it
	// interruptible if there is ever a demand for that.  UPDATE: g_AllowInterruption is now used
	// instead of g.AllowThreadToBeInterrupted for two reasons:
	// 1) It avoids the need to do "int mUninterruptedLineCountMax_prev = g_script.mUninterruptedLineCountMax;"
	//    (Disable this item so that ExecUntil() won't automatically make our new thread uninterruptible
	//    after it has executed a certain number of lines).
	// 2) If the thread we're interrupting is uninterruptible, the uinterruptible timer might be
	//    currently pending.  When it fires, it would make the OnExit subroutine interruptible
	//    rather than the underlying subroutine.  The above fixes the first part of that problem.
	//    The 2nd part is fixed by reinstating the timer when the uninterruptible thread is resumed.
	//    This special handling is only necessary here -- not in other places where new threads are
	//    created -- because OnExit is the only type of thread that can interrupt an uninterruptible
	//    thread.
	bool g_AllowInterruption_prev = g_AllowInterruption;  // Save current setting.
	g_AllowInterruption = false; // Mark the thread just created above as permanently uninterruptible (i.e. until it finishes and is destroyed).

	// This addresses the 2nd part of the problem described in the above large comment:
	bool uninterruptible_timer_was_pending = g_UninterruptibleTimerExists;

	// If the current quasi-thread is paused, the thread we're about to launch
	// will not be, so the icon needs to be checked:
	g_script.UpdateTrayIcon();

	sExitLabelIsRunning = true;
	if (mOnExitLabel->Execute() == FAIL)
		// If the subroutine encounters a failure condition such as a runtime error, exit immediately.
		// Otherwise, there will be no way to exit the script if the subroutine fails on each attempt.
		TerminateApp(aExitCode);
	sExitLabelIsRunning = false;  // In case the user wanted the thread to end normally (see above).

	if (terminate_afterward)
		TerminateApp(aExitCode);

	// Otherwise:
	ResumeUnderlyingThread(&global_saved, ErrorLevel_saved, false);
	g_AllowInterruption = g_AllowInterruption_prev;  // Restore original setting.
	if (uninterruptible_timer_was_pending)
		// Update: An alternative to the below would be to make the current thread interruptible
		// right before the OnExit thread interrupts it, and keep it that way.
		// Below macro recreates the timer if it doesn't already exists (i.e. if it fired during
		// the running of the OnExit subroutine).  Although such a firing would have had
		// no negative impact on the OnExit subroutine (since it's kept always-uninterruptible
		// via g_AllowInterruption), reinstate the timer so that it will make the thread
		// we're resuming interruptible.  The interval might not be exactly right -- since we
		// don't know when the thread started -- but it seems relatively unimportant to
		// worry about such accuracy given how rare and usually-inconsequential this whole
		// scenario is:
		SET_UNINTERRUPTIBLE_TIMER

	return OK;  // for caller convenience.
}



void Script::TerminateApp(int aExitCode)
// Note that g_script's destructor takes care of most other cleanup work, such as destroying
// tray icons, menus, and unowned windows such as ToolTip.
{
	// We call DestroyWindow() because MainWindowProc() has left that up to us.
	// DestroyWindow() will cause MainWindowProc() to immediately receive and process the
	// WM_DESTROY msg, which should in turn result in any child windows being destroyed
	// and other cleanup being done:
	if (IsWindow(g_hWnd)) // Adds peace of mind in case WM_DESTROY was already received in some unusual way.
	{
		g_DestroyWindowCalled = true;
		DestroyWindow(g_hWnd);
	}
	Hotkey::AllDestructAndExit(aExitCode);
}



#ifdef AUTOHOTKEYSC
LineNumberType Script::LoadFromFile()
#else
LineNumberType Script::LoadFromFile(bool aScriptWasNotspecified)
#endif
// Returns the number of non-comment lines that were loaded, or LOADING_FAILED on error.
{
	mNoHotkeyLabels = true;  // Indicate that there are no hotkey labels, since we're (re)loading the entire file.
	mIsReadyToExecute = AutoExecSectionIsRunning = false;
	if (!mFileSpec || !*mFileSpec) return LOADING_FAILED;

#ifndef AUTOHOTKEYSC  // When not in stand-alone mode, read an external script file.
	DWORD attr = GetFileAttributes(mFileSpec);
	if (attr == MAXDWORD) // File does not exist or lacking the authorization to get its attributes.
	{
		char buf[MAX_PATH + 256];
		if (aScriptWasNotspecified) // v1.0.46.09: Give a more descriptive prompt to help users get started.
		{
			snprintf(buf, sizeof(buf),
"To help you get started, would you like to create a sample script in the My Documents folder?\n"
"\n"
"Press YES to create and display the sample script.\n"
"Press NO to exit.\n");
		}
		else // Mostly for backward compatibility, also prompt to create if an explicitly specified script doesn't exist.
			snprintf(buf, sizeof(buf), "The script file \"%s\" does not exist.  Create it now?", mFileSpec);
		int response = MsgBox(buf, MB_YESNO);
		if (response != IDYES)
			return 0;
		FILE *fp2 = fopen(mFileSpec, "a");
		if (!fp2)
		{
			MsgBox("Could not create file, perhaps because the current directory is read-only"
				" or has insufficient permissions.");
			return LOADING_FAILED;
		}
		fputs(
"; IMPORTANT INFO ABOUT GETTING STARTED: Lines that start with a\n"
"; semicolon, such as this one, are comments.  They are not executed.\n"
"\n"
"; This script has a special filename and path because it is automatically\n"
"; launched when you run the program directly.  Also, any text file whose\n"
"; name ends in .ahk is associated with the program, which means that it\n"
"; can be launched simply by double-clicking it.  You can have as many .ahk\n"
"; files as you want, located in any folder.  You can also run more than\n"
"; one ahk file simultaneously and each will get its own tray icon.\n"
"\n"
"; SAMPLE HOTKEYS: Below are two sample hotkeys.  The first is Win+Z and it\n"
"; launches a web site in the default browser.  The second is Control+Alt+N\n"
"; and it launches a new Notepad window (or activates an existing one).  To\n"
"; try out these hotkeys, run AutoHotkey again, which will load this file.\n"
"\n"
"#z::Run www.autohotkey.com\n"
"\n"
"^!n::\n"
"IfWinExist Untitled - Notepad\n"
"\tWinActivate\n"
"else\n"
"\tRun Notepad\n"
"return\n"
"\n"
"\n"
"; Note: From now on whenever you run AutoHotkey directly, this script\n"
"; will be loaded.  So feel free to customize it to suit your needs.\n"
"\n"
"; Please read the QUICK-START TUTORIAL near the top of the help file.\n"
"; It explains how to perform common automation tasks such as sending\n"
"; keystrokes and mouse clicks.  It also explains more about hotkeys.\n"
"\n"
, fp2);
		fclose(fp2);
		// One or both of the below would probably fail -- at least on Win95 -- if mFileSpec ever
		// has spaces in it (since it's passed as the entire param string).  So enclose the filename
		// in double quotes.  I don't believe the directory needs to be in double quotes since it's
		// a separate field within the CreateProcess() and ShellExecute() structures:
		snprintf(buf, sizeof(buf), "\"%s\"", mFileSpec);
		if (!ActionExec("edit", buf, mFileDir, false))
			if (!ActionExec("Notepad.exe", buf, mFileDir, false))
			{
				MsgBox("Can't open script."); // Short msg since so rare.
				return LOADING_FAILED;
			}
		// future: have it wait for the process to close, then try to open the script again:
		return 0;
	}
#endif

	// v1.0.42: Placeholder to use in place of a NULL label to simplify code in some places.
	// This must be created before loading the script because it's relied upon when creating
	// hotkeys to provide an alternative to having a NULL label. It will be given a non-NULL
	// mJumpToLine further down.
	if (   !(mPlaceholderLabel = new Label(""))   ) // Not added to linked list since it's never looked up.
		return LOADING_FAILED;

	// Load the main script file.  This will also load any files it includes with #Include.
	if (   LoadIncludedFile(mFileSpec, false, false) != OK
		|| !AddLine(ACT_EXIT) // Fix for v1.0.47.04: Add an Exit because otherwise, a script that ends in an IF-statement will crash in PreparseBlocks() because PreparseBlocks() expects every IF-statements mNextLine to be non-NULL (helps loading performance too).
		|| !PreparseBlocks(mFirstLine)   ) // Must preparse the blocks before preparsing the If/Else's further below because If/Else may rely on blocks.
		return LOADING_FAILED; // Error was already displayed by the above calls.
	// ABOVE: In v1.0.47, the above may have auto-included additional files from the userlib/stdlib.
	// That's why the above is done prior to adding the EXIT lines and other things below.

#ifndef AUTOHOTKEYSC
	if (mIncludeLibraryFunctionsThenExit)
	{
		fclose(mIncludeLibraryFunctionsThenExit);
		return 0; // Tell our caller to do a normal exit.
	}
#endif

	// v1.0.35.11: Restore original working directory so that changes made to it by the above (via
	// "#Include C:\Scripts" or "#Include %A_ScriptDir%" or even stdlib/userlib) do not affect the
	// script's runtime working directory.  This preserves the flexibility of having a startup-determined
	// working directory for the script's runtime (i.e. it seems best that the mere presence of
	// "#Include NewDir" should not entirely eliminate this flexibility).
	SetCurrentDirectory(g_WorkingDirOrig); // g_WorkingDirOrig previously set by WinMain().

	// Rather than do this, which seems kinda nasty if ever someday support same-line
	// else actions such as "else return", just add two EXITs to the end of every script.
	// That way, if the first EXIT added accidentally "corrects" an actionless ELSE
	// or IF, the second one will serve as the anchoring end-point (mRelatedLine) for that
	// IF or ELSE.  In other words, since we never want mRelatedLine to be NULL, this should
	// make absolutely sure of that:
	//if (mLastLine->mActionType == ACT_ELSE ||
	//	ACT_IS_IF(mLastLine->mActionType)
	//	...
	// Second ACT_EXIT: even if the last line of the script is already "exit", always add
	// another one in case the script ends in a label.  That way, every label will have
	// a non-NULL target, which simplifies other aspects of script execution.
	// Making sure that all scripts end with an EXIT ensures that if the script
	// file ends with ELSEless IF or an ELSE, that IF's or ELSE's mRelatedLine
	// will be non-NULL, which further simplifies script execution.
	// Not done since it's number doesn't much matter: ++mCombinedLineNumber;
	++mCombinedLineNumber;  // So that the EXITs will both show up in ListLines as the line # after the last physical one in the script.
	if (!(AddLine(ACT_EXIT) && AddLine(ACT_EXIT))) // Second exit guaranties non-NULL mRelatedLine(s).
		return LOADING_FAILED;
	mPlaceholderLabel->mJumpToLine = mLastLine; // To follow the rule "all labels should have a non-NULL line before the script starts running".

	if (!PreparseIfElse(mFirstLine))
		return LOADING_FAILED; // Error was already displayed by the above calls.

	// Use FindOrAdd, not Add, because the user may already have added it simply by
	// referring to it in the script:
	if (   !(g_ErrorLevel = FindOrAddVar("ErrorLevel"))   )
		return LOADING_FAILED; // Error.  Above already displayed it for us.
	// Initialize the var state to zero right before running anything in the script:
	g_ErrorLevel->Assign(ERRORLEVEL_NONE);

	// Initialize the random number generator:
	// Note: On 32-bit hardware, the generator module uses only 2506 bytes of static
	// data, so it doesn't seem worthwhile to put it in a class (so that the mem is
	// only allocated on first use of the generator).  For v1.0.24, _ftime() is not
	// used since it could be as large as 0.5 KB of non-compressed code.  A simple call to
	// GetSystemTimeAsFileTime() seems just as good or better, since it produces
	// a FILETIME, which is "the number of 100-nanosecond intervals since January 1, 1601."
	// Use the low-order DWORD since the high-order one rarely changes.  If my calculations are correct,
	// the low-order 32-bits traverses its full 32-bit range every 7.2 minutes, which seems to make
	// using it as a seed superior to GetTickCount for most purposes.
	RESEED_RANDOM_GENERATOR;

	return mLineCount; // The count of runnable lines that were loaded, which might be zero.
}



bool IsFunction(char *aBuf, bool *aPendingFunctionHasBrace = NULL)
// Helper function for LoadIncludedFile().
// Caller passes in an aBuf containing a candidate line such as "function(x, y)"
// Caller has ensured that aBuf is rtrim'd.
// Caller should pass NULL for aPendingFunctionHasBrace to indicate that function definitions (open-brace
// on same line as function) are not allowed.  When non-NULL *and* aBuf is a function call/def,
// *aPendingFunctionHasBrace is set to true if a brace is present at the end, or false otherwise.
// In addition, any open-brace is removed from aBuf in this mode.
{
	char *action_end = StrChrAny(aBuf, EXPR_ALL_SYMBOLS EXPR_ILLEGAL_CHARS);
	// Can't be a function definition or call without an open-parenthesis as first char found by the above.
	// In addition, if action_end isn't NULL, that confirms that the string in aBuf prior to action_end contains
	// no spaces, tabs, colons, or equal-signs.  As a result, it can't be:
	// 1) a hotstring, since they always start with at least one colon that would be caught immediately as
	//    first-expr-char-is-not-open-parenthesis by the above.
	// 2) Any kind of math or assignment, such as var:=(x+y) or var+=(x+y).
	// The only things it could be other than a function call or function definition are:
	// Normal label that ends in single colon but contains an open-parenthesis prior to the colon, e.g. Label(x):
	// Single-line hotkey such as KeyName::MsgBox.  But since '(' isn't valid inside KeyName, this isn't a concern.
	// In addition, note that it isn't necessary to check for colons that lie outside of quoted strings because
	// we're only interested in the first "word" of aBuf: If this is indeed a function call or definition, what
	// lies to the left of its first open-parenthesis can't contain any colons anyway because the above would
	// have caught it as first-expr-char-is-not-open-parenthesis.  In other words, there's no way for a function's
	// opening parenthesis to occur after a legtimate/quoted colon or double-colon in its parameters.
	// v1.0.40.04: Added condition "action_end != aBuf" to allow a hotkey or remap or hotkey such as
	// such as "(::" to work even if it ends in a close-parenthesis such as "(::)" or "(::MsgBox )"
	if (   !(action_end && *action_end == '(' && action_end != aBuf
		&& (action_end - aBuf != 2 || strnicmp(aBuf, "IF", 2)))
		|| action_end[1] == ':'   ) // v1.0.44.07: This prevents "$(::fn_call()" from being seen as a function-call vs. hotkey-with-call.  For simplicity and due to rarity, omit_leading_whitespace() isn't called; i.e. assumes that the colon immediate follows the '('.
		return false;
	char *aBuf_last_char = action_end + strlen(action_end) - 1; // Above has already ensured that action_end is "(...".
	if (aPendingFunctionHasBrace) // Caller specified that an optional open-brace may be present at the end of aBuf.
	{
		if (*aPendingFunctionHasBrace = (*aBuf_last_char == '{')) // Caller has ensured that aBuf is rtrim'd.
		{
			*aBuf_last_char = '\0'; // For the caller, remove it from further consideration.
			aBuf_last_char = aBuf + rtrim(aBuf, aBuf_last_char - aBuf) - 1; // Omit trailing whitespace too.
		}
	}
	return *aBuf_last_char == ')'; // This last check avoids detecting a label such as "Label(x):" as a function.
	// Also, it seems best never to allow if(...) to be a function call, even if it's blank inside such as if().
	// In addition, it seems best not to allow if(...) to ever be a function definition since such a function
	// could never be called as ACT_EXPRESSION since it would be seen as an IF-stmt instead.
}



ResultType Script::LoadIncludedFile(char *aFileSpec, bool aAllowDuplicateInclude, bool aIgnoreLoadFailure)
// Returns OK or FAIL.
// Below: Use double-colon as delimiter to set these apart from normal labels.
// The main reason for this is that otherwise the user would have to worry
// about a normal label being unintentionally valid as a hotkey, e.g.
// "Shift:" might be a legitimate label that the user forgot is also
// a valid hotkey:
#define HOTKEY_FLAG "::"
#define HOTKEY_FLAG_LENGTH 2
{
	if (!aFileSpec || !*aFileSpec) return FAIL;

#ifndef AUTOHOTKEYSC
	if (Line::sSourceFileCount >= Line::sMaxSourceFiles)
	{
		if (Line::sSourceFileCount >= ABSOLUTE_MAX_SOURCE_FILES)
			return ScriptError("Too many includes."); // Short msg since so rare.
		int new_max;
		if (Line::sMaxSourceFiles)
		{
			new_max = 2*Line::sMaxSourceFiles;
			if (new_max > ABSOLUTE_MAX_SOURCE_FILES)
				new_max = ABSOLUTE_MAX_SOURCE_FILES;
		}
		else
			new_max = 100;
		// For simplicity and due to rarity of every needing to, expand by reallocating the array.
		// Use a temp var. because realloc() returns NULL on failure but leaves original block allocated.
		char **realloc_temp = (char **)realloc(Line::sSourceFile, new_max*sizeof(char *)); // If passed NULL, realloc() will do a malloc().
		if (!realloc_temp)
			return ScriptError(ERR_OUTOFMEM); // Short msg since so rare.
		Line::sSourceFile = realloc_temp;
		Line::sMaxSourceFiles = new_max;
	}

	char full_path[MAX_PATH];
#endif

	// Keep this var on the stack due to recursion, which allows newly created lines to be given the
	// correct file number even when some #include's have been encountered in the middle of the script:
	int source_file_index = Line::sSourceFileCount;

	if (!source_file_index)
		// Since this is the first source file, it must be the main script file.  Just point it to the
		// location of the filespec already dynamically allocated:
		Line::sSourceFile[source_file_index] = mFileSpec;
#ifndef AUTOHOTKEYSC  // The "else" part below should never execute for compiled scripts since they never include anything (other than the main/combined script).
	else
	{
		// Get the full path in case aFileSpec has a relative path.  This is done so that duplicates
		// can be reliably detected (we only want to avoid including a given file more than once):
		char *filename_marker;
		GetFullPathName(aFileSpec, sizeof(full_path), full_path, &filename_marker);
		// Check if this file was already included.  If so, it's not an error because we want
		// to support automatic "include once" behavior.  So just ignore repeats:
		if (!aAllowDuplicateInclude)
			for (int f = 0; f < source_file_index; ++f) // Here, source_file_index==Line::sSourceFileCount
				if (!lstrcmpi(Line::sSourceFile[f], full_path)) // Case insensitive like the file system (testing shows that "�" == "�" in the NTFS, which is hopefully how lstrcmpi works regardless of locale).
					return OK;
		// The file is added to the list further below, after the file has been opened, in case the
		// opening fails and aIgnoreLoadFailure==true.
	}
#endif

	UCHAR *script_buf = NULL;  // Init for the case when the buffer isn't used (non-standalone mode).
	ULONG nDataSize = 0;

	// <buf> should be no larger than LINE_SIZE because some later functions rely upon that:
	char msg_text[MAX_PATH + 256], buf1[LINE_SIZE], buf2[LINE_SIZE], suffix[16], pending_function[LINE_SIZE] = "";
	char *buf = buf1, *next_buf = buf2; // Oscillate between bufs to improve performance (avoids memcpy from buf2 to buf1).
	size_t buf_length, next_buf_length, suffix_length;
	bool pending_function_has_brace;

#ifndef AUTOHOTKEYSC
	// Future: might be best to put a stat() or GetFileAttributes() in here for better handling.
	FILE *fp = fopen(aFileSpec, "r");
	if (!fp)
	{
		if (aIgnoreLoadFailure)
			return OK;
		snprintf(msg_text, sizeof(msg_text), "%s file \"%s\" cannot be opened."
			, Line::sSourceFileCount > 0 ? "#Include" : "Script", aFileSpec);
		MsgBox(msg_text);
		return FAIL;
	}
	// v1.0.40.11: Otherwise, check if the first three bytes of the file are the UTF-8 BOM marker (and if
	// so omit them from further consideration).  Apps such as Notepad, WordPad, and Word all insert this
	// marker if the file is saved in UTF-8 format.  This omits such markers from both the main script and
	// any files it includes via #Include.
	// NOTE: To save code size, any UTF-8 BOM bytes at the beginning of a compiled script have already been
	// stripped out by the script compiler.  Thus, there is no need to check for them in the AUTOHOTKEYSC
	// section further below.
	if (fgets(buf, 4, fp)) // Success (the fourth character is the terminator).
	{
		if (strcmp(buf, "﻿"))  // UTF-8 BOM marker is NOT present.
			rewind(fp);  // Go back to the beginning so that the first three bytes aren't omitted during loading.
			// The code size of rewind() has been checked and it seems very tiny.
	}
	//else file read error or EOF, let a later section handle it.

	// This is done only after the file has been successfully opened in case aIgnoreLoadFailure==true:
	if (source_file_index > 0)
		Line::sSourceFile[source_file_index] = SimpleHeap::Malloc(full_path);
	//else the first file was already taken care of by another means.

#else // Stand-alone mode (there are no include files in this mode since all of them were merged into the main script at the time of compiling).
	HS_EXEArc_Read oRead;
	// AutoIt3: Open the archive in this compiled exe.
	// Jon gave me some details about why a password isn't needed: "The code in those libararies will
	// only allow files to be extracted from the exe is is bound to (i.e the script that it was
	// compiled with).  There are various checks and CRCs to make sure that it can't be used to read
	// the files from any other exe that is passed."
	if (oRead.Open(aFileSpec, "") != HS_EXEARC_E_OK)
	{
		MsgBox(ERR_EXE_CORRUPTED, 0, aFileSpec); // Usually caused by virus corruption.
		return FAIL;
	}
	// AutoIt3: Read the script (the func allocates the memory for the buffer :) )
	if (oRead.FileExtractToMem(">AUTOHOTKEY SCRIPT<", &script_buf, &nDataSize) == HS_EXEARC_E_OK)
		mCompiledHasCustomIcon = false;
	else if (oRead.FileExtractToMem(">AHK WITH ICON<", &script_buf, &nDataSize) == HS_EXEARC_E_OK)
		mCompiledHasCustomIcon = true;
	else
	{
		oRead.Close();							// Close the archive
		MsgBox("Could not extract script from EXE.", 0, aFileSpec);
		return FAIL;
	}
	UCHAR *script_buf_marker = script_buf;  // "marker" will track where we are in the mem. file as we read from it.

	// Must cast to int to avoid loss of negative values:
	#define SCRIPT_BUF_SPACE_REMAINING ((int)(nDataSize - (script_buf_marker - script_buf)))
	int script_buf_space_remaining, max_chars_to_read; // script_buf_space_remaining must be an int to detect negatives.

	// AutoIt3: We have the data in RAW BINARY FORM, the script is a text file, so
	// this means that instead of a newline character, there may also be carridge
	// returns 0x0d 0x0a (\r\n)
	HS_EXEArc_Read *fp = &oRead;  // To help consolidate the code below.
#endif

	++Line::sSourceFileCount;

	// File is now open, read lines from it.

	char *hotkey_flag, *cp, *cp1, *action_end, *hotstring_start, *hotstring_options;
	Hotkey *hk;
	LineNumberType pending_function_line_number, saved_line_number;
	HookActionType hook_action;
	bool is_label, suffix_has_tilde, in_comment_section, hotstring_options_all_valid;

	// For the remap mechanism, e.g. a::b
	int remap_stage;
	vk_type remap_source_vk, remap_dest_vk = 0; // Only dest is initialized to enforce the fact that it is the flag/signal to indicate whether remapping is in progress.
	char remap_source[32], remap_dest[32], remap_dest_modifiers[8]; // Must fit the longest key name (currently Browser_Favorites [17]), but buffer overflow is checked just in case.
	char *extra_event;
	bool remap_source_is_mouse, remap_dest_is_mouse, remap_keybd_to_mouse;

	// For the line continuation mechanism:
	bool do_ltrim, do_rtrim, literal_escapes, literal_derefs, literal_delimiters
		, has_continuation_section, is_continuation_line;
	#define CONTINUATION_SECTION_WITHOUT_COMMENTS 1 // MUST BE 1 because it's the default set by anything that's boolean-true.
	#define CONTINUATION_SECTION_WITH_COMMENTS    2 // Zero means "not in a continuation section".
	int in_continuation_section;

	char *next_option, *option_end, orig_char, one_char_string[2], two_char_string[3]; // Line continuation mechanism's option parsing.
	one_char_string[1] = '\0';  // Pre-terminate these to simplify code later below.
	two_char_string[2] = '\0';  //
	int continuation_line_count;

	#define MAX_FUNC_VAR_EXCEPTIONS 2000
	Var *func_exception_var[MAX_FUNC_VAR_EXCEPTIONS];

	// Init both for main file and any included files loaded by this function:
	mCurrFileIndex = source_file_index;  // source_file_index is kept on the stack due to recursion (from #include).

#ifdef AUTOHOTKEYSC
	// -1 (MAX_UINT in this case) to compensate for the fact that there is a comment containing
	// the version number added to the top of each compiled script:
	LineNumberType phys_line_number = -1;
	// For compiled scripts, limit the number of characters to read to however many remain in the memory
	// file or the size of the buffer, whichever is less.
	script_buf_space_remaining = SCRIPT_BUF_SPACE_REMAINING;  // Resolve macro only once, for performance.
	max_chars_to_read = (LINE_SIZE - 1 < script_buf_space_remaining) ? LINE_SIZE - 1
		: script_buf_space_remaining;
	buf_length = GetLine(buf, max_chars_to_read, 0, script_buf_marker);
#else
	LineNumberType phys_line_number = 0;
	buf_length = GetLine(buf, LINE_SIZE - 1, 0, fp);
#endif

	if (in_comment_section = !strncmp(buf, "/*", 2))
	{
		// Fixed for v1.0.35.08. Must reset buffer to allow a script's first line to be "/*".
		*buf = '\0';
		buf_length = 0;
	}

	while (buf_length != -1)  // Compare directly to -1 since length is unsigned.
	{
		// For each whole line (a line with continuation section is counted as only a single line
		// for the purpose of this outer loop).

		// Keep track of this line's *physical* line number within its file for A_LineNumber and
		// error reporting purposes.  This must be done only in the outer loop so that it tracks
		// the topmost line of any set of lines merged due to continuation section/line(s)..
		mCombinedLineNumber = phys_line_number + 1;

		// This must be reset for each iteration because a prior iteration may have changed it, even
		// indirectly by calling something that changed it:
		mCurrLine = NULL;  // To signify that we're in transition, trying to load a new one.

		// v1.0.44.13: An additional call to IsDirective() is now made up here so that #CommentFlag affects
		// the line beneath it the same way as other lines (#EscapeChar et. al. didn't have this bug).
		// It's best not to process ALL directives up here because then they would no longer support a
		// continuation section beneath them (and possibly other drawbacks because it was never thoroughly
		// tested).
		if (!strnicmp(buf, "#CommentFlag", 12)) // Have IsDirective() process this now (it will also process it again later, which is harmless).
			if (IsDirective(buf) == FAIL) // IsDirective() already displayed the error.
				return CloseAndReturn(fp, script_buf, FAIL);

		// Read in the next line (if that next line is the start of a continuation secttion, append
		// it to the line currently being processed:
		for (has_continuation_section = false, in_continuation_section = 0;;)
		{
			// This increment relies on the fact that this loop always has at least one iteration:
			++phys_line_number; // Tracks phys. line number in *this* file (independent of any recursion caused by #Include).
#ifdef AUTOHOTKEYSC
			// See similar section above for comments about the following:
			script_buf_space_remaining = SCRIPT_BUF_SPACE_REMAINING;  // Resolve macro only once, for performance.
			max_chars_to_read = (LINE_SIZE - 1 < script_buf_space_remaining) ? LINE_SIZE - 1
				: script_buf_space_remaining;
			next_buf_length = GetLine(next_buf, max_chars_to_read, in_continuation_section, script_buf_marker);
#else
			next_buf_length = GetLine(next_buf, LINE_SIZE - 1, in_continuation_section, fp);
#endif
			if (next_buf_length && next_buf_length != -1) // Prevents infinite loop when file ends with an unclosed "/*" section.  Compare directly to -1 since length is unsigned.
			{
				if (in_comment_section) // Look for the uncomment-flag.
				{
					if (!strncmp(next_buf, "*/", 2))
					{
						in_comment_section = false;
						next_buf_length -= 2; // Adjust for removal of /* from the beginning of the string.
						memmove(next_buf, next_buf + 2, next_buf_length + 1);  // +1 to include the string terminator.
						next_buf_length = ltrim(next_buf, next_buf_length); // Get rid of any whitespace that was between the comment-end and remaining text.
						if (!*next_buf) // The rest of the line is empty, so it was just a naked comment-end.
							continue;
					}
					else
						continue;
				}
				else if (!in_continuation_section && !strncmp(next_buf, "/*", 2))
				{
					in_comment_section = true;
					continue; // It's now commented out, so the rest of this line is ignored.
				}
			}

			if (in_comment_section) // Above has incremented and read the next line, which is everything needed while inside /* .. */
			{
				if (next_buf_length == -1) // Compare directly to -1 since length is unsigned.
					break; // By design, it's not an error.  This allows "/*" to be used to comment out the bottommost portion of the script without needing a matching "*/".
				// Otherwise, continue reading lines so that they can be merged with the line above them
				// if they qualify as continuation lines.
				continue;
			}

			if (!in_continuation_section) // This is either the first iteration or the line after the end of a previous continuation section.
			{
				// v1.0.38.06: The following has been fixed to exclude "(:" and "(::".  These should be
				// labels/hotkeys, not the start of a contination section.  In addition, a line that starts
				// with '(' but that ends with ':' should be treated as a label because labels such as
				// "(label):" are far more common than something obscure like a continuation section whose
				// join character is colon, namely "(Join:".
				if (   !(in_continuation_section = (next_buf_length != -1 && *next_buf == '(' // Compare directly to -1 since length is unsigned.
					&& next_buf[1] != ':' && next_buf[next_buf_length - 1] != ':'))   ) // Relies on short-circuit boolean order.
				{
					if (next_buf_length == -1)  // Compare directly to -1 since length is unsigned.
						break;
					if (!next_buf_length)
						// It is permitted to have blank lines and comment lines in between the line above
						// and any continuation section/line that might come after the end of the
						// comment/blank lines:
						continue;
					// SINCE ABOVE DIDN'T BREAK/CONTINUE, NEXT_BUF IS NON-BLANK.
					if (next_buf[next_buf_length - 1] == ':' && *next_buf != ',')
						// With the exception of lines starting with a comma, the last character of any
						// legitimate continuation line can't be a colon because expressions can't end
						// in a colon. The only exception is the ternary operator's colon, but that is
						// very rare because it requires the line after it also be a continuation line
						// or section, which is unusual to say the least -- so much so that it might be
						// too obscure to even document as a known limitation.  Anyway, by excluding lines
						// that end with a colon from consideration ambiguity with normal labels
						// and non-single-line hotkeys and hotstrings is eliminated.
						break;

					is_continuation_line = false; // Set default.
					switch(toupper(*next_buf)) // Above has ensured *next_buf != '\0' (toupper might have problems with '\0').
					{
					case 'A': // "AND".
						// See comments in the default section further below.
						if (!strnicmp(next_buf, "and", 3) && IS_SPACE_OR_TAB_OR_NBSP(next_buf[3])) // Relies on short-circuit boolean order.
						{
							cp = omit_leading_whitespace(next_buf + 3);
							// v1.0.38.06: The following was fixed to use EXPR_CORE vs. EXPR_OPERAND_TERMINATORS
							// to properly detect a continuation line whose first char after AND/OR is "!~*&-+()":
							if (!strchr(EXPR_CORE, *cp))
								// This check recognizes the following examples as NON-continuation lines by checking
								// that AND/OR aren't followed immediately by something that's obviously an operator:
								//    and := x, and = 2 (but not and += 2 since the an operand can have a unary plus/minus).
								// This is done for backward compatibility.  Also, it's documented that
								// AND/OR/NOT aren't supported as variable names inside expressions.
								is_continuation_line = true; // Override the default set earlier.
						}
						break;
					case 'O': // "OR".
						// See comments in the default section further below.
						if (toupper(next_buf[1]) == 'R' && IS_SPACE_OR_TAB_OR_NBSP(next_buf[2])) // Relies on short-circuit boolean order.
						{
							cp = omit_leading_whitespace(next_buf + 2);
							// v1.0.38.06: The following was fixed to use EXPR_CORE vs. EXPR_OPERAND_TERMINATORS
							// to properly detect a continuation line whose first char after AND/OR is "!~*&-+()":
							if (!strchr(EXPR_CORE, *cp)) // See comment in the "AND" case above.
								is_continuation_line = true; // Override the default set earlier.
						}
						break;
					default:
						// Desired line continuation operators:
						// Pretty much everything, namely:
						// +, -, *, /, //, **, <<, >>, &, |, ^, <, >, <=, >=, =, ==, <>, !=, :=, +=, -=, /=, *=, ?, :
						// And also the following remaining unaries (i.e. those that aren't also binaries): !, ~
						// The first line below checks for ::, ++, and --.  Those can't be continuation lines because:
						// "::" isn't a valid operator (this also helps performance if there are many hotstrings).
						// ++ and -- are ambiguous with an isolated line containing ++Var or --Var (and besides,
						// wanting to use ++ to continue an expression seems extremely rare, though if there's ever
						// demand for it, might be able to look at what lies to the right of the operator's operand
						// -- though that would produce inconsisent continuation behavior since ++Var itself still
						// could never be a continuation line due to ambiguity).
						//
						// The logic here isn't smart enough to differentiate between a leading ! or - that's
						// meant as a continuation character and one that isn't. Even if it were, it would
						// still be ambiguous in some cases because the author's intent isn't known; for example,
						// the leading minus sign on the second line below is ambiguous, so will probably remain
						// a continuation character in both v1 and v2:
						//    x := y
						//    -z ? a:=1 : func()
						if ((*next_buf == ':' || *next_buf == '+' || *next_buf == '-') && next_buf[1] == *next_buf // See above.
							|| (*next_buf == '.' || *next_buf == '?') && !IS_SPACE_OR_TAB_OR_NBSP(next_buf[1]) // The "." and "?" operators require a space or tab after them to be legitimate.  For ".", this is done in case period is ever a legal character in var names, such as struct support.  For "?", it's done for backward compatibility since variable names can contain question marks (though "?" by itself is not considered a variable in v1.0.46).
								&& next_buf[1] != '=' // But allow ".=" (and "?=" too for code simplicity), since ".=" is the concat-assign operator.
							|| !strchr(CONTINUATION_LINE_SYMBOLS, *next_buf)) // Line doesn't start with a continuation char.
							break; // Leave is_continuation_line set to its default of false.
						// Some of the above checks must be done before the next ones.
						if (   !(hotkey_flag = strstr(next_buf, HOTKEY_FLAG))   ) // Without any "::", it can't be a hotkey or hotstring.
						{
							is_continuation_line = true; // Override the default set earlier.
							break;
						}
						if (*next_buf == ':') // First char is ':', so it's more likely a hotstring than a hotkey.
						{
							// Remember that hotstrings can contain what *appear* to be quoted literal strings,
							// so detecting whether a "::" is in a quoted/literal string in this case would
							// be more complicated.  That's one reason this other method is used.
							for (hotstring_options_all_valid = true, cp = next_buf + 1; *cp && *cp != ':'; ++cp)
								if (!IS_HOTSTRING_OPTION(*cp)) // Not a perfect test, but eliminates most of what little remaining ambiguity exists between ':' as a continuation character vs. ':' as the start of a hotstring.  It especially eliminates the ":=" operator.
								{
									hotstring_options_all_valid = false;
									break;
								}
							if (hotstring_options_all_valid && *cp == ':') // It's almost certainly a hotstring.
								break; // So don't treat it as a continuation line.
							//else it's not a hotstring but it might still be a hotkey such as ": & x::".
							// So continue checking below.
						}
						// Since above didn't "break", this line isn't a hotstring but it is probably a hotkey
						// because above already discovered that it contains "::" somewhere. So try to find out
						// if there's anything that disqualifies this from being a hotkey, such as some
						// expression line that contains a quoted/literal "::" (or a line starting with
						// a comma that contains an unquoted-but-literal "::" such as for FileAppend).
						if (*next_buf == ',')
						{
							cp = omit_leading_whitespace(next_buf + 1);
							// The above has set cp to the position of the non-whitespace item to the right of
							// this comma.  Normal (single-colon) labels can't contain commas, so only hotkey
							// labels are sources of ambiguity.  In addition, normal labels and hotstrings have
							// already been checked for, higher above.
							if (   strncmp(cp, HOTKEY_FLAG, HOTKEY_FLAG_LENGTH) // It's not a hotkey such as ",::action".
								&& strncmp(cp - 1, COMPOSITE_DELIMITER, COMPOSITE_DELIMITER_LENGTH)   ) // ...and it's not a hotkey such as ", & y::action".
								is_continuation_line = true; // Override the default set earlier.
						}
						else // First symbol in line isn't a comma but some other operator symbol.
						{
							// Check if the "::" found earlier appears to be inside a quoted/literal string.
							// This check is NOT done for a line beginning with a comma since such lines
							// can contain an unquoted-but-literal "::".  In addition, this check is done this
							// way to detect hotkeys such as the following:
							//   +keyname:: (and other hotkey modifier symbols such as ! and ^)
							//   +keyname1 & keyname2::
							//   +^:: (i.e. a modifier symbol followed by something that is a hotkey modifer and/or a hotkey suffix and/or an expression operator).
							//   <:: and &:: (i.e. hotkeys that are also expression-continuation symbols)
							// By contrast, expressions that qualify as continuation lines can look like:
							//   . "xxx::yyy"
							//   + x . "xxx::yyy"
							// In addition, hotkeys like the following should continue to be supported regardless
							// of how things are done here:
							//   ^"::
							//   . & "::
							// Finally, keep in mind that an expression-continuation line can start with two
							// consecutive unary operators like !! or !*. It can also start with a double-symbol
							// operator such as <=, <>, !=, &&, ||, //, **.
							for (cp = next_buf; cp < hotkey_flag && *cp != '"'; ++cp);
							if (cp == hotkey_flag) // No '"' found to left of "::", so this "::" appears to be a real hotkey flag rather than part of a literal string.
								break; // Treat this line as a normal line vs. continuation line.
							for (cp = hotkey_flag + HOTKEY_FLAG_LENGTH; *cp && *cp != '"'; ++cp);
							if (*cp)
							{
								// Closing quote was found so "::" is probably inside a literal string of an
								// expression (further checking seems unnecessary given the fairly extreme
								// rarity of using '"' as a key in a hotkey definition).
								is_continuation_line = true; // Override the default set earlier.
							}
							//else no closing '"' found, so this "::" probably belongs to something like +":: or
							// . & "::.  Treat this line as a normal line vs. continuation line.
						}
					} // switch(toupper(*next_buf))

					if (is_continuation_line)
					{
						if (buf_length + next_buf_length >= LINE_SIZE - 1) // -1 to account for the extra space added below.
						{
							ScriptError(ERR_CONTINUATION_SECTION_TOO_LONG, next_buf);
							return CloseAndReturn(fp, script_buf, FAIL);
						}
						if (*next_buf != ',') // Insert space before expression operators so that built/combined expression works correctly (some operators like 'and', 'or', '.', and '?' currently require spaces on either side) and also for readability of ListLines.
							buf[buf_length++] = ' ';
						memcpy(buf + buf_length, next_buf, next_buf_length + 1); // Append this line to prev. and include the zero terminator.
						buf_length += next_buf_length;
						continue; // Check for yet more continuation lines after this one.
					}
					// Since above didn't continue, there is no continuation line or section.  In addition,
					// since this line isn't blank, no further searching is needed.
					break;
				} // if (!in_continuation_section)

				// OTHERWISE in_continuation_section != 0, so the above has found the first line of a new
				// continuation section.
				// "has_continuation_section" indicates whether the line we're about to construct is partially
				// composed of continuation lines beneath it.  It's separate from continuation_line_count
				// in case there is another continuation section immediately after/adjacent to the first one,
				// but the second one doesn't have any lines in it:
				has_continuation_section = true;
				continuation_line_count = 0; // Reset for this new section.
				// Otherwise, parse options.  First set the defaults, which can be individually overridden
				// by any options actually present.  RTrim defaults to ON for two reasons:
				// 1) Whitespace often winds up at the end of a lines in a text editor by accident.  In addition,
				//    whitespace at the end of any consolidated/merged line will be rtrim'd anyway, since that's
				//    how command parsing works.
				// 2) Copy & paste from the forum and perhaps other web sites leaves a space at the end of each
				//    line.  Although this behavior is probably site/browser-specific, it's a consideration.
				do_ltrim = g_ContinuationLTrim; // Start off at global default.
				do_rtrim = true; // Seems best to rtrim even if this line is a hotstring, since it is very rare that trailing spaces and tabs would ever be desirable.
				// For hotstrings (which could be detected via *buf==':'), it seems best not to default the
				// escape character (`) to be literal because the ability to have `t `r and `n inside the
				// hotstring continuation section seems more useful/common than the ability to use the
				// accent character by itself literally (which seems quite rare in most languages).
				literal_escapes = false;
				literal_derefs = false;
				literal_delimiters = true; // This is the default even for hotstrings because although using (*buf != ':') would improve loading performance, it's not a 100% reliable way to detect hotstrings.
				// The default is linefeed because:
				// 1) It's the best choice for hotstrings, for which the line continuation mechanism is well suited.
				// 2) It's good for FileAppend.
				// 3) Minor: Saves memory in large sections by being only one character instead of two.
				suffix[0] = '\n';
				suffix[1] = '\0';
				suffix_length = 1;
				for (next_option = omit_leading_whitespace(next_buf + 1); *next_option; next_option = omit_leading_whitespace(option_end))
				{
					// Find the end of this option item:
					if (   !(option_end = StrChrAny(next_option, " \t"))   )  // Space or tab.
						option_end = next_option + strlen(next_option); // Set to position of zero terminator instead.

					// Temporarily terminate to help eliminate ambiguity for words contained inside other words,
					// such as hypothetical "Checked" inside of "CheckedGray":
					orig_char = *option_end;
					*option_end = '\0';

					if (!strnicmp(next_option, "Join", 4))
					{
						next_option += 4;
						strlcpy(suffix, next_option, sizeof(suffix)); // The word "Join" by itself will product an empty string, as documented.
						// Passing true for the last parameter supports `s as the special escape character,
						// which allows space to be used by itself and also at the beginning or end of a string
						// containing other chars.
						ConvertEscapeSequences(suffix, g_EscapeChar, true);
						suffix_length = strlen(suffix);
					}
					else if (!strnicmp(next_option, "LTrim", 5))
						do_ltrim = (next_option[5] != '0');  // i.e. Only an explicit zero will turn it off.
					else if (!strnicmp(next_option, "RTrim", 5))
						do_rtrim = (next_option[5] != '0');
					else
					{
						// Fix for v1.0.36.01: Missing "else" above, because otherwise, the option Join`r`n
						// would be processed above but also be processed again below, this time seeing the
						// accent and thinking it's the signal to treat accents literally for the entire
						// continuation section rather than as escape characters.
						// Within this terminated option substring, allow the characters to be adjacent to
						// improve usability:
						for (; *next_option; ++next_option)
						{
							switch (*next_option)
							{
							case '`': // Although not using g_EscapeChar (reduces code size/complexity), #EscapeChar is still supported by continuation sections; it's just that enabling the option uses '`' rather than the custom escape-char (one reason is that that custom escape-char might be ambiguous with future/past options if it's somehing weird like an alphabetic character).
								literal_escapes = true;
								break;
							case '%': // Same comment as above.
								literal_derefs = true;
								break;
							case ',': // Same comment as above.
								literal_delimiters = false;
								break;
							case 'C': // v1.0.45.03: For simplicity, anything that begins with "C" is enough to
							case 'c': // identify it as the option to allow comments in the section.
								in_continuation_section = CONTINUATION_SECTION_WITH_COMMENTS; // Override the default, which is boolean true (i.e. 1).
								break;
							}
						}
					}

					// If the item was not handled by the above, ignore it because it is unknown.

					*option_end = orig_char; // Undo the temporary termination.

				} // for() each item in option list

				continue; // Now that the open-parenthesis of this continuation section has been processed, proceed to the next line.
			} // if (!in_continuation_section)

			// Since above didn't "continue", we're in the continuation section and thus next_buf contains
			// either a line to be appended onto buf or the closing parenthesis of this continuation section.
			if (next_buf_length == -1) // Compare directly to -1 since length is unsigned.
			{
				ScriptError(ERR_MISSING_CLOSE_PAREN, buf);
				return CloseAndReturn(fp, script_buf, FAIL);
			}
			if (next_buf_length == -2) // v1.0.45.03: Special flag that means "this is a commented-out line to be
				continue;              // entirely omitted from the continuation section." Compare directly to -2 since length is unsigned.

			if (*next_buf == ')')
			{
				in_continuation_section = 0; // Facilitates back-to-back continuation sections and proper incrementing of phys_line_number.
				next_buf_length = rtrim(next_buf); // Done because GetLine() wouldn't have done it due to have told it we're in a continuation section.
				// Anything that lies to the right of the close-parenthesis gets appended verbatim, with
				// no trimming (for flexibility) and no options-driven translation:
				cp = next_buf + 1;  // Use temp var cp to avoid altering next_buf (for maintainability).
				--next_buf_length;  // This is now the length of cp, not next_buf.
			}
			else
			{
				cp = next_buf;
				// The following are done in this block only because anything that comes after the closing
				// parenthesis (i.e. the block above) is exempt from translations and custom trimming.
				// This means that commas are always delimiters and percent signs are always deref symbols
				// in the previous block.
				if (do_rtrim)
					next_buf_length = rtrim(next_buf, next_buf_length);
				if (do_ltrim)
					next_buf_length = ltrim(next_buf, next_buf_length);
				// Escape each comma and percent sign in the body of the continuation section so that
				// the later parsing stages will see them as literals.  Although, it's not always
				// necessary to do this (e.g. commas in the last parameter of a command don't need to
				// be escaped, nor do percent signs in hotstrings' auto-replace text), the settings
				// are applied unconditionally because:
				// 1) Determining when its safe to omit the translation would add a lot of code size and complexity.
				// 2) The translation doesn't affect the functionality of the script since escaped literals
				//    are always de-escaped at a later stage, at least for everything that's likely to matter
				//    or that's reasonable to put into a continuation section (e.g. a hotstring's replacement text).
				// UPDATE for v1.0.44.11: #EscapeChar, #DerefChar, #Delimiter are now supported by continuation
				// sections because there were some requests for that in forum.
				int replacement_count = 0;
				if (literal_escapes) // literal_escapes must be done FIRST because otherwise it would also replace any accents added for literal_delimiters or literal_derefs.
				{
					one_char_string[0] = g_EscapeChar; // These strings were terminated earlier, so no need to
					two_char_string[0] = g_EscapeChar; // do it here.  In addition, these strings must be set by
					two_char_string[1] = g_EscapeChar; // each iteration because the #EscapeChar (and similar directives) can occur multiple times, anywhere in the script.
					replacement_count += StrReplace(next_buf, one_char_string, two_char_string, SCS_SENSITIVE, UINT_MAX, LINE_SIZE);
				}
				if (literal_derefs)
				{
					one_char_string[0] = g_DerefChar;
					two_char_string[0] = g_EscapeChar;
					two_char_string[1] = g_DerefChar;
					replacement_count += StrReplace(next_buf, one_char_string, two_char_string, SCS_SENSITIVE, UINT_MAX, LINE_SIZE);
				}
				if (literal_delimiters)
				{
					one_char_string[0] = g_delimiter;
					two_char_string[0] = g_EscapeChar;
					two_char_string[1] = g_delimiter;
					replacement_count += StrReplace(next_buf, one_char_string, two_char_string, SCS_SENSITIVE, UINT_MAX, LINE_SIZE);
				}

				if (replacement_count) // Update the length if any actual replacements were done.
					next_buf_length = strlen(next_buf);
			} // Handling of a normal line within a continuation section.

			// Must check the combined length only after anything that might have expanded the string above.
			if (buf_length + next_buf_length + suffix_length >= LINE_SIZE)
			{
				ScriptError(ERR_CONTINUATION_SECTION_TOO_LONG, cp);
				return CloseAndReturn(fp, script_buf, FAIL);
			}

			++continuation_line_count;
			// Append this continuation line onto the primary line.
			// The suffix for the previous line gets written immediately prior writing this next line,
			// which allows the suffix to be omitted for the final line.  But if this is the first line,
			// No suffix is written because there is no previous line in the continuation section.
			// In addition, cp!=next_buf, this is the special line whose text occurs to the right of the
			// continuation section's closing parenthesis. In this case too, the previous line doesn't
			// get a suffix.
			if (continuation_line_count > 1 && suffix_length && cp == next_buf)
			{
				memcpy(buf + buf_length, suffix, suffix_length + 1); // Append and include the zero terminator.
				buf_length += suffix_length; // Must be done only after the old value of buf_length was used above.
			}
			if (next_buf_length)
			{
				memcpy(buf + buf_length, cp, next_buf_length + 1); // Append this line to prev. and include the zero terminator.
				buf_length += next_buf_length; // Must be done only after the old value of buf_length was used above.
			}
		} // for() each sub-line (continued line) that composes this line.

		// buf_length can't be -1 (though next_buf_length can) because outer loop's condition prevents it:
		if (!buf_length) // Done only after the line number increments above so that the physical line number is properly tracked.
			goto continue_main_loop; // In lieu of "continue", for performance.

		// Since neither of the above executed, or they did but didn't "continue",
		// buf now contains a non-commented line, either by itself or built from
		// any continuation sections/lines that might have been present.  Also note that
		// by design, phys_line_number will be greater than mCombinedLineNumber whenever
		// a continuation section/lines were used to build this combined line.

		// If there's a previous line waiting to be processed, its fate can now be determined based on the
		// nature of *this* line:
		if (*pending_function)
		{
			// Somewhat messy to decrement then increment later, but it's probably easier than the
			// alternatives due to the use of "continue" in some places above.  NOTE: phys_line_number
			// would not need to be decremented+incremented even if the below resulted in a recursive
			// call to us (though it doesn't currently) because line_number's only purpose is to
			// remember where this layer left off when the recursion collapses back to us.
			// Fix for v1.0.31.05: It's not enough just to decrement mCombinedLineNumber because there
			// might be some blank lines or commented-out lines between this function call/definition
			// and the line that follows it, each of which will have previously incremented mCombinedLineNumber.
			saved_line_number = mCombinedLineNumber;
			mCombinedLineNumber = pending_function_line_number;  // Done so that any syntax errors that occur during the calls below will report the correct line number.
			// Open brace means this is a function definition. NOTE: buf was already ltrimmed by GetLine().
			// Could use *g_act[ACT_BLOCK_BEGIN].Name instead of '{', but it seems too elaborate to be worth it.
			if (*buf == '{' || pending_function_has_brace) // v1.0.41: Support one-true-brace, e.g. fn(...) {
			{
				// Note that two consecutive function definitions aren't possible:
				// fn1()
				// fn2()
				// {
				//  ...
				// }
				// In the above, the first would automatically be deemed a function call by means of
				// the check higher above (by virtue of the fact that the line after it isn't an open-brace).
				if (g.CurrentFunc)
				{
					// Though it might be allowed in the future -- perhaps to have nested functions have
					// access to their parent functions' local variables, or perhaps just to improve
					// script readability and maintainability -- it's currently not allowed because of
					// the practice of maintaining the func_exception_var list on our stack:
					ScriptError("Functions cannot contain functions.", pending_function);
					return CloseAndReturn(fp, script_buf, FAIL);
				}
				if (!DefineFunc(pending_function, func_exception_var))
					return CloseAndReturn(fp, script_buf, FAIL);
				if (pending_function_has_brace) // v1.0.41: Support one-true-brace for function def, e.g. fn() {
					if (!AddLine(ACT_BLOCK_BEGIN))
						return CloseAndReturn(fp, script_buf, FAIL);
			}
			else // It's a function call on a line by itself, such as fn(x). It can't be if(..) because another section checked that.
			{
				if (!ParseAndAddLine(pending_function, ACT_EXPRESSION))
					return CloseAndReturn(fp, script_buf, FAIL);
				mCurrLine = NULL; // Prevents showing misleading vicinity lines if the line after a function call is a syntax error.
			}
			mCombinedLineNumber = saved_line_number;
			*pending_function = '\0'; // Reset now that it's been fully handled, as an indicator for subsequent iterations.
			// Now fall through to the below so that *this* line (the one after it) will be processed.
			// Note that this line might be a pre-processor directive, label, etc. that won't actually
			// become a runtime line per se.
		} // if (*pending_function)

		// By doing the following section prior to checking for hotkey and hotstring labels, double colons do
		// not need to be escaped inside naked function calls and function definitions such as the following:
		// fn("::")      ; Function call.
		// fn(Str="::")  ; Function definition with default value for its param (though technically, strings other than "" aren't yet supported).
		if (IsFunction(buf, &pending_function_has_brace)) // If true, it's either a function definition or a function call (to be distinguished later).
		{
			// Defer this line until the next line comes in, which helps determine whether this line is
			// a function call vs. definition:
			strcpy(pending_function, buf);
			pending_function_line_number = mCombinedLineNumber;
			goto continue_main_loop; // In lieu of "continue", for performance.
		}

		// The following "examine_line" label skips the following parts above:
		// 1) IsFunction() because that's only for a function call or definition alone on a line
		//    e.g. not "if fn()" or x := fn().  Those who goto this label don't need that processing.
		// 2) The "if (*pending_function)" block: Doesn't seem applicable for the callers of this label.
		// 3) The inner loop that handles continuation sections: Not needed by the callers of this label.
		// 4) Things like the following should be skipped because callers of this label don't want the
		//    physical line number changed (which would throw off the count of lines that lie beneath a remap):
		//    mCombinedLineNumber = phys_line_number + 1;
		//    ++phys_line_number;
		// 5) "mCurrLine = NULL": Probably not necessary since it's only for error reporting.  Worst thing
		//    that could happen is that syntax errors would be thrown off, which testing shows isn't the case.
examine_line:
		// "::" alone isn't a hotstring, it's a label whose name is colon.
		// Below relies on the fact that no valid hotkey can start with a colon, since
		// ": & somekey" is not valid (since colon is a shifted key) and colon itself
		// should instead be defined as "+;::".  It also relies on short-circuit boolean:
		hotstring_start = NULL;
		hotstring_options = NULL; // Set default as "no options were specified for this hotstring".
		hotkey_flag = NULL;
		if (buf[0] == ':' && buf[1])
		{
			if (buf[1] != ':')
			{
				hotstring_options = buf + 1; // Point it to the hotstring's option letters.
				// The following relies on the fact that options should never contain a literal colon.
				// ALSO, the following doesn't use IS_HOTSTRING_OPTION() for backward compatibility,
				// performance, and because it seems seldom if ever necessary at this late a stage.
				if (   !(hotstring_start = strchr(hotstring_options, ':'))   )
					hotstring_start = NULL; // Indicate that this isn't a hotstring after all.
				else
					++hotstring_start; // Points to the hotstring itself.
			}
			else // Double-colon, so it's a hotstring if there's more after this (but this means no options are present).
				if (buf[2])
					hotstring_start = buf + 2; // And leave hotstring_options at its default of NULL to indicate no options.
				//else it's just a naked "::", which is considered to be an ordinary label whose name is colon.
		}
		if (hotstring_start)
		{
			// Find the hotstring's final double-colon by considering escape sequences from left to right.
			// This is necessary for to handles cases such as the following:
			// ::abc```::::Replacement String
			// The above hotstring translates literally into "abc`::".
			char *escaped_double_colon = NULL;
			for (cp = hotstring_start; ; ++cp)  // Increment to skip over the symbol just found by the inner for().
			{
				for (; *cp && *cp != g_EscapeChar && *cp != ':'; ++cp);  // Find the next escape char or colon.
				if (!*cp) // end of string.
					break;
				cp1 = cp + 1;
				if (*cp == ':')
				{
					if (*cp1 == ':') // Found a non-escaped double-colon, so this is the right one.
					{
						hotkey_flag = cp++;  // Increment to have loop skip over both colons.
						// and the continue with the loop so that escape sequences in the replacement
						// text (if there is replacement text) are also translated.
					}
					// else just a single colon, or the second colon of an escaped pair (`::), so continue.
					continue;
				}
				switch (*cp1)
				{
					// Only lowercase is recognized for these:
					case 'a': *cp1 = '\a'; break;  // alert (bell) character
					case 'b': *cp1 = '\b'; break;  // backspace
					case 'f': *cp1 = '\f'; break;  // formfeed
					case 'n': *cp1 = '\n'; break;  // newline
					case 'r': *cp1 = '\r'; break;  // carriage return
					case 't': *cp1 = '\t'; break;  // horizontal tab
					case 'v': *cp1 = '\v'; break;  // vertical tab
					// Otherwise, if it's not one of the above, the escape-char is considered to
					// mark the next character as literal, regardless of what it is. Examples:
					// `` -> `
					// `:: -> :: (effectively)
					// `; -> ;
					// `c -> c (i.e. unknown escape sequences resolve to the char after the `)
				}
				// Below has a final +1 to include the terminator:
				MoveMemory(cp, cp1, strlen(cp1) + 1);
				// Since single colons normally do not need to be escaped, this increments one extra
				// for double-colons to skip over the entire pair so that its second colon
				// is not seen as part of the hotstring's final double-colon.  Example:
				// ::ahc```::::Replacement String
				if (*cp == ':' && *cp1 == ':')
					++cp;
			} // for()
			if (!hotkey_flag)
				hotstring_start = NULL;  // Indicate that this isn't a hotstring after all.
		}
		if (!hotstring_start) // Not a hotstring (hotstring_start is checked *again* in case above block changed it; otherwise hotkeys like ": & x" aren't recognized).
		{
			// Note that there may be an action following the HOTKEY_FLAG (on the same line).
			if (hotkey_flag = strstr(buf, HOTKEY_FLAG)) // Find the first one from the left, in case there's more than 1.
			{
				if (hotkey_flag == buf && hotkey_flag[2] == ':') // v1.0.46: Support ":::" to mean "colon is a hotkey".
					++hotkey_flag;
				// v1.0.40: It appears to be a hotkey, but validate it as such before committing to processing
				// it as a hotkey.  If it fails validation as a hotkey, treat it as a command that just happens
				// to contain a double-colon somewhere.  This avoids the need to escape double colons in scripts.
				// Note: Hotstrings can't suffer from this type of ambiguity because a leading colon or pair of
				// colons makes them easier to detect.
				cp = omit_trailing_whitespace(buf, hotkey_flag); // For maintainability.
				orig_char = *cp;
				*cp = '\0'; // Temporarily terminate.
				if (!Hotkey::TextInterpret(omit_leading_whitespace(buf), NULL, false)) // Passing NULL calls it in validate-only mode.
					hotkey_flag = NULL; // It's not a valid hotkey, so indicate that it's a command (i.e. one that contains a literal double-colon, which avoids the need to escape the double-colon).
				*cp = orig_char; // Undo the temp. termination above.
			}
		}

		// Treat a naked "::" as a normal label whose label name is colon:
		if (is_label = (hotkey_flag && hotkey_flag > buf)) // It's a hotkey/hotstring label.
		{
			if (g.CurrentFunc)
			{
				// Even if it weren't for the reasons below, the first hotkey/hotstring label in a script
				// will end the auto-execute section with a "return".  Therefore, if this restriction here
				// is ever removed, be sure that that extra return doesn't get put inside the function.
				//
				// The reason for not allowing hotkeys and hotstrings inside a function's body is that
				// when the subroutine is launched, the hotstring/hotkey would be using the function's
				// local variables.  But that is not appropriate and it's likely to cause problems even
				// if it were.  It doesn't seem useful in any case.  By contrast, normal labels can
				// safely exist inside a function body and since the body is a block, other validation
				// ensures that a Gosub or Goto can't jump to it from outside the function.
				ScriptError("Hotkeys/hotstrings are not allowed inside functions.", buf);
				return CloseAndReturn(fp, script_buf, FAIL);
			}
			if (mLastLine && mLastLine->mActionType == ACT_IFWINACTIVE)
			{
				mCurrLine = mLastLine; // To show vicinity lines.
				ScriptError("IfWin should be #IfWin.", buf);
				return CloseAndReturn(fp, script_buf, FAIL);
			}
			*hotkey_flag = '\0'; // Terminate so that buf is now the label itself.
			hotkey_flag += HOTKEY_FLAG_LENGTH;  // Now hotkey_flag is the hotkey's action, if any.
			if (!hotstring_start)
			{
				ltrim(hotkey_flag); // Has already been rtrimmed by GetLine().
				rtrim(buf); // Trim the new substring inside of buf (due to temp termination). It has already been ltrimmed.
				cp = hotkey_flag; // Set default, conditionally overridden below (v1.0.44.07).
				// v1.0.40: Check if this is a remap rather than hotkey:
				if (   *hotkey_flag // This hotkey's action is on the same line as its label.
					&& (remap_source_vk = TextToVK(cp1 = Hotkey::TextToModifiers(buf, NULL)))
					&& (remap_dest_vk = hotkey_flag[1] ? TextToVK(cp = Hotkey::TextToModifiers(hotkey_flag, NULL)) : 0xFF)   ) // And the action appears to be a remap destination rather than a command.
					// For above:
					// Fix for v1.0.44.07: Set remap_dest_vk to 0xFF if hotkey_flag's length is only 1 because:
					// 1) It allows a destination key that doesn't exist in the keyboard layout (such as 6::� in
					//    English).
					// 2) It improves performance a little by not calling TextToVK except when the destination key
					//    might be a mouse button or some longer key name whose actual/correct VK value is relied
					//    upon by other places below.
					// Fix for v1.0.40.01: Since remap_dest_vk is also used as the flag to indicate whether
					// this line qualifies as a remap, must do it last in the statement above.  Otherwise,
					// the statement might short-circuit and leave remap_dest_vk as non-zero even though
					// the line shouldn't be a remap.  For example, I think a hotkey such as "x & y::return"
					// would trigger such a bug.
				{
					// These will be ignored in other stages if it turns out not to be a remap later below:
					remap_source_is_mouse = IsMouseVK(remap_source_vk);
					remap_dest_is_mouse = IsMouseVK(remap_dest_vk);
					remap_keybd_to_mouse = !remap_source_is_mouse && remap_dest_is_mouse;
					snprintf(remap_source, sizeof(remap_source), "%s%s"
						, strlen(cp1) == 1 && IsCharUpper(*cp1) ? "+" : ""  // Allow A::b to be different than a::b.
						, buf); // Include any modifiers too, e.g. ^b::c.
					strlcpy(remap_dest, cp, sizeof(remap_dest));      // But exclude modifiers here; they're wanted separately.
					strlcpy(remap_dest_modifiers, hotkey_flag, sizeof(remap_dest_modifiers));
					if (cp - hotkey_flag < sizeof(remap_dest_modifiers)) // Avoid reading beyond the end.
						remap_dest_modifiers[cp - hotkey_flag] = '\0';   // Terminate at the proper end of the modifier string.
					remap_stage = 0; // Init for use in the next stage.
					// In the unlikely event that the dest key has the same name as a command, disqualify it
					// from being a remap (as documented).  v1.0.40.05: If the destination key has any modifiers,
					// it is unambiguously a key name rather than a command, so the switch() isn't necessary.
					if (*remap_dest_modifiers)
						goto continue_main_loop; // It will see that remap_dest_vk is non-zero and act accordingly.
					switch (remap_dest_vk)
					{
					case VK_CONTROL: // Checked in case it was specified as "Control" rather than "Ctrl".
					case VK_SLEEP:
						if (StrChrAny(hotkey_flag, " \t,")) // Not using g_delimiter (reduces code size/complexity).
							break; // Any space, tab, or enter means this is a command rather than a remap destination.
						goto continue_main_loop; // It will see that remap_dest_vk is non-zero and act accordingly.
					// "Return" and "Pause" as destination keys are always considered commands instead.
					// This is documented and is done to preserve backward compatibility.
					case VK_RETURN:
						// v1.0.40.05: Although "Return" can't be a destination, "Enter" can be.  Must compare
						// to "Return" not "Enter" so that things like "vk0d" (the VK of "Enter") can also be a
						// destination key:
						if (!stricmp(remap_dest, "Return"))
							break;
						goto continue_main_loop; // It will see that remap_dest_vk is non-zero and act accordingly.
					case VK_PAUSE:  // Used for both "Pause" and "Break"
						break;
					default: // All other VKs are valid destinations and thus the remap is valid.
						goto continue_main_loop; // It will see that remap_dest_vk is non-zero and act accordingly.
					}
					// Since above didn't goto, indicate that this is not a remap after all:
					remap_dest_vk = 0;
				}
			}
			// else don't trim hotstrings since literal spaces in both substrings are significant.

			// If this is the first hotkey label encountered, Add a return before
			// adding the label, so that the auto-exectute section is terminated.
			// Only do this if the label is a hotkey because, for example,
			// the user may want to fully execute a normal script that contains
			// no hotkeys but does contain normal labels to which the execution
			// should fall through, if specified, rather than returning.
			// But this might result in weirdness?  Example:
			//testlabel:
			// Sleep, 1
			// return
			// ^a::
			// return
			// It would put the hard return in between, which is wrong.  But in the case above,
			// the first sub shouldn't have a return unless there's a part up top that ends in Exit.
			// So if Exit is encountered before the first hotkey, don't add the return?
			// Even though wrong, the return is harmless because it's never executed?  Except when
			// falling through from above into a hotkey (which probably isn't very valid anyway)?
			// Update: Below must check if there are any true hotkey labels, not just regular labels.
			// Otherwise, a normal (non-hotkey) label in the autoexecute section would count and
			// thus the RETURN would never be added here, even though it should be:

			// Notes about the below macro:
			// Fix for v1.0.34: Don't point labels to this particular RETURN so that labels
			// can point to the very first hotkey or hotstring in a script.  For example:
			// Goto Test
			// Test:
			// ^!z::ToolTip Without the fix`, this is never displayed by "Goto Test".
			// UCHAR_MAX signals it not to point any pending labels to this RETURN.
			// mCurrLine = NULL -> signifies that we're in transition, trying to load a new one.
			#define CHECK_mNoHotkeyLabels \
			if (mNoHotkeyLabels)\
			{\
				mNoHotkeyLabels = false;\
				if (!AddLine(ACT_RETURN, NULL, UCHAR_MAX))\
					return CloseAndReturn(fp, script_buf, FAIL);\
				mCurrLine = NULL;\
			}
			CHECK_mNoHotkeyLabels
			// For hotstrings, the below makes the label include leading colon(s) and the full option
			// string (if any) so that the uniqueness of labels is preserved.  For example, we want
			// the following two hotstring labels to be unique rather than considered duplicates:
			// ::abc::
			// :c:abc::
			if (!AddLabel(buf, true)) // Always add a label before adding the first line of its section.
				return CloseAndReturn(fp, script_buf, FAIL);
			hook_action = 0; // Set default.
			if (*hotkey_flag) // This hotkey's action is on the same line as its label.
			{
				if (!hotstring_start)
					// Don't add the alt-tabs as a line, since it has no meaning as a script command.
					// But do put in the Return regardless, in case this label is ever jumped to
					// via Goto/Gosub:
					if (   !(hook_action = Hotkey::ConvertAltTab(hotkey_flag, false))   )
						if (!ParseAndAddLine(hotkey_flag, IsFunction(hotkey_flag) ? ACT_EXPRESSION : ACT_INVALID)) // It can't be a function definition vs. call since it's a single-line hotkey.
							return CloseAndReturn(fp, script_buf, FAIL);
				// Also add a Return that's implicit for a single-line hotkey.  This is also
				// done for auto-replace hotstrings in case gosub/goto is ever used to jump
				// to their labels:
				if (!AddLine(ACT_RETURN))
					return CloseAndReturn(fp, script_buf, FAIL);
			}

			if (hotstring_start)
			{
				if (!*hotstring_start)
				{
					// The following error message won't indicate the correct line number because
					// the hotstring (as a label) does not actually exist as a line.  But it seems
					// best to report it this way in case the hotstring is inside a #Include file,
					// so that the correct file name and approximate line number are shown:
					ScriptError("This hotstring is missing its abbreviation.", buf); // Display buf vs. hotkey_flag in case the line is simply "::::".
					return CloseAndReturn(fp, script_buf, FAIL);
				}
				// In the case of hotstrings, hotstring_start is the beginning of the hotstring itself,
				// i.e. the character after the second colon.  hotstring_options is NULL if no options,
				// otherwise it's the first character in the options list (option string is not terminated,
				// but instead ends in a colon).  hotkey_flag is blank if it's not an auto-replace
				// hotstring, otherwise it contains the auto-replace text.
				// v1.0.42: Unlike hotkeys, duplicate hotstrings are not detected.  This is because
				// hotstrings are less commonly used and also because it requires more code to find
				// hotstring duplicates (and performs a lot worse if a script has thousands of
				// hotstrings) because of all the hotstring options.
				if (!Hotstring::AddHotstring(mLastLabel, const_cast<char*>(hotstring_options ? hotstring_options : "")
					, hotstring_start, hotkey_flag, has_continuation_section))
					return CloseAndReturn(fp, script_buf, FAIL);
			}
			else // It's a hotkey vs. hotstring.
			{
				if (hk = Hotkey::FindHotkeyByTrueNature(buf, suffix_has_tilde)) // Parent hotkey found.  Add a child/variant hotkey for it.
				{
					if (hook_action) // suffix_has_tilde has always been ignored for these types (alt-tab hotkeys).
					{
						// Hotkey::Dynamic() contains logic and comments similar to this, so maintain them together.
						// An attempt to add an alt-tab variant to an existing hotkey.  This might have
						// merit if the intention is to make it alt-tab now but to later disable that alt-tab
						// aspect via the Hotkey cmd to let the context-sensitive variants shine through
						// (take effect).
						hk->mHookAction = hook_action;
					}
					else
					{
						// Detect duplicate hotkey variants to help spot bugs in scripts.
						if (hk->FindVariant()) // See if there's already a variant matching the current criteria (suffix_has_tilde does not make variants distinct form each other because it would require firing two hotkey IDs in response to pressing one hotkey, which currently isn't in the design).
						{
							mCurrLine = NULL;  // Prevents showing unhelpful vicinity lines.
							ScriptError("Duplicate hotkey.", buf);
							return CloseAndReturn(fp, script_buf, FAIL);
						}
						if (!hk->AddVariant(mLastLabel, suffix_has_tilde))
						{
							ScriptError(ERR_OUTOFMEM, buf);
							return CloseAndReturn(fp, script_buf, FAIL);
						}
					}
				}
				else // No parent hotkey yet, so create it.
					if (   !(hk = Hotkey::AddHotkey(mLastLabel, hook_action, NULL, suffix_has_tilde, false))   )
						return CloseAndReturn(fp, script_buf, FAIL); // It already displayed the error.
			}
			goto continue_main_loop; // In lieu of "continue", for performance.
		} // if (is_label = ...)

		// Otherwise, not a hotkey or hotstring.  Check if it's a generic, non-hotkey label:
		if (buf[buf_length - 1] == ':') // Labels must end in a colon (buf was previously rtrimmed).
		{
			if (buf_length == 1) // v1.0.41.01: Properly handle the fact that this line consists of only a colon.
			{
				ScriptError(ERR_UNRECOGNIZED_ACTION, buf);
				return CloseAndReturn(fp, script_buf, FAIL);
			}
			// Labels (except hotkeys) must contain no whitespace, delimiters, or escape-chars.
			// This is to avoid problems where a legitimate action-line ends in a colon,
			// such as "WinActivate SomeTitle" and "#Include c:".
			// We allow hotkeys to violate this since they may contain commas, and since a normal
			// script line (i.e. just a plain command) is unlikely to ever end in a double-colon:
			for (cp = buf, is_label = true; *cp; ++cp)
				if (IS_SPACE_OR_TAB(*cp) || *cp == g_delimiter || *cp == g_EscapeChar)
				{
					is_label = false;
					break;
				}
			if (is_label) // It's a generic, non-hotkey/non-hotstring label.
			{
				// v1.0.44.04: Fixed this check by moving it after the above loop.
				// Above has ensured buf_length>1, so it's safe to check for double-colon:
				// v1.0.44.03: Don't allow anything that ends in "::" (other than a line consisting only
				// of "::") to be a normal label.  Assume it's a command instead (if it actually isn't, a
				// later stage will report it as "invalid hotkey"). This change avoids the situation in
				// which a hotkey like ^!�:: is seen as invalid because the current keyboard layout doesn't
				// have a "�" key. Without this change, if such a hotkey appears at the top of the script,
				// its subroutine would execute immediately as a normal label, which would be especially
				// bad if the hotkey were something like the "Shutdown" command.
				if (buf[buf_length - 2] == ':' && buf_length > 2) // i.e. allow "::" as a normal label, but consider anything else with double-colon to be a failed-hotkey label that terminates the auto-exec section.
				{
					CHECK_mNoHotkeyLabels // Terminate the auto-execute section since this is a failed hotkey vs. a mere normal label.
					snprintf(msg_text, sizeof(msg_text), "Note: The hotkey %s will not be active because it does not exist in the current keyboard layout.", buf);
					MsgBox(msg_text);
				}
				buf[--buf_length] = '\0';  // Remove the trailing colon.
				rtrim(buf, buf_length); // Has already been ltrimmed.
				if (!AddLabel(buf, false))
					return CloseAndReturn(fp, script_buf, FAIL);
				goto continue_main_loop; // In lieu of "continue", for performance.
			}
		}
		// Since above didn't "goto", it's not a label.
		if (*buf == '#')
		{
			saved_line_number = mCombinedLineNumber; // Backup in case IsDirective() processes an include file, which would change mCombinedLineNumber's value.
			switch(IsDirective(buf)) // Note that it may alter the contents of buf, at least in the case of #IfWin.
			{
			case CONDITION_TRUE:
				// Since the directive may have been a #include which called us recursively,
				// restore the class's values for these two, which are maintained separately
				// like this to avoid having to specify them in various calls, especially the
				// hundreds of calls to ScriptError() and LineError():
				mCurrFileIndex = source_file_index;
				mCombinedLineNumber = saved_line_number;
				goto continue_main_loop; // In lieu of "continue", for performance.
			case FAIL: // IsDirective() already displayed the error.
				return CloseAndReturn(fp, script_buf, FAIL);
			//case CONDITION_FALSE: Do nothing; let processing below handle it.
			}
		}
		// Otherwise, treat it as a normal script line.

		// v1.0.41: Support the "} else {" style in one-true-brace (OTB).  As a side-effect,
		// any command, not just an else, is probably supported to the right of '}', not just "else".
		// This is undocumented because it would make for less readable scripts, and doesn't seem
		// to have much value.
		if (*buf == '}')
		{
			if (!AddLine(ACT_BLOCK_END))
				return CloseAndReturn(fp, script_buf, FAIL);
			// The following allows the next stage to see "else" or "else {" if it's present:
			if (   !*(buf = omit_leading_whitespace(buf + 1))   )
				goto continue_main_loop; // It's just a naked "}", so no more processing needed for this line.
			buf_length = strlen(buf); // Update for possible use below.
		}
		// First do a little special handling to support actions on the same line as their
		// ELSE, e.g.:
		// else if x = 1
		// This is done here rather than in ParseAndAddLine() because it's fairly
		// complicated to do there (already tried it) mostly due to the fact that
		// literal_map has to be properly passed in a recursive call to itself, as well
		// as properly detecting special commands that don't have keywords such as
		// IF comparisons, ACT_ASSIGN, +=, -=, etc.
		// v1.0.41: '{' was added to the line below to support no spaces inside "}else{".
		if (!(action_end = StrChrAny(buf, "\t ,{"))) // Position of first tab/space/comma/open-brace.  For simplicitly, a non-standard g_delimiter is not supported.
			action_end = buf + buf_length; // It's done this way so that ELSE can be fully handled here; i.e. that ELSE does not have to be in the list of commands recognizable by ParseAndAddLine().
		// The following method ensures that words or variables that start with "Else", e.g. ElseAction, are not
		// incorrectly detected as an Else command:
		if (strlicmp(buf, "Else", (UINT)(action_end - buf))) // It's not an ELSE. ("Else" is used vs. g_act[ACT_ELSE].Name for performance).
		{
			// It's not an ELSE.  Also, at this stage it can't be ACT_EXPRESSION (such as an isolated function call)
			// because it would have been already handled higher above.
			// v1.0.41.01: Check if there is a command/action on the same line as the '{'.  This is apparently
			// a style that some people use, and it also supports "{}" as a shorthand way of writing an empty block.
			if (*buf == '{')
			{
				if (!AddLine(ACT_BLOCK_BEGIN))
					return CloseAndReturn(fp, script_buf, FAIL);
				if (   *(action_end = omit_leading_whitespace(buf + 1))   )  // There is an action to the right of the '{'.
				{
					mCurrLine = NULL;  // To signify that we're in transition, trying to load a new one.
					if (!ParseAndAddLine(action_end, IsFunction(action_end) ? ACT_EXPRESSION : ACT_INVALID)) // If it's a function, it must be a call vs. a definition because a function can't be defined on the same line as an open-brace.
						return CloseAndReturn(fp, script_buf, FAIL);
				}
				// Otherwise, there was either no same-line action or the same-line action was successfully added,
				// so do nothing.
			}
			else
				if (!ParseAndAddLine(buf))
					return CloseAndReturn(fp, script_buf, FAIL);
		}
		else // This line is an ELSE, possibly with another command immediately after it (on the same line).
		{
			// Add the ELSE directly rather than calling ParseAndAddLine() because that function
			// would resolve escape sequences throughout the entire length of <buf>, which we
			// don't want because we wouldn't have access to the corresponding literal-map to
			// figure out the proper use of escaped characters:
			if (!AddLine(ACT_ELSE))
				return CloseAndReturn(fp, script_buf, FAIL);
			mCurrLine = NULL;  // To signify that we're in transition, trying to load a new one.
			action_end = omit_leading_whitespace(action_end); // Now action_end is the word after the ELSE.
			if (*action_end == g_delimiter) // Allow "else, action"
				action_end = omit_leading_whitespace(action_end + 1);
			if (*action_end && !ParseAndAddLine(action_end, IsFunction(action_end) ? ACT_EXPRESSION : ACT_INVALID)) // If it's a function, it must be a call vs. a definition because a function can't be defined on the same line as an Else.
				return CloseAndReturn(fp, script_buf, FAIL);
			// Otherwise, there was either no same-line action or the same-line action was successfully added,
			// so do nothing.
		}

continue_main_loop: // This method is used in lieu of "continue" for performance and code size reduction.
		if (remap_dest_vk)
		{
			// For remapping, decided to use a "macro expansion" approach because I think it's considerably
			// smaller in code size and complexity than other approaches would be.  I originally wanted to
			// do it with the hook by changing the incoming event prior to passing it back out again (for
			// example, a::b would transform an incoming 'a' keystroke into 'b' directly without having
			// to suppress the original keystroke and simulate a new one).  Unfortunately, the low-level
			// hooks apparently do not allow this.  Here is the test that confirmed it:
			// if (event.vkCode == 'A')
			// {
			//	event.vkCode = 'B';
			//	event.scanCode = 0x30; // Or use vk_to_sc(event.vkCode).
			//	return CallNextHookEx(g_KeybdHook, aCode, wParam, lParam);
			// }
			switch (++remap_stage)
			{
			case 1: // Stage 1: Add key-down hotkey label, e.g. *LButton::
				buf_length = sprintf(buf, "*%s::", remap_source); // Should be no risk of buffer overflow due to prior validation.
				goto examine_line; // Have the main loop process the contents of "buf" as though it came in from the script.
			case 2: // Stage 2.
				// Copied into a writable buffer for maintainability: AddLine() might rely on this.
				// Also, it seems unnecessary to set press-duration to -1 even though the auto-exec section might
				// have set it to something higher than -1 because:
				// 1) Press-duration doesn't apply to normal remappings since they use down-only and up-only events.
				// 2) Although it does apply to remappings such as a::B and a::^b (due to press-duration being
				//    applied after a change to modifier state), those remappings are fairly rare and supporting
				//    a non-negative-one press-duration (almost always 0) probably adds a degree of flexibility
				//    that may be desirable to keep.
				// 3) SendInput may become the predominant SendMode, so press-duration won't often be in effect anyway.
				// 4) It has been documented that remappings use the auto-execute section's press-duration.
				strcpy(buf, "-1"); // Does NOT need to be "-1, -1" for SetKeyDelay (see above).
				// The primary reason for adding Key/MouseDelay -1 is to minimize the chance that a one of
				// these hotkey threads will get buried under some other thread such as a timer, which
				// would disrupt the remapping if #MaxThreadsPerHotkey is at its default of 1.
				AddLine(remap_dest_is_mouse ? ACT_SETMOUSEDELAY : ACT_SETKEYDELAY, &buf, 1, NULL); // PressDuration doesn't need to be specified because it doesn't affect down-only and up-only events.
				if (remap_keybd_to_mouse)
				{
					// Since source is keybd and dest is mouse, prevent keyboard auto-repeat from auto-repeating
					// the mouse button (since that would be undesirable 90% of the time).  This is done
					// by inserting a single extra IF-statement above the Send that produces the down-event:
					buf_length = sprintf(buf, "if not GetKeyState(\"%s\")", remap_dest); // Should be no risk of buffer overflow due to prior validation.
					remap_stage = 9; // Have it hit special stage 9+1 next time for code reduction purposes.
					goto examine_line; // Have the main loop process the contents of "buf" as though it came in from the script.
				}
				// Otherwise, remap_keybd_to_mouse==false, so fall through to next case.
			case 10:
				extra_event = ""; // Set default.
				switch (remap_dest_vk)
				{
				case VK_LMENU:
				case VK_RMENU:
				case VK_MENU:
					switch (remap_source_vk)
					{
					case VK_LCONTROL:
					case VK_CONTROL:
						extra_event = "{LCtrl up}"; // Somewhat surprisingly, this is enough to make "Ctrl::Alt" properly remap both right and left control.
						break;
					case VK_RCONTROL:
						extra_event = "{RCtrl up}";
						break;
					// Below is commented out because its only purpose was to allow a shift key remapped to alt
					// to be able to alt-tab.  But that wouldn't work correctly due to the need for the following
					// hotkey, which does more harm than good by impacting the normal Alt key's ability to alt-tab
					// (if the normal Alt key isn't remapped): *Tab::Send {Blind}{Tab}
					//case VK_LSHIFT:
					//case VK_SHIFT:
					//	extra_event = "{LShift up}";
					//	break;
					//case VK_RSHIFT:
					//	extra_event = "{RShift up}";
					//	break;
					}
					break;
				}
				mCurrLine = NULL; // v1.0.40.04: Prevents showing misleading vicinity lines for a syntax-error such as %::%
				sprintf(buf, "{Blind}%s%s{%s DownTemp}", extra_event, remap_dest_modifiers, remap_dest); // v1.0.44.05: DownTemp vs. Down. See Send's DownTemp handler for details.
				if (!AddLine(ACT_SEND, &buf, 1, NULL)) // v1.0.40.04: Check for failure due to bad remaps such as %::%.
					return CloseAndReturn(fp, script_buf, FAIL);
				AddLine(ACT_RETURN);
				// Add key-up hotkey label, e.g. *LButton up::
				buf_length = sprintf(buf, "*%s up::", remap_source); // Should be no risk of buffer overflow due to prior validation.
				remap_stage = 2; // Adjust to hit stage 3 next time (in case this is stage 10).
				goto examine_line; // Have the main loop process the contents of "buf" as though it came in from the script.
			case 3: // Stage 3.
				strcpy(buf, "-1");
				AddLine(remap_dest_is_mouse ? ACT_SETMOUSEDELAY : ACT_SETKEYDELAY, &buf, 1, NULL);
				sprintf(buf, "{Blind}{%s Up}", remap_dest); // Unlike the down-event above, remap_dest_modifiers is not included for the up-event; e.g. ^{b up} is inappropriate.
				AddLine(ACT_SEND, &buf, 1, NULL);
				AddLine(ACT_RETURN);
				remap_dest_vk = 0; // Reset to signal that the remapping expansion is now complete.
				break; // Fall through to the next section so that script loading can resume at the next line.
			}
		} // if (remap_dest_vk)
		// Since above didn't "continue", resume loading script line by line:
		buf = next_buf;
		buf_length = next_buf_length;
		next_buf = (buf == buf1) ? buf2 : buf1;
		// The line above alternates buffers (toggles next_buf to be the unused buffer), which helps
		// performance because it avoids memcpy from buf2 to buf1.
	} // for each whole/constructed line.

	if (*pending_function) // Since this is the last non-comment line, the pending function must be a function call, not a function definition.
	{
		// Somewhat messy to decrement then increment later, but it's probably easier than the
		// alternatives due to the use of "continue" in some places above.
		saved_line_number = mCombinedLineNumber;
		mCombinedLineNumber = pending_function_line_number; // Done so that any syntax errors that occur during the calls below will report the correct line number.
		if (!ParseAndAddLine(pending_function, ACT_EXPRESSION)) // Must be function call vs. definition since otherwise the above would have detected the opening brace beneath it and already cleared pending_function.
			return CloseAndReturn(fp, script_buf, FAIL);
		mCombinedLineNumber = saved_line_number;
	}

#ifdef AUTOHOTKEYSC
	free(script_buf); // AutoIt3: Close the archive and free the file in memory.
	oRead.Close();    //
#else
	fclose(fp);
#endif
	return OK;
}



// Small inline to make LoadIncludedFile() code cleaner.
#ifdef AUTOHOTKEYSC
inline ResultType Script::CloseAndReturn(HS_EXEArc_Read *fp, UCHAR *aBuf, ResultType aReturnValue)
{
	free(aBuf);
	fp->Close();
	return aReturnValue;
}
#else
inline ResultType Script::CloseAndReturn(FILE *fp, UCHAR *aBuf, ResultType aReturnValue)
{
	// aBuf is unused in this case.
	fclose(fp);
	return aReturnValue;
}
#endif



#ifdef AUTOHOTKEYSC
size_t Script::GetLine(char *aBuf, int aMaxCharsToRead, int aInContinuationSection, UCHAR *&aMemFile) // last param = reference to pointer
#else
size_t Script::GetLine(char *aBuf, int aMaxCharsToRead, int aInContinuationSection, FILE *fp)
#endif
{
	size_t aBuf_length = 0;
#ifdef AUTOHOTKEYSC
	if (!aBuf || !aMemFile) return -1;
	if (aMaxCharsToRead < 1) return -1; // We're signaling to caller that the end of the memory file has been reached.
	// Otherwise, continue reading characters from the memory file until either a newline is
	// reached or aMaxCharsToRead have been read:
	// Track "i" separately from aBuf_length because we want to read beyond the bounds of the memory file.
	int i;
	for (i = 0; i < aMaxCharsToRead; ++i)
	{
		if (aMemFile[i] == '\n')
		{
			// The end of this line has been reached.  Don't copy this char into the target buffer.
			// In addition, if the previous char was '\r', remove it from the target buffer:
			if (aBuf_length > 0 && aBuf[aBuf_length - 1] == '\r')
				aBuf[--aBuf_length] = '\0';
			++i; // i.e. so that aMemFile will be adjusted to omit this newline char.
			break;
		}
		else
			aBuf[aBuf_length++] = aMemFile[i];
	}
	// We either read aMaxCharsToRead or reached the end of the line (as indicated by the newline char).
	// In the former case, aMemFile might now be changed to be a position outside the bounds of the
	// memory area, which the caller will reflect back to us during the next call as a 0 value for
	// aMaxCharsToRead, which we then signal to the caller (above) as the end of the file):
	aMemFile += i; // Update this value for use by the caller.
	// Terminate the buffer (the caller has already ensured that there's room for the terminator
	// via its value of aMaxCharsToRead):
	aBuf[aBuf_length] = '\0';
#else
	if (!aBuf || !fp) return -1;
	if (aMaxCharsToRead < 1) return 0;
	if (feof(fp)) return -1; // Previous call to this function probably already read the last line.
	if (fgets(aBuf, aMaxCharsToRead, fp) == NULL) // end-of-file or error
	{
		*aBuf = '\0';  // Reset since on error, contents added by fgets() are indeterminate.
		return -1;
	}
	aBuf_length = strlen(aBuf);
	if (!aBuf_length)
		return 0;
	if (aBuf[aBuf_length-1] == '\n')
		aBuf[--aBuf_length] = '\0';
	if (aBuf[aBuf_length-1] == '\r')  // In case there are any, e.g. a Macintosh or Unix file?
		aBuf[--aBuf_length] = '\0';
#endif

	if (aInContinuationSection)
	{
		char *cp = omit_leading_whitespace(aBuf);
		if (aInContinuationSection == CONTINUATION_SECTION_WITHOUT_COMMENTS) // By default, continuation sections don't allow comments (lines beginning with a semicolon are treated as literal text).
		{
			// Caller relies on us to detect the end of the continuation section so that trimming
			// will be done on the final line of the section and so that a comment can immediately
			// follow the closing parenthesis (on the same line).  Example:
			// (
			//	Text
			// ) ; Same line comment.
			if (*cp != ')') // This isn't the last line of the continuation section, so leave the line untrimmed (caller will apply the ltrim setting on its own).
				return aBuf_length; // Earlier sections are responsible for keeping aBufLength up-to-date with any changes to aBuf.
			//else this line starts with ')', so continue on to later section that checks for a same-line comment on its right side.
		}
		else // aInContinuationSection == CONTINUATION_SECTION_WITH_COMMENTS (i.e. comments are allowed in this continuation section).
		{
			// Fix for v1.0.46.09+: The "com" option shouldn't put "ltrim" into effect.
			if (!strncmp(cp, g_CommentFlag, g_CommentFlagLength)) // Case sensitive.
			{
				*aBuf = '\0'; // Since this line is a comment, have the caller ignore it.
				return -2; // Callers tolerate -2 only when in a continuation section.  -2 indicates, "don't include this line at all, not even as a blank line to which the JOIN string (default "\n") will apply.
			}
			if (*cp == ')') // This isn't the last line of the continuation section, so leave the line untrimmed (caller will apply the ltrim setting on its own).
			{
				ltrim(aBuf); // Ltrim this line unconditionally so that caller will see that it starts with ')' without having to do extra steps.
				aBuf_length = strlen(aBuf); // ltrim() doesn't always return an accurate length, so do it this way.
			}
		}
	}
	// Since above didn't return, either:
	// 1) We're not in a continuation section at all, so apply ltrim() to support semicolons after tabs or
	//    other whitespace.  Seems best to rtrim also.
	// 2) CONTINUATION_SECTION_WITHOUT_COMMENTS but this line is the final line of the section.  Apply
	//    trim() and other logic further below because caller might rely on it.
	// 3) CONTINUATION_SECTION_WITH_COMMENTS (i.e. comments allowed), but this line isn't a comment (though
	//    it may start with ')' and thus be the final line of this section). In either case, need to check
	//    for same-line comments further below.
	if (aInContinuationSection != CONTINUATION_SECTION_WITH_COMMENTS) // Case #1 & #2 above.
	{
		aBuf_length = trim(aBuf);
		if (!strncmp(aBuf, g_CommentFlag, g_CommentFlagLength)) // Case sensitive.
		{
			// Due to other checks, aInContinuationSection==false whenever the above condition is true.
			*aBuf = '\0';
			return 0;
		}
	}
	//else CONTINUATION_SECTION_WITH_COMMENTS (case #3 above), which due to other checking also means that
	// this line isn't a comment (though it might have a comment on its right side, which is checked below).
	// CONTINUATION_SECTION_WITHOUT_COMMENTS would already have returned higher above if this line isn't
	// the last line of the continuation section.
	if (g_AllowSameLineComments)
	{
		// Handle comment-flags that appear to the right of a valid line.  But don't
		// allow these types of comments if the script is considers to be the AutoIt2
		// style, to improve compatibility with old scripts that may use non-escaped
		// comment-flags as literal characters rather than comments:
		char *cp, *prevp;
		for (cp = strstr(aBuf, g_CommentFlag); cp; cp = strstr(cp + g_CommentFlagLength, g_CommentFlag))
		{
			// If no whitespace to its left, it's not a valid comment.
			// We insist on this so that a semi-colon (for example) immediately after
			// a word (as semi-colons are often used) will not be considered a comment.
			prevp = cp - 1;
			if (prevp < aBuf) // should never happen because we already checked above.
			{
				*aBuf = '\0';
				return 0;
			}
			if (IS_SPACE_OR_TAB_OR_NBSP(*prevp)) // consider it to be a valid comment flag
			{
				*prevp = '\0';
				aBuf_length = rtrim_with_nbsp(aBuf, prevp - aBuf); // Since it's our responsibility to return a fully trimmed string.
				break; // Once the first valid comment-flag is found, nothing after it can matter.
			}
			else // No whitespace to the left.
				if (*prevp == g_EscapeChar) // Remove the escape char.
				{
					// The following isn't exactly correct because it prevents an include filename from ever
					// containing the literal string "`;".  This is because attempts to escape the accent via
					// "``;" are not supported.  This is documented here as a known limitation because fixing
					// it would probably break existing scripts that rely on the fact that accents do not need
					// to be escaped inside #Include.  Also, the likelihood of "`;" appearing literally in a
					// legitimate #Include file seems vanishingly small.
					memmove(prevp, prevp + 1, strlen(prevp + 1) + 1);  // +1 for the terminator.
					--aBuf_length;
					// Then continue looking for others.
				}
				// else there wasn't any whitespace to its left, so keep looking in case there's
				// another further on in the line.
		} // for()
	} // if (g_AllowSameLineComments)

	return aBuf_length; // The above is responsible for keeping aBufLength up-to-date with any changes to aBuf.
}



inline ResultType Script::IsDirective(char *aBuf)
// aBuf must be a modifiable string since this function modifies it in the case of "#Include %A_ScriptDir%"
// changes it.  It must also be large enough to accept the replacement of %A_ScriptDir% with a larger string.
// Returns CONDITION_TRUE, CONDITION_FALSE, or FAIL.
// Note: Don't assume that every line in the script that starts with '#' is a directive
// because hotkeys can legitimately start with that as well.  i.e., the following line should
// not be unconditionally ignored, just because it starts with '#', since it is a valid hotkey:
// #y::run, notepad
{
	char end_flags[] = {' ', '\t', g_delimiter, '\0'}; // '\0' must be last.
	char *directive_end, *parameter_raw;
	if (   !(directive_end = StrChrAny(aBuf, end_flags))   )
	{
		directive_end = aBuf + strlen(aBuf); // Point it to the zero terminator.
		parameter_raw = NULL;
	}
	else
		if (!*(parameter_raw = omit_leading_whitespace(directive_end)))
			parameter_raw = NULL;

	// The raw parameter retains any leading comma for those directives that need that (none currently).
	// But the following omits that comma:
	char *parameter;
	if (!parameter_raw)
		parameter = NULL;
	else // Since parameter_raw is non-NULL, it's also non-blank and non-whitespace due to the above checking.
		if (*parameter_raw != g_delimiter)
			parameter = parameter_raw;
		else // It's a delimiter, so "parameter" will be whatever non-whitespace character follows it, if any.
			if (!*(parameter = omit_leading_whitespace(parameter_raw + 1)))
				parameter = NULL;
			//else leave it set to the value returned by omit_leading_whitespace().

	int value; // Helps detect values that are too large, since some of the target globals are UCHAR.

	// Use strnicmp() so that a match is found as long as aBuf starts with the string in question.
	// e.g. so that "#SingleInstance, on" will still work too, but
	// "#a::run, something, "#SingleInstance" (i.e. a hotkey) will not be falsely detected
	// due to using a more lenient function such as strcasestr().
	// UPDATE: Using strlicmp() now so that overlapping names, such as #MaxThreads and #MaxThreadsPerHotkey,
	// won't get mixed up:
	#define IS_DIRECTIVE_MATCH(directive) (!strlicmp(aBuf, directive, directive_name_length))
	UINT directive_name_length = (UINT)(directive_end - aBuf); // To avoid calculating it every time in the macro above.

	bool is_include_again = false; // Set default in case of short-circuit boolean.
	if (IS_DIRECTIVE_MATCH("#Include") || (is_include_again = IS_DIRECTIVE_MATCH("#IncludeAgain")))
	{
		// Standalone EXEs ignore this directive since the included files were already merged in
		// with the main file when the script was compiled.  These should have been removed
		// or commented out by Ahk2Exe, but just in case, it's safest to ignore them:
#ifdef AUTOHOTKEYSC
		return CONDITION_TRUE;
#else
		// If the below decision is ever changed, be sure to update ahk2exe with the same change:
		// "parameter" is checked rather than parameter_raw for backward compatibility with earlier versions,
		// in which a leading comma is not considered part of the filename.  Although this behavior is incorrect
		// because it prevents files whose names start with a comma from being included without the first
		// delim-comma being there too, it is kept because filesnames that start with a comma seem
		// exceedingly rare.  As a workaround, the script can do #Include ,,FilenameWithLeadingComma.ahk
		if (!parameter)
			return ScriptError(ERR_PARAM1_REQUIRED, aBuf);
		// v1.0.32:
		bool ignore_load_failure = (parameter[0] == '*' && toupper(parameter[1]) == 'I'); // Relies on short-circuit boolean order.
		if (ignore_load_failure)
		{
			parameter += 2;
			if (IS_SPACE_OR_TAB(*parameter)) // Skip over at most one space or tab, since others might be a literal part of the filename.
				++parameter;
		}

		size_t space_remaining = LINE_SIZE - (parameter-aBuf);
		char buf[MAX_PATH];
		StrReplace(parameter, "%A_ScriptDir%", mFileDir, SCS_INSENSITIVE, 1, space_remaining); // v1.0.35.11.  Caller has ensured string is writable.
		if (strcasestr(parameter, "%A_AppData%")) // v1.0.45.04: This and the next were requested by Tekl to make it easier to customize scripts on a per-user basis.
		{
			BIV_AppData(buf, "A_AppData");
			StrReplace(parameter, "%A_AppData%", buf, SCS_INSENSITIVE, 1, space_remaining);
		}
		if (strcasestr(parameter, "%A_AppDataCommon%")) // v1.0.45.04.
		{
			BIV_AppData(buf, "A_AppDataCommon");
			StrReplace(parameter, "%A_AppDataCommon%", buf, SCS_INSENSITIVE, 1, space_remaining);
		}

		DWORD attr = GetFileAttributes(parameter);
		if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) // File exists and its a directory (possibly A_ScriptDir or A_AppData set above).
		{
			// v1.0.35.11 allow changing of load-time directory to increase flexibility.  This feature has
			// been asked for directly or indirectly several times.
			// If a script ever wants to use a string like "%A_ScriptDir%" literally in an include's filename,
			// that would not work.  But that seems too rare to worry about.
			// v1.0.45.01: Call SetWorkingDir() vs. SetCurrentDirectory() so that it succeeds even for a root
			// drive like C: that lacks a backslash (see SetWorkingDir() for details).
			SetWorkingDir(parameter);
			return CONDITION_TRUE;
		}
		// Since above didn't return, it's a file (or non-existent file, in which case the below will display
		// the error).  This will also display any other errors that occur:
		return LoadIncludedFile(parameter, is_include_again, ignore_load_failure) ? CONDITION_TRUE : FAIL;
#endif
	}

	if (IS_DIRECTIVE_MATCH("#NoEnv"))
	{
		g_NoEnv = true;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#NoTrayIcon"))
	{
		g_NoTrayIcon = true;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#Persistent"))
	{
		g_persistent = true;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#SingleInstance"))
	{
		g_AllowOnlyOneInstance = SINGLE_INSTANCE_PROMPT; // Set default.
		if (parameter)
		{
			if (!stricmp(parameter, "Force"))
				g_AllowOnlyOneInstance = SINGLE_INSTANCE_REPLACE;
			else if (!stricmp(parameter, "Ignore"))
				g_AllowOnlyOneInstance = SINGLE_INSTANCE_IGNORE;
			else if (!stricmp(parameter, "Off"))
				g_AllowOnlyOneInstance = SINGLE_INSTANCE_OFF;
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#InstallKeybdHook"))
	{
		// It seems best not to report this warning because a user may want to use partial functionality
		// of a script on Win9x:
		//MsgBox("#InstallKeybdHook is not supported on Windows 95/98/Me.  This line will be ignored.");
		if (!g_os.IsWin9x())
			Hotkey::RequireHook(HOOK_KEYBD);
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#InstallMouseHook"))
	{
		// It seems best not to report this warning because a user may want to use partial functionality
		// of a script on Win9x:
		//MsgBox("#InstallMouseHook is not supported on Windows 95/98/Me.  This line will be ignored.");
		if (!g_os.IsWin9x())
			Hotkey::RequireHook(HOOK_MOUSE);
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#UseHook"))
	{
		g_ForceKeybdHook = !parameter || Line::ConvertOnOff(parameter) != TOGGLED_OFF;
		return CONDITION_TRUE;
	}

	if (!strnicmp(aBuf, "#IfWin", 6))
	{
		bool invert = !strnicmp(aBuf + 6, "Not", 3);
		if (!strnicmp(aBuf + (invert ? 9 : 6), "Active", 6)) // It matches #IfWin[Not]Active.
			g_HotCriterion = invert ? HOT_IF_NOT_ACTIVE : HOT_IF_ACTIVE;
		else if (!strnicmp(aBuf + (invert ? 9 : 6), "Exist", 5))
			g_HotCriterion = invert ? HOT_IF_NOT_EXIST : HOT_IF_EXIST;
		else // It starts with #IfWin but isn't Active or Exist: Don't alter g_HotCriterion.
			return CONDITION_FALSE; // Indicate unknown directive since there are currently no other possibilities.
		if (!parameter) // The omission of the parameter indicates that any existing criteria should be turned off.
		{
			g_HotCriterion = HOT_NO_CRITERION; // Indicate that no criteria are in effect for subsequent hotkeys.
			g_HotWinTitle = ""; // Helps maintainability and some things might rely on it.
			g_HotWinText = "";  //
			return CONDITION_TRUE;
		}
		char *hot_win_title = parameter, *hot_win_text; // Set default for title; text is determined later.
		// Scan for the first non-escaped comma.  If there is one, it marks the second paramter: WinText.
		char *cp, *first_non_escaped_comma;
		for (first_non_escaped_comma = NULL, cp = hot_win_title; ; ++cp)  // Increment to skip over the symbol just found by the inner for().
		{
			for (; *cp && !(*cp == g_EscapeChar || *cp == g_delimiter || *cp == g_DerefChar); ++cp);  // Find the next escape char, comma, or %.
			if (!*cp) // End of string was found.
				break;
#define ERR_ESCAPED_COMMA_PERCENT "Literal commas and percent signs must be escaped (e.g. `%)"
			if (*cp == g_DerefChar)
				return ScriptError(ERR_ESCAPED_COMMA_PERCENT, aBuf);
			if (*cp == g_delimiter) // non-escaped delimiter was found.
			{
				// Preserve the ability to add future-use parameters such as section of window
				// over which the mouse is hovering, e.g. #IfWinActive, Untitled - Notepad,, TitleBar
				if (first_non_escaped_comma) // A second non-escaped comma was found.
					return ScriptError(ERR_ESCAPED_COMMA_PERCENT, aBuf);
				// Otherwise:
				first_non_escaped_comma = cp;
				continue; // Check if there are any more non-escaped commas.
			}
			// Otherwise, an escape character was found, so skip over the next character (if any).
			if (!*(++cp)) // The string unexpectedly ends in an escape character, so avoid out-of-bounds.
				break;
			// Otherwise, the ++cp above has skipped over the escape-char itself, and the loop's ++cp will now
			// skip over the char-to-be-escaped, which is not the one we want (even if it is a comma).
		}
		if (first_non_escaped_comma) // Above found a non-escaped comma, so there is a second parameter (WinText).
		{
			// Omit whitespace to (seems best to conform to convention/expectations rather than give
			// strange whitespace flexibility that would likely cause unwanted bugs due to inadvertently
			// have two spaces instead of one).  The user may use `s and `t to put literal leading/trailing
			// spaces/tabs into these paramters.
			hot_win_text = omit_leading_whitespace(first_non_escaped_comma + 1);
			*first_non_escaped_comma = '\0'; // Terminate at the comma to split off hot_win_title on its own.
			rtrim(hot_win_title, first_non_escaped_comma - hot_win_title);  // Omit whitespace (see similar comment above).
			// The following must be done only after trimming and omitting whitespace above, so that
			// `s and `t can be used to insert leading/trailing spaces/tabs.  ConvertEscapeSequences()
			// also supports insertion of literal commas via escaped sequences.
			ConvertEscapeSequences(hot_win_text, g_EscapeChar, true);
		}
		else
			hot_win_text = ""; // And leave hot_win_title set to the entire string because there's only one parameter.
		// The following must be done only after trimming and omitting whitespace above (see similar comment above).
		ConvertEscapeSequences(hot_win_title, g_EscapeChar, true);
		// The following also handles the case where both title and text are blank, which could happen
		// due to something weird but legit like: #IfWinActive, ,
		if (!SetGlobalHotTitleText(hot_win_title, hot_win_text))
			return ScriptError(ERR_OUTOFMEM); // So rare that no second param is provided (since its contents may have been temp-terminated or altered above).
		return CONDITION_TRUE;
	} // Above completely handles all directives and non-directives that start with "#IfWin".

	if (IS_DIRECTIVE_MATCH("#Hotstring"))
	{
		if (parameter)
		{
			char *suboption = strcasestr(parameter, "EndChars");
			if (suboption)
			{
				// Since it's not realistic to have only a couple, spaces and literal tabs
				// must be included in between other chars, e.g. `n `t has a space in between.
				// Also, EndChar  \t  will have a space and a tab since there are two spaces
				// after the word EndChar.
				if (    !(parameter = StrChrAny(suboption, "\t "))   )
					return CONDITION_TRUE;
				strlcpy(g_EndChars, ++parameter, sizeof(g_EndChars));
				ConvertEscapeSequences(g_EndChars, g_EscapeChar, false);
				return CONDITION_TRUE;
			}
			if (!strnicmp(parameter, "NoMouse", 7)) // v1.0.42.03
			{
				g_HSResetUponMouseClick = false;
				return CONDITION_TRUE;
			}
			// Otherwise assume it's a list of options.  Note that for compatibility with its
			// other caller, it will stop at end-of-string or ':', whichever comes first.
			Hotstring::ParseOptions(parameter, g_HSPriority, g_HSKeyDelay, g_HSSendMode, g_HSCaseSensitive
				, g_HSConformToCase, g_HSDoBackspace, g_HSOmitEndChar, g_HSSendRaw, g_HSEndCharRequired
				, g_HSDetectWhenInsideWord, g_HSDoReset);
		}
		return CONDITION_TRUE;
	}

	if (IS_DIRECTIVE_MATCH("#HotkeyModifierTimeout"))
	{
		if (parameter)
			g_HotkeyModifierTimeout = ATOI(parameter);  // parameter was set to the right position by the above macro
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#HotkeyInterval"))
	{
		if (parameter)
		{
			g_HotkeyThrottleInterval = ATOI(parameter);  // parameter was set to the right position by the above macro
			if (g_HotkeyThrottleInterval < 10) // values under 10 wouldn't be useful due to timer granularity.
				g_HotkeyThrottleInterval = 10;
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#MaxHotkeysPerInterval"))
	{
		if (parameter)
		{
			g_MaxHotkeysPerInterval = ATOI(parameter);  // parameter was set to the right position by the above macro
			if (g_MaxHotkeysPerInterval < 1) // sanity check
				g_MaxHotkeysPerInterval = 1;
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#MaxThreadsPerHotkey"))
	{
		if (parameter)
		{
			// Use value as a temp holder since it's int vs. UCHAR and can thus detect very large or negative values:
			value = ATOI(parameter);  // parameter was set to the right position by the above macro
			if (value > MAX_THREADS_LIMIT) // For now, keep this limited to prevent stack overflow due to too many pseudo-threads.
				value = MAX_THREADS_LIMIT;
			else if (value < 1)
				value = 1;
			g_MaxThreadsPerHotkey = value; // Note: g_MaxThreadsPerHotkey is UCHAR.
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#MaxThreadsBuffer"))
	{
		g_MaxThreadsBuffer = !parameter || Line::ConvertOnOff(parameter) != TOGGLED_OFF;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#MaxThreads"))
	{
		if (parameter)
		{
			value = ATOI(parameter);  // parameter was set to the right position by the above macro
			if (value > MAX_THREADS_LIMIT) // For now, keep this limited to prevent stack overflow due to too many pseudo-threads.
				value = MAX_THREADS_LIMIT;
			else if (value < 1)
				value = 1;
			g_MaxThreadsTotal = value;
		}
		return CONDITION_TRUE;
	}

	if (IS_DIRECTIVE_MATCH("#ClipboardTimeout"))
	{
		if (parameter)
			g_ClipboardTimeout = ATOI(parameter);  // parameter was set to the right position by the above macro
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#LTrim"))
	{
		g_ContinuationLTrim = !parameter || Line::ConvertOnOff(parameter) != TOGGLED_OFF;
		return CONDITION_TRUE;
	}

	if (IS_DIRECTIVE_MATCH("#WinActivateForce"))
	{
		g_WinActivateForce = true;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#ErrorStdOut"))
	{
		mErrorStdOut = true;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#AllowSameLineComments"))  // i.e. There's no way to turn it off, only on.
	{
		g_AllowSameLineComments = true;
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#MaxMem"))
	{
		if (parameter)
		{
			double valuef = ATOF(parameter);  // parameter was set to the right position by the above macro
			if (valuef > 4095)  // Don't exceed capacity of VarSizeType, which is currently a DWORD (4 gig).
				valuef = 4095;  // Don't use 4096 since that might be a special/reserved value for some functions.
			else if (valuef  < 1)
				valuef = 1;
			g_MaxVarCapacity = (VarSizeType)(valuef * 1024 * 1024);
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#KeyHistory"))
	{
		if (parameter)
		{
			g_MaxHistoryKeys = ATOI(parameter);  // parameter was set to the right position by the above macro
			if (g_MaxHistoryKeys < 0)
				g_MaxHistoryKeys = 0;
			else if (g_MaxHistoryKeys > 500)
				g_MaxHistoryKeys = 500;
			// Above: There are two reasons for limiting the history file to 500 keystrokes:
			// 1) GetHookStatus() only has a limited size buffer in which to transcribe the keystrokes.
			//    500 events is about what you would expect to fit in a 32 KB buffer (it the unlikely event
			//    that the transcribed events create too much text, the text will be truncated, so it's
			//    not dangerous anyway).
			// 2) To reduce the impression that AutoHotkey designed for key logging (the key history file
			//    is in a very unfriendly format that type of key logging anyway).
		}
		return CONDITION_TRUE;
	}

	// For the below series, it seems okay to allow the comment flag to contain other reserved chars,
	// such as DerefChar, since comments are evaluated, and then taken out of the game at an earlier
	// stage than DerefChar and the other special chars.
	if (IS_DIRECTIVE_MATCH("#CommentFlag"))
	{
		if (parameter)
		{
			if (!*(parameter + 1))  // i.e. the length is 1
			{
				// Don't allow '#' since it's the preprocessor directive symbol being used here.
				// Seems ok to allow "." to be the comment flag, since other constraints mandate
				// that at least one space or tab occur to its left for it to be considered a
				// comment marker.
				if (*parameter == '#' || *parameter == g_DerefChar || *parameter == g_EscapeChar || *parameter == g_delimiter)
					return ScriptError(ERR_PARAM1_INVALID, aBuf);
				// Exclude hotkey definition chars, such as ^ and !, because otherwise
				// the following example wouldn't work:
				// User defines ! as the comment flag.
				// The following hotkey would never be in effect since it's considered to
				// be commented out:
				// !^a::run,notepad
				if (*parameter == '!' || *parameter == '^' || *parameter == '+' || *parameter == '$' || *parameter == '~' || *parameter == '*'
					|| *parameter == '<' || *parameter == '>')
					// Note that '#' is already covered by the other stmt. above.
					return ScriptError(ERR_PARAM1_INVALID, aBuf);
			}
			strlcpy(g_CommentFlag, parameter, MAX_COMMENT_FLAG_LENGTH + 1);
			g_CommentFlagLength = strlen(g_CommentFlag);  // Keep this in sync with above.
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#EscapeChar"))
	{
		if (parameter)
		{
			// Don't allow '.' since that can be part of literal floating point numbers:
			if (   *parameter == '#' || *parameter == g_DerefChar || *parameter == g_delimiter || *parameter == '.'
				|| (g_CommentFlagLength == 1 && *parameter == *g_CommentFlag)   )
				return ScriptError(ERR_PARAM1_INVALID, aBuf);
			g_EscapeChar = *parameter;
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#DerefChar"))
	{
		if (parameter)
		{
			if (   *parameter == g_EscapeChar || *parameter == g_delimiter || *parameter == '.'
				|| (g_CommentFlagLength == 1 && *parameter == *g_CommentFlag)   ) // Fix for v1.0.47.05: Allow deref char to be # as documented.
				return ScriptError(ERR_PARAM1_INVALID, aBuf);
			g_DerefChar = *parameter;
		}
		return CONDITION_TRUE;
	}
	if (IS_DIRECTIVE_MATCH("#Delimiter"))
	{
		// Attempts to change the delimiter to its starting default (comma) are ignored.
		// For example, "#Delimiter ," isn't meaningful if the delimiter already is a comma,
		// which is good because "parameter" has already assumed that the comma is accidental
		// (not a symbol) and omitted it.
		if (parameter)
		{
			if (   *parameter == '#' || *parameter == g_EscapeChar || *parameter == g_DerefChar || *parameter == '.'
				|| (g_CommentFlagLength == 1 && *parameter == *g_CommentFlag)   )
				return ScriptError(ERR_PARAM1_INVALID, aBuf);
			g_delimiter = *parameter;
		}
		return CONDITION_TRUE;
	}

	// Otherwise, report that this line isn't a directive:
	return CONDITION_FALSE;
}



void ScriptTimer::Disable()
{
	mEnabled = false;
	--g_script.mTimerEnabledCount;
	if (!g_script.mTimerEnabledCount && !g_nLayersNeedingTimer && !Hotkey::sJoyHotkeyCount)
		KILL_MAIN_TIMER
	// Above: If there are now no enabled timed subroutines, kill the main timer since there's no other
	// reason for it to exist if we're here.   This is because or direct or indirect caller is
	// currently always ExecUntil(), which doesn't need the timer while its running except to
	// support timed subroutines.  UPDATE: The above is faulty; Must also check g_nLayersNeedingTimer
	// because our caller can be one that still needs a timer as proven by this script that
	// hangs otherwise:
	//SetTimer, Test, on
	//Sleep, 1000
	//msgbox, done
	//return
	//Test:
	//SetTimer, Test, off
	//return
}



ResultType Script::UpdateOrCreateTimer(Label *aLabel, char *aPeriod, char *aPriority, bool aEnable
	, bool aUpdatePriorityOnly)
// Caller should specific a blank aPeriod to prevent the timer's period from being changed
// (i.e. if caller just wants to turn on or off an existing timer).  But if it does this
// for a non-existent timer, that timer will be created with the default period as specfied in
// the constructor.
{
	ScriptTimer *timer;
	for (timer = mFirstTimer; timer != NULL; timer = timer->mNextTimer)
		if (timer->mLabel == aLabel) // Match found.
			break;
	bool timer_existed = (timer != NULL);
	if (!timer_existed)  // Create it.
	{
		if (   !(timer = new ScriptTimer(aLabel))   )
			return ScriptError(ERR_OUTOFMEM);
		if (!mFirstTimer)
			mFirstTimer = mLastTimer = timer;
		else
		{
			mLastTimer->mNextTimer = timer;
			// This must be done after the above:
			mLastTimer = timer;
		}
		++mTimerCount;
	}
	// Update its members:
	if (aEnable && !timer->mEnabled) // Must check both or the mTimerEnabledCount below will be wrong.
	{
		// The exception is if the timer already existed but the caller only wanted its priority changed:
		if (!(timer_existed && aUpdatePriorityOnly))
		{
			timer->mEnabled = true;
			++mTimerEnabledCount;
			SET_MAIN_TIMER  // Ensure the API timer is always running when there is at least one enabled timed subroutine.
		}
		//else do nothing, leave it disabled.
	}
	else if (!aEnable && timer->mEnabled) // Must check both or the below count will be wrong.
		timer->Disable();

	if (*aPeriod) // Caller wanted us to update this member.
	{
		__int64 period = ATOI64(aPeriod);
		if (period < 0) // v1.0.46.16: Support negative periods to mean "run only once".
		{
			timer->mRunOnlyOnce = true;
			timer->mPeriod = (DWORD)-period;
		}
		else // Positive number.  v1.0.36.33: Changed from int to DWORD, and ATOI to ATOU, to double its capacity:
		{
			timer->mPeriod = (DWORD)period; // Always use this method & check to retain compatibility with existing scripts.
			timer->mRunOnlyOnce = false;
		}
	}

	if (*aPriority) // Caller wants this member to be changed from its current or default value.
		timer->mPriority = ATOI(aPriority); // Read any float in a runtime variable reference as an int.

	if (!(timer_existed && aUpdatePriorityOnly))
		// Caller relies on us updating mTimeLastRun in this case.  This is done because it's more
		// flexible, e.g. a user might want to create a timer that is triggered 5 seconds from now.
		// In such a case, we don't want the timer's first triggering to occur immediately.
		// Instead, we want it to occur only when the full 5 seconds have elapsed:
		timer->mTimeLastRun = GetTickCount();

    // Below is obsolete, see above for why:
	// We don't have to kill or set the main timer because the only way this function is called
	// is directly from the execution of a script line inside ExecUntil(), in which case:
	// 1) KILL_MAIN_TIMER is never needed because the timer shouldn't exist while in ExecUntil().
	// 2) SET_MAIN_TIMER is never needed because it will be set automatically the next time ExecUntil()
	//    calls MsgSleep().
	return OK;
}



Label *Script::FindLabel(char *aLabelName)
// Returns the first label whose name matches aLabelName, or NULL if not found.
// v1.0.42: Since duplicates labels are now possible (to support #IfWin variants of a particular
// hotkey or hotstring), callers must be aware that only the first match is returned.
// This helps performance by requiring on average only half the labels to be searched before
// a match is found.
{
	if (!aLabelName || !*aLabelName) return NULL;
	for (Label *label = mFirstLabel; label != NULL; label = label->mNextLabel)
		if (!stricmp(label->mName, aLabelName)) // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
			return label; // Match found.
	return NULL; // No match found.
}



ResultType Script::AddLabel(char *aLabelName, bool aAllowDupe)
// Returns OK or FAIL.
{
	if (!*aLabelName)
		return FAIL; // For now, silent failure because callers should check this beforehand.
	if (!aAllowDupe && FindLabel(aLabelName)) // Relies on short-circuit boolean order.
		// Don't attempt to dereference "duplicate_label->mJumpToLine because it might not
		// exist yet.  Example:
		// label1:
		// label1:  <-- This would be a dupe-error but it doesn't yet have an mJumpToLine.
		// return
		return ScriptError("Duplicate label.", aLabelName);
	char *new_name = SimpleHeap::Malloc(aLabelName);
	if (!new_name)
		return FAIL;  // It already displayed the error for us.
	Label *the_new_label = new Label(new_name); // Pass it the dynamic memory area we created.
	if (the_new_label == NULL)
		return ScriptError(ERR_OUTOFMEM);
	the_new_label->mPrevLabel = mLastLabel;  // Whether NULL or not.
	if (mFirstLabel == NULL)
		mFirstLabel = the_new_label;
	else
		mLastLabel->mNextLabel = the_new_label;
	// This must be done after the above:
	mLastLabel = the_new_label;
	if (!stricmp(new_name, "OnClipboardChange"))
		mOnClipboardChangeLabel = the_new_label;
	return OK;
}



ResultType Script::ParseAndAddLine(char *aLineText, ActionTypeType aActionType, ActionTypeType aOldActionType
	, char *aActionName, char *aEndMarker, char *aLiteralMap, size_t aLiteralMapLength)
// Returns OK or FAIL.
// aLineText needs to be a string whose contents are modifiable (though the string won't be made any
// longer than it is now, so it doesn't have to be of size LINE_SIZE). This helps performance by
// allowing the string to be split into sections without having to make temporary copies.
{
#ifdef _DEBUG
	if (!aLineText || !*aLineText)
		return ScriptError("DEBUG: ParseAndAddLine() called incorrectly.");
#endif

	bool in_quotes;
	int open_parens;

	char action_name[MAX_VAR_NAME_LENGTH + 1], *end_marker;
	if (aActionName) // i.e. this function was called recursively with explicit values for the optional params.
	{
		strcpy(action_name, aActionName);
		end_marker = aEndMarker;
	}
	else if (aActionType == ACT_EXPRESSION)
	{
		*action_name = '\0';
		end_marker = NULL; // Indicate that there is no action to mark the end of.
	}
	else // We weren't called recursively from self, nor is it ACT_EXPRESSION, so set action_name and end_marker the normal way.
	{
		for (;;) // A loop with only one iteration so that "break" can be used instead of a lot of nested if's.
		{
			if (!g.CurrentFunc) // Not inside a function body, so "Global"/"Local"/"Static" get no special treatment.
				break;

			#define VAR_DECLARE_NONE   0
			#define VAR_DECLARE_GLOBAL 1
			#define VAR_DECLARE_LOCAL  2
			#define VAR_DECLARE_STATIC 3
			int declare_type;
			char *cp;
			if (!strnicmp(aLineText, "Global", 6)) // Checked first because it's more common than the others.
			{
				cp = aLineText + 6; // The character after the declaration word.
				declare_type = VAR_DECLARE_GLOBAL;
			}
			else if (!strnicmp(aLineText, "Local", 5))
			{
				cp = aLineText + 5; // The character after the declaration word.
				declare_type = VAR_DECLARE_LOCAL;
			}
			else if (!strnicmp(aLineText, "Static", 6)) // Static also implies local (for functions that default to global).
			{
				cp = aLineText + 6; // The character after the declaration word.
				declare_type = VAR_DECLARE_STATIC;
			}
			else // It's not the word "global", "local", or static, so no further checking is done.
				break;

			if (*cp && !IS_SPACE_OR_TAB(*cp)) // There is a character following the word local but it's not a space or tab.
				break; // It doesn't qualify as being the global or local keyword because it's something like global2.
			if (*cp && *(cp = omit_leading_whitespace(cp))) // Probably always a true stmt since caller rtrimmed it, but even if not it's handled correctly.
			{
				// Check whether the first character is an operator by seeing if it alone would be a
				// valid variable name.  If it's not valid, this doesn't qualify as the global or local
				// keyword because it's something like this instead:
				// local := xyz
				// local += 3
				char orig_char = cp[1];
				cp[1] = '\0'; // Temporarily terminate.
				ResultType result = Var::ValidateName(cp, false, DISPLAY_NO_ERROR);
				cp[1] = orig_char; // Undo the termination.
				if (!result) // It's probably operator, e.g. local = %var%
					break;
			}
			else // It's the word "global", "local", "static" by itself.  But only global is valid that way (when it's the first line in the function body).
			{
				// All of the following must be checked to catch back-to-back conflicting declarations such
				// as these:
				// global x
				// global  ; Should be an error because global vars are implied/automatic.
				if (declare_type == VAR_DECLARE_GLOBAL && mNextLineIsFunctionBody && g.CurrentFunc->mDefaultVarType == VAR_ASSUME_NONE)
				{
					g.CurrentFunc->mDefaultVarType = VAR_ASSUME_GLOBAL;
					// No further action is required for the word "global" by itself.
					return OK;
				}
				// Otherwise, it's the word "local"/"static" by itself or "global" by itself but that occurs too far down in the body.
				return ScriptError(ERR_UNRECOGNIZED_ACTION, aLineText); // Vague error since so rare.
			}
			if (mNextLineIsFunctionBody && g.CurrentFunc->mDefaultVarType == VAR_ASSUME_NONE)
			{
				// Both of the above must be checked to catch back-to-back conflicting declarations such
				// as these:
				// local x
				// global y  ; Should be an error because global vars are implied/automatic.
				// This line will become first non-directive, non-label line in the function's body.

				// If the first non-directive, non-label line in the function's body contains
				// the "local" keyword, everything inside this function will assume that variables
				// are global unless they are explicitly declared local (this is the opposite of
				// the default).  The converse is also true.  UPDATE: "static" must also force ASSUME_LOCAL
				// into effect because otherwise statics wouldn't go into the exception list and thus
				// wouldn't be properly looked up when they're referenced throughout the function body.
				// Therefore, if the first line of the function body is "static MyVar", VAR_DECLARE_LOCAL
				// goes into effect permanently, which can be worked around by using the word "global"
				// as the first word of the function instead.
				g.CurrentFunc->mDefaultVarType = declare_type == VAR_DECLARE_LOCAL ? VAR_ASSUME_GLOBAL : VAR_ASSUME_LOCAL;
			}
			else // Since this isn't the first line of the function's body, mDefaultVarType has aleady been set permanently.
			{
				// Seems best to flag errors since they might be an indication to the user that something
				// is being done incorrectly in this function, not to mention being a reminder about what
				// mode the function is in:
				if (g.CurrentFunc->mDefaultVarType == VAR_ASSUME_GLOBAL)
				{
					if (declare_type == VAR_DECLARE_GLOBAL)
						return ScriptError("Global variables do not need to be declared in this function.", aLineText);
				}
				else // Must be VAR_ASSUME_LOCAL at this stage.
					if (declare_type == VAR_DECLARE_LOCAL)
						return ScriptError("Local variables do not need to be declared in this function.", aLineText);
			}
			// Since above didn't break or return, a variable is being declared as an exception to the
			// mode specified by mDefaultVarType (except if it's a static, which would be an exception
			// only if VAR_ASSUME_GLOBAL is in effect, since statics are implicitly local).

			// If the declare_type is local or global, inversion must be done (i.e. this will be an exception
			// variable) because otherwise it would have already displayed an "unnecessary declaration" error
			// and returned above.  But if the declare_type is static, and given that all static variables are
			// local, inversion is necessary only if the current mode isn't LOCAL:
			bool is_already_exception, is_exception = (declare_type != VAR_DECLARE_STATIC
				|| g.CurrentFunc->mDefaultVarType == VAR_ASSUME_GLOBAL); // Above has ensured that NONE can't be in effect by the time we reach the first static.
			bool open_brace_was_added, belongs_to_if_or_else_or_loop;
			VarSizeType var_name_length;
			char *item;

			for (belongs_to_if_or_else_or_loop = ACT_IS_IF_OR_ELSE_OR_LOOP(mLastLine->mActionType)
				, open_brace_was_added = false, item = cp
				; *item;) // FOR EACH COMMA-SEPARATED ITEM IN THE DECLARATION LIST.
			{
				char *item_end = StrChrAny(item, ", \t=:");  // Comma, space or tab, equal-sign, colon.
				if (!item_end) // This is probably the last/only variable in the list; e.g. the "x" in "local x"
					item_end = item + strlen(item);
				var_name_length = (VarSizeType)(item_end - item);

				int always_use;
				if (is_exception)
					always_use = g.CurrentFunc->mDefaultVarType == VAR_ASSUME_GLOBAL ? ALWAYS_USE_LOCAL : ALWAYS_USE_GLOBAL;
				else
					always_use = ALWAYS_USE_DEFAULT;

				Var *var;
				if (   !(var = FindOrAddVar(item, var_name_length, always_use, &is_already_exception))   )
					return FAIL; // It already displayed the error.
				if (is_already_exception) // It was already in the exception list (previously declared).
					return ScriptError("Duplicate declaration.", item);
				if (var->Type() != VAR_NORMAL || !strlicmp(item, "ErrorLevel", var_name_length)) // Shouldn't be declared either way (global or local).
					return ScriptError("Built-in variables must not be declared.", item);
				for (int i = 0; i < g.CurrentFunc->mParamCount; ++i) // Search by name to find both global and local declarations.
					if (!strlicmp(item, g.CurrentFunc->mParam[i].var->mName, var_name_length))
						return ScriptError("Parameters must not be declared.", item);
				if (is_exception)
				{
					if (mFuncExceptionVarCount >= MAX_FUNC_VAR_EXCEPTIONS)
						return ScriptError("Too many declarations.", item); // Short message since it's so unlikely.
					mFuncExceptionVar[mFuncExceptionVarCount++] = var;
				}
				if (declare_type == VAR_DECLARE_STATIC)
					var->OverwriteAttrib(VAR_ATTRIB_STATIC);

				item_end = omit_leading_whitespace(item_end); // Move up to the next comma, assignment-op, or '\0'.

				bool convert_the_operator;
				switch(*item_end)
				{
				case ',':  // No initializer is present for this variable, so move on to the next one.
					item = omit_leading_whitespace(item_end + 1); // Set "item" for use by the next iteration.
					continue; // No further processing needed below.
				case '\0': // No initializer is present for this variable, so move on to the next one.
					item = item_end; // Set "item" for use by the next iteration.
					continue;
				case ':':
					if (item_end[1] != '=') // Colon with no following '='.
						return ScriptError(ERR_UNRECOGNIZED_ACTION, item); // Vague error since so rare.
					item_end += 2; // Point to the character after the ":=".
					convert_the_operator = false;
					break;
				case '=': // Here '=' is clearly an assignment not a comparison, so further below it will be converted to :=
					++item_end; // Point to the character after the "=".
					convert_the_operator = true;
					break;
				}
				char *right_side_of_operator = item_end; // Save for use by VAR_DECLARE_STATIC below.

				// Since above didn't "continue", this declared variable also has an initializer.
				// Add that initializer as a separate line to be executed at runtime. Separate lines
				// might actually perform better at runtime because most initializers tend to be simple
				// literals or variables that are simplified into non-expressions at runtime. In addition,
				// items without an initializer are omitted, further improving runtime performance.
				// However, the following must be done ONLY after having done the FindOrAddVar()
				// above, since that may have changed this variable to a non-default type (local or global).
				// But what about something like "global x, y=x"? Even that should work as long as x
				// appears in the list prior to initializers that use it.
				// Now, find the comma (or terminator) that marks the end of this sub-statement.
				// The search must exclude commas that are inside quoted/literal strings and those that
				// are inside parentheses (chiefly those of function-calls, but possibly others).

				for (in_quotes = false, open_parens = 0; *item_end; ++item_end) // FIND THE NEXT "REAL" COMMA.
				{
					if (*item_end == ',') // This is outside the switch() further below so that its "break" can get out of the loop.
					{
						if (!in_quotes && open_parens < 1) // A delimiting comma other than one in a sub-statement or function. Shouldn't need to worry about unquoted escaped commas since they don't make sense in a declaration list.
							break;
						// Otherwise, its a quoted/literal comma or one in parentheses (such as function-call).
						continue; // Continue past it to look for the correct comma.
					}
					switch (*item_end)
					{
					case '"': // There are sections similar this one later below; so see them for comments.
						in_quotes = !in_quotes;
						break;
					case '(':
						if (!in_quotes) // Literal parentheses inside a quoted string should not be counted for this purpose.
							++open_parens;
						break;
					case ')':
						if (!in_quotes)
						{
							if (!open_parens)
								return ScriptError(ERR_MISSING_OPEN_PAREN, item);
							--open_parens;
						}
						break;
					//default: some other character; just have the loop skip over it.
					}
				} // for() to look for the ending comma or terminator of this sub-statement.
				if (open_parens) // At least one '(' is never closed.
					return ScriptError(ERR_MISSING_CLOSE_PAREN, item); // Use "item" because the problem is probably somewhere after that point in the declaration list.
				if (in_quotes)
					return ScriptError(ERR_MISSING_CLOSE_QUOTE, item);

				// Above has now found the final comma of this sub-statement (or the terminator if there is no comma).
				char *terminate_here = omit_trailing_whitespace(item, item_end-1) + 1; // v1.0.47.02: Fix the fact that "x=5 , y=6" would preserve the whitespace at the end of "5".  It also fixes wrongly showing a syntax error for things like: static d="xyz"  , e = 5
				char orig_char = *terminate_here;
				*terminate_here = '\0'; // Temporarily terminate (it might already be the terminator, but that's harmless).

				if (declare_type == VAR_DECLARE_STATIC) // v1.0.46: Support simple initializers for static variables.
				{
					// The following is similar to the code used to support default values for function parameters.
					// So maybe maintain them together.
					right_side_of_operator = omit_leading_whitespace(right_side_of_operator);
					if (!stricmp(right_side_of_operator, "false"))
						var->Assign("0");
					else if (!stricmp(right_side_of_operator, "true"))
						var->Assign("1");
					else // The only other supported initializers are "string", integers, and floats.
					{
						// Vars could be supported here via FindVar(), but only globals ABOVE this point in
						// the script would be supported (since other globals don't exist yet; in fact, even
						// those that do exist don't have any contents yet, so it would be pointless). So it
						// seems best to wait until full/comprehesive support for expressions is
						// studied/designed for both statics and parameter-default-values.
						if (*right_side_of_operator == '"' && terminate_here[-1] == '"') // Quoted/literal string.
						{
							++right_side_of_operator; // Omit the opening-quote from further consideration.
							terminate_here[-1] = '\0'; // Remove the close-quote from further consideration.
							ConvertEscapeSequences(right_side_of_operator, g_EscapeChar, false); // Raw escape sequences like `n haven't been converted yet, so do it now.
							// Convert all pairs of quotes into single literal quotes:
							StrReplace(right_side_of_operator, "\"\"", "\"", SCS_SENSITIVE);
						}
						else // It's not a quoted string (nor the empty string); or it has a missing ending quote (rare).
						{
							if (!IsPureNumeric(right_side_of_operator, true, false, true)) // It's not a number, and since we're here it's not a quoted/literal string either.
								return ScriptError("Unsupported static initializer.", right_side_of_operator);
							//else it's an int or float, so just assign the numeric string itself (there
							// doesn't seem to be any need to convert it to float/int first, though that would
							// make things more consistent such as storing .1 as 0.1).
						}
						if (*right_side_of_operator) // It can be "" in cases such as "" being specified literally in the script, in which case nothing needs to be done because all variables start off as "".
							var->Assign(right_side_of_operator);
					}
				}
				else // A non-static initializer, so a line of code must be produced that will executed at runtime every time the function is called.
				{
					char *line_to_add;
					if (convert_the_operator) // Convert first '=' in item to be ":=".
					{
						// Prevent any chance of overflow by using new_buf (overflow might otherwise occur in cases
						// such as this sub-statement being the very last one in the declaration list, and being
						// at the limit of the buffer's capacity).
						char new_buf[LINE_SIZE]; // Using so much stack space here and in caller seems unlikely to affect performance, so _alloca seems unlikely to help.
						StrReplace(strcpy(new_buf, item), "=", ":=", SCS_SENSITIVE, 1); // Can't overflow because there's only one replacement and we know item's length can't be that close to the capacity limit.
						line_to_add = new_buf;
					}
					else
						line_to_add = item;
					if (belongs_to_if_or_else_or_loop && !open_brace_was_added) // v1.0.46.01: Put braces to allow initializers to work even directly under an IF/ELSE/LOOP.  Note that the braces aren't added or needed for static initializers.
					{
						if (!AddLine(ACT_BLOCK_BEGIN))
							return FAIL;
						open_brace_was_added = true;
					}
					// Call Parse() vs. AddLine() because it detects and optimizes simple assignments into
					// non-exprssions for faster runtime execution.
					if (!ParseAndAddLine(line_to_add)) // For simplicity and maintainability, call self rather than trying to set things up properly to stay in self.
						return FAIL; // Above already displayed the error.
				}

				*terminate_here = orig_char; // Undo the temporary termination.
				// Set "item" for use by the next iteration:
				item = (*item_end == ',') // i.e. it's not the terminator and thus not the final item in the list.
					? omit_leading_whitespace(item_end + 1)
					: item_end; // It's the terminator, so let the loop detect that to finish.
			} // for() each item in the declaration list.
			if (open_brace_was_added)
				if (!AddLine(ACT_BLOCK_END))
					return FAIL;
			return OK;
		} // single-iteration for-loop

		// Since above didn't return, it's not a declaration such as "global MyVar".
		if (   !(end_marker = ParseActionType(action_name, aLineText, true))   )
			return FAIL; // It already displayed the error.
	}

	// Above has ensured that end_marker is the address of the last character of the action name,
	// or NULL if there is no action name.
	// Find the arguments (not to be confused with exec_params) of this action, if it has any:
	char *action_args = end_marker ? omit_leading_whitespace(end_marker + 1) : aLineText;
	// Now action_args is either the first delimiter or the first parameter (if the optional first
	// delimiter was omitted).
	bool add_openbrace_afterward = false; // v1.0.41: Set default for use in supporting brace in "if (expr) {" and "Loop {".

	if (*action_args == g_delimiter)
	{
		// Since there's a comma, don't change aActionType because if it's ACT_INVALID, it should stay that way
		// so that "something, += 4" is not a valid assignment or other operator, but should still be checked
		// against the list of commands to see if it's something like "MsgBox, += 4" (in this case, a script may
		// use the comma to avoid ambiguity).
		// Find the start of the next token (or its ending delimiter if the token is blank such as ", ,"):
		for (++action_args; IS_SPACE_OR_TAB(*action_args); ++action_args);
	}
	else if (!aActionType && !aOldActionType) // i.e. the caller hasn't yet determined this line's action type.
	{
		if (!stricmp(action_name, "IF")) // It's an IF-statement.
		{
			/////////////////////////////////////
			// Detect all types of IF-statements.
			/////////////////////////////////////
			char *operation, *next_word;
			if (*action_args == '(') // i.e. if (expression)
			{
				// To support things like the following, the outermost enclosing parentheses are not removed:
				// if (x < 3) or (x > 6)
				// Also note that although the expression must normally start with an open-parenthesis to be
				// recognized as ACT_IFEXPR, it need not end in a close-paren; e.g. if (x = 1) or !done.
				// If these or any other parentheses are unbalanced, it will caught further below.
				aActionType = ACT_IFEXPR; // Fixed for v1.0.31.01.
			}
			else // Generic or indeterminate IF-statement, so find out what type it is.
			{
				DEFINE_END_FLAGS
				// Skip over the variable name so that the "is" and "is not" operators are properly supported:
				if (   !(operation = StrChrAny(action_args, end_flags))   )
					operation = action_args + strlen(action_args); // Point it to the NULL terminator instead.
				else
					operation = omit_leading_whitespace(operation);

				// v1.0.42: Fix "If not Installed" not be seen as "If var-named-'not' in MatchList", being
				// careful not to break "If NotInstalled in MatchList".  The following are also fixed in
				// a similar way:
				// If not BetweenXXX
				// If not ContainsXXX
				bool first_word_is_not = !strnicmp(action_args, "Not", 3) && strchr(end_flags, action_args[3]);

				switch (*operation)
				{
				case '=': // But don't allow == to be "Equals" since the 2nd '=' might be literal.
					aActionType = ACT_IFEQUAL;
					break;
				case '<':
					// Note: User can use whitespace to differentiate a literal symbol from
					// part of an operator, e.g. if var1 < =  <--- char is literal
					switch(operation[1])
					{
					case '=': aActionType = ACT_IFLESSOREQUAL; operation[1] = ' '; break;
					case '>': aActionType = ACT_IFNOTEQUAL; operation[1] = ' '; break;
					default:  aActionType = ACT_IFLESS;  // i.e. some other symbol follows '<'
					}
					break;
				case '>': // Don't allow >< to be NotEqual since the '<' might be intended as a literal part of an arg.
					if (operation[1] == '=')
					{
						aActionType = ACT_IFGREATEROREQUAL;
						operation[1] = ' '; // Remove it from so that it won't be considered by later parsing.
					}
					else
						aActionType = ACT_IFGREATER;
					break;
				case '!':
					if (operation[1] == '=')
					{
						aActionType = ACT_IFNOTEQUAL;
						operation[1] = ' '; // Remove it from so that it won't be considered by later parsing.
					}
					else
						// To minimize the times where expressions must have an outer set of parentheses,
						// assume all unknown operators are expressions, e.g. "if !var"
						aActionType = ACT_IFEXPR;
					break;
				case 'b': // "Between"
				case 'B':
					// Must fall back to ACT_IFEXPR, otherwise "if not var_name_beginning_with_b" is a syntax error.
					if (first_word_is_not || strnicmp(operation, "between", 7))
						aActionType = ACT_IFEXPR;
					else
					{
						aActionType = ACT_IFBETWEEN;
						// Set things up to be parsed as args further down.  A delimiter is inserted later below:
						memset(operation, ' ', 7);
					}
					break;
				case 'c': // "Contains"
				case 'C':
					// Must fall back to ACT_IFEXPR, otherwise "if not var_name_beginning_with_c" is a syntax error.
					if (first_word_is_not || strnicmp(operation, "contains", 8))
						aActionType = ACT_IFEXPR;
					else
					{
						aActionType = ACT_IFCONTAINS;
						// Set things up to be parsed as args further down.  A delimiter is inserted later below:
						memset(operation, ' ', 8);
					}
					break;
				case 'i':  // "is" or "is not"
				case 'I':
					switch (toupper(operation[1]))
					{
					case 's':  // "IS"
					case 'S':
						if (first_word_is_not)        // v1.0.45: Had forgotten to fix this one with the others,
							aActionType = ACT_IFEXPR; // so now "if not is_something" and "if not is_something()" work.
						else
						{
							next_word = omit_leading_whitespace(operation + 2);
							if (strnicmp(next_word, "not", 3))
								aActionType = ACT_IFIS;
							else
							{
								aActionType = ACT_IFISNOT;
								// Remove the word "not" to set things up to be parsed as args further down.
								memset(next_word, ' ', 3);
							}
							operation[1] = ' '; // Remove the 'S' in "IS".  'I' is replaced with ',' later below.
						}
						break;
					case 'n':  // "IN"
					case 'N':
						if (first_word_is_not)
							aActionType = ACT_IFEXPR;
						else
						{
							aActionType = ACT_IFIN;
							operation[1] = ' '; // Remove the 'N' in "IN".  'I' is replaced with ',' later below.
						}
						break;
					default:
						// v1.0.35.01 It must fall back to ACT_IFEXPR, otherwise "if not var_name_beginning_with_i"
						// is a syntax error.
						aActionType = ACT_IFEXPR;
					} // switch()
					break;
				case 'n':  // It's either "not in", "not between", or "not contains"
				case 'N':
					// Must fall back to ACT_IFEXPR, otherwise "if not var_name_beginning_with_n" is a syntax error.
					if (strnicmp(operation, "not", 3))
						aActionType = ACT_IFEXPR;
					else
					{
						// Remove the "NOT" separately in case there is more than one space or tab between
						// it and the following word, e.g. "not   between":
						memset(operation, ' ', 3);
						next_word = omit_leading_whitespace(operation + 3);
						if (!strnicmp(next_word, "in", 2))
						{
							aActionType = ACT_IFNOTIN;
							memset(next_word, ' ', 2);
						}
						else if (!strnicmp(next_word, "between", 7))
						{
							aActionType = ACT_IFNOTBETWEEN;
							memset(next_word, ' ', 7);
						}
						else if (!strnicmp(next_word, "contains", 8))
						{
							aActionType = ACT_IFNOTCONTAINS;
							memset(next_word, ' ', 8);
						}
					}
					break;

				default: // To minimize the times where expressions must have an outer set of parentheses, assume all unknown operators are expressions.
					aActionType = ACT_IFEXPR;
				} // switch()
			} // Detection of type of IF-statement.

			if (aActionType == ACT_IFEXPR) // There are various ways above for aActionType to become ACT_IFEXPR.
			{
				// Since this is ACT_IFEXPR, action_args is known not to be an empty string, which is relied on below.
				char *action_args_last_char = action_args + strlen(action_args) - 1; // Shouldn't be a whitespace char since those should already have been removed at an earlier stage.
				if (*action_args_last_char == '{') // This is an if-expression statement with an open-brace on the same line.
				{
					*action_args_last_char = '\0';
					rtrim(action_args, action_args_last_char - action_args);  // Remove the '{' and all its whitespace from further consideration.
					add_openbrace_afterward = true;
				}
			}
			else // It's a IF-statement, but a traditional/non-expression one.
			{
				// Set things up to be parsed as args later on.
				*operation = g_delimiter;
				if (aActionType == ACT_IFBETWEEN || aActionType == ACT_IFNOTBETWEEN)
				{
					// I decided against the syntax "if var between 3,8" because the gain in simplicity
					// and the small avoidance of ambiguity didn't seem worth the cost in terms of readability.
					for (next_word = operation;;)
					{
						if (   !(next_word = strcasestr(next_word, "and"))   )
							return ScriptError("BETWEEN requires the word AND.", aLineText); // Seems too rare a thing to warrant falling back to ACT_IFEXPR for this.
						if (strchr(" \t", *(next_word - 1)) && strchr(" \t", *(next_word + 3)))
						{
							// Since there's a space or tab on both sides, we know this is the correct "and",
							// i.e. not one contained within one of the parameters.  Examples:
							// if var between band and cat  ; Don't falsely detect "band"
							// if var betwwen Andy and David  ; Don't falsely detect "Andy".
							// Replace the word AND with a delimiter so that it will be parsed correctly later:
							*next_word = g_delimiter;
							*(next_word + 1) = ' ';
							*(next_word + 2) = ' ';
							break;
						}
						else
							next_word += 3;  // Skip over this false "and".
					} // for()
				} // ACT_IFBETWEEN
			} // aActionType != ACT_IFEXPR
		}
		else // It isn't an IF-statement, so check for assignments/operators that determine that this line isn't one that starts with a named command.
		{
			//////////////////////////////////////////////////////
			// Detect operators and assignments such as := and +=
			//////////////////////////////////////////////////////
			// This section is done before the section that checks whether action_name is a valid command
			// because it avoids ambiguity in a line such as the following:
			//    Input = test  ; Would otherwise be confused with the Input command.
			// But there may be times when a line like this is used:
			//    MsgBox =  ; i.e. the equals is intended to be the first parameter, not an operator.
			// In the above case, the user can provide the optional comma to avoid the ambiguity:
			//    MsgBox, =
			char action_args_2nd_char = action_args[1];
			bool convert_pre_inc_or_dec = false; // Set default.

			switch(*action_args)
			{
			case '=': // i.e. var=value (old-style assignment)
				aActionType = ACT_ASSIGN;
				break;
			case ':':
				// v1.0.40: Allow things like "MsgBox :: test" to be valid by insisting that '=' follows ':'.
				if (action_args_2nd_char == '=') // i.e. :=
					aActionType = ACT_ASSIGNEXPR;
				break;
			case '+':
				// Support for ++i (and in the next case, --i).  In these cases, action_name must be either
				// "+" or "-", and the first character of action_args must match it.
				if ((convert_pre_inc_or_dec = action_name[0] == '+' && !action_name[1]) // i.e. the pre-increment operator; e.g. ++index.
					|| action_args_2nd_char == '=') // i.e. x+=y (by contrast, post-increment is recognized only after we check for a command name to cut down on ambiguity).
					aActionType = ACT_ADD;
				break;
			case '-':
				// Do a complete validation/recognition of the operator to allow a line such as the following,
				// which omits the first optional comma, to still be recognized as a command rather than a
				// variable-with-operator:
				// SetBatchLines -1
				if ((convert_pre_inc_or_dec = action_name[0] == '-' && !action_name[1]) // i.e. the pre-decrement operator; e.g. --index.
					|| action_args_2nd_char == '=') // i.e. x-=y  (by contrast, post-decrement is recognized only after we check for a command name to cut down on ambiguity).
					aActionType = ACT_SUB;
				break;
			case '*':
				if (action_args_2nd_char == '=') // i.e. *=
					aActionType = ACT_MULT;
				break;
			case '/':
				if (action_args_2nd_char == '=') // i.e. /=
					aActionType = ACT_DIV;
				// ACT_DIV is different than //= and // because ACT_DIV supports floating point inputs by yielding
				// a floating point result (i.e. it doesn't Floor() the result when the inputs are floats).
				else if (action_args_2nd_char == '/' && action_args[2] == '=') // i.e. //=
					aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
				break;
			case '.':
			case '|':
			case '&':
			case '^':
				if (action_args_2nd_char == '=') // i.e. .= and |= and &= and ^=
					aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
				break;
			//case '?': Stand-alone ternary such as true ? fn1() : fn2().  These are rare so are
			// checked later, only after action_name has been checked to see if it's a valid command.
			case '>':
			case '<':
				if (action_args_2nd_char == *action_args && action_args[2] == '=') // i.e. >>= and <<=
					aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
				break;
			//default: Leave aActionType set to ACT_INVALID. This also covers case '\0' in case that's possible.
			} // switch()

			if (aActionType) // An assignment or other type of action was discovered above.
			{
				if (convert_pre_inc_or_dec) // Set up pre-ops like ++index and --index to be parsed properly later.
				{
					// The following converts:
					// ++x -> EnvAdd x,1 (not really "EnvAdd" per se; but ACT_ADD).
					// Set action_args to be the word that occurs after the ++ or --:
					action_args = omit_leading_whitespace(++action_args); // Though there generally isn't any.
					if (StrChrAny(action_args, EXPR_ALL_SYMBOLS ".")) // Support things like ++Var ? f1() : f2() and ++Var /= 5. Don't need strstr(action_args, " ?") because the search already looks for ':'.
						aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
					else
					{
						// Set up aLineText and action_args to be parsed later on as a list of two parameters:
						// The variable name followed by the amount to be added or subtracted (e.g. "ScriptVar, 1").
						// We're not changing the length of aLineText by doing this, so it should be large enough:
						size_t new_length = strlen(action_args);
						// Since action_args is just a pointer into the aLineText buffer (which caller has ensured
						// is modifiable), use memmove() so that overlapping source & dest are properly handled:
						memmove(aLineText, action_args, new_length + 1); // +1 to include the zero terminator.
						// Append the second param, which is just "1" since the ++ and -- only inc/dec by 1:
						aLineText[new_length++] = g_delimiter;
						aLineText[new_length++] = '1';
						aLineText[new_length] = '\0';
					}
				}
				else if (aActionType != ACT_EXPRESSION) // i.e. it's ACT_ASSIGN/ASSIGNEXPR/ADD/SUB/MULT/DIV
				{
					if (aActionType != ACT_ASSIGN) // i.e. it's ACT_ASSIGNEXPR/ADD/SUB/MULT/DIV
					{
						// Find the first non-function comma, which in the case of ACT_ADD/SUB can be
						// either a statement-separator comma (expression) or the time units arg.
						// Reasons for this:
						// 1) ACT_ADD/SUB: Need to distinguish compound statements from date/time math;
						//    e.g. "x+=1, y+=2" should be marked as a stand-alone expression, not date math.
						// 2) ACT_ASSIGNEXPR/MULT/DIV (and ACT_ADD/SUB for that matter): Need to make
						//    comma-separated sub-expressions into one big ACT_EXPRESSION so that the
						//    leftmost sub-expression will get evaluated prior to the others (for consistency
						//    and as documented).  However, this has some side-effects, such as making
						//    the leftmost /= operator into true division rather than ENV_DIV behavior,
						//    and treating blanks as errors in math expressions when otherwise ENV_MULT
						//    would treat them as zero.
						// ALSO: ACT_ASSIGNEXPR/ADD/SUB/MULT/DIV are made into ACT_EXPRESSION *only* when multi-
						// statement commas are present because the following legacy behaviors must be retained:
						// 1) Math treatment of blanks as zero in ACT_ADD/SUB/etc.
						// 2) EnvDiv's special behavior, which is different than both true divide and floor divide.
						// 3) Possibly add/sub's date/time math.
						// 4) For performance, don't want trivial assignments to become ACT_EXPRESSION.
						char *cp;
						for (in_quotes = false, open_parens = 0, cp = action_args + 2; *cp; ++cp)
						{
							switch (*cp)
							{
							case '"': // This is whole section similar to another one later below, so see it for comments.
								in_quotes = !in_quotes;
								break;
							case '(':
								if (!in_quotes) // Literal parentheses inside a quoted string should not be counted for this purpose.
									++open_parens;
								break;
							case ')':
								if (!in_quotes)
									--open_parens;
								break;
							}
							if (*cp == g_delimiter && !in_quotes && open_parens < 1) // A delimiting comma other than one in a sub-statement or function. Shouldn't need to worry about unquoted escaped commas since they don't make sense with += and -=.
							{
								if (aActionType == ACT_ADD || aActionType == ACT_SUB)
								{
									cp = omit_leading_whitespace(cp + 1);
									if (StrChrAny(cp, EXPR_ALL_SYMBOLS ".")) // Don't need strstr(cp, " ?") because the search already looks for ':'.
										aActionType = ACT_EXPRESSION; // It's clearly an expression not a word like Days or %VarContainingTheWordDays%.
									//else it's probably date/time math, so leave it as-is.
								}
								else // ACT_ASSIGNEXPR/MULT/DIV, for which any non-function comma qualifies it as multi-statement.
									aActionType = ACT_EXPRESSION;
								break;
							}
						}
					}
					if (aActionType != ACT_EXPRESSION) // The above didn't make it a stand-alone expression.
					{
						// The following converts:
						// x+=2 -> ACT_ADD x, 2.
						// x:=2 -> ACT_ASSIGNEXPR, x, 2
						// etc.
						// But post-inc/dec are recognized only after we check for a command name to cut down on ambiguity
						*action_args = g_delimiter; // Replace the =,+,-,:,*,/ with a delimiter for later parsing.
						if (aActionType != ACT_ASSIGN) // i.e. it's not just a plain equal-sign (which has no 2nd char).
							action_args[1] = ' '; // Remove the "=" from consideration.
					}
				}
				//else it's already an isolated expression, so no changes are desired.
				action_args = aLineText; // Since this is an assignment and/or expression, use the line's full text for later parsing.
			} // if (aActionType)
		} // Handling of assignments and other operators.
	}
	//else aActionType was already determined by the caller.

	// Now the above has ensured that action_args is the first parameter itself, or empty-string if none.
	// If action_args now starts with a delimiter, it means that the first param is blank/empty.

	if (!aActionType && !aOldActionType) // Caller nor logic above has yet determined the action.
		if (   !(aActionType = ConvertActionType(action_name))   ) // Is this line a command?
			aOldActionType = ConvertOldActionType(action_name);    // If not, is it an old-command?

	if (!aActionType && !aOldActionType) // Didn't find any action or command in this line.
	{
		// v1.0.41: Support one-true brace style even if there's no space, but make it strict so that
		// things like "Loop{ string" are reported as errors (in case user intended a file-pattern loop).
		if (!stricmp(action_name, "Loop{") && !*action_args)
		{
			aActionType = ACT_LOOP;
			add_openbrace_afterward = true;
		}
		else if (*action_args == '?' && IS_SPACE_OR_TAB(action_args[1]) // '?' currently requires a trailing space or tab because variable names can contain '?' (except '?' by itself).  For simplicty, no NBSP check.
			|| strchr(EXPR_ALL_SYMBOLS ".", *action_args))
		{
			char *question_mark;
			if ((*action_args == '+' || *action_args == '-') && action_args[1] == *action_args) // Post-inc/dec. See comments further below.
			{
				if (action_args[2]) // i.e. if the ++ and -- isn't the last thing; e.g. x++ ? fn1() : fn2() ... Var++ //= 2
					aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
				else
				{
					// The logic here allows things like IfWinActive-- to be seen as commands even without
					// a space before the -- or ++.  For backward compatibility and code simplicity, it seems
					// best to keep that behavior rather than distinguishing between Command-- and Command --.
					// In any case, "Command --" should continue to be seen as a command regardless of what
					// changes are ever made.  That's why this section occurs below the command-name lookup.
					// The following converts x++ to "ACT_ADD x,1".
					aActionType = (*action_args == '+') ? ACT_ADD : ACT_SUB;
					*action_args = g_delimiter;
					action_args[1] = '1';
				}
				action_args = aLineText; // Since this is an assignment and/or expression, use the line's full text for later parsing.
			}
			else if (*action_args == '?' // Don't need a leading space if first char is '?' (though should have a trailing, but for simplicity it isn't checked).
				|| (question_mark = strstr(action_args, " ? ")) && strchr(question_mark, ':')) // Rough check (see comments below). Relies on short-circuit boolean order.
			{
				// To avoid hindering load-time error detection such as misspelled command names, allow stand-alone
				// expressions only for things that can produce a side-effect (currently only ternaries like
				// the ones mentioned later below need to be checked since the following other things were
				// previously recognized as ACT_EXPRESSION if appropriate: function-calls, post- and
				// pre-inc/dec (++/--), and assignment operators like := += *= (though these don't necessarily
				// need to be ACT_EXPRESSION to support multi-statement; they can be ACT_ASSIGNEXPR, ACT_ADD, etc.
				// and still support comma-separated statements.
				// Stand-alone ternaries are checked for here rather than earlier to allow a command name
				// (of present) to take precedence (since stand-alone ternaries seem much rarer than
				// "Command ? something" such as "MsgBox ? something".  Could also check for a colon somewhere
				// to the right if further ambiguity-resolution is ever needed.  Also, a stand-alone ternary
				// should have at least one function-call and/or assignment; otherwise it would serve no purpose.
				// A line may contain a stand-alone ternary operator to call functions that have side-effects
				// or perform assignments.  For example:
				//    IsDone ? fn1() : fn2()
				//    3 > 2 ? x:=1 : y:=1
				//    (3 > 2) ... not supported due to overlap with continuation sections.
				aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
				action_args = aLineText; // Since this is an assignment and/or expression, use the line's full text for later parsing.
			}
			//else leave it as an unknown action to avoid hindering load-time error detection.
			// In other words, don't be too permissive about what gets marked as a stand-alone expression.
		}
		if (!aActionType) // Above still didn't find a valid action (i.e. check aActionType again in case the above changed it).
		{
			if (*action_args == '(') // v1.0.46.11: Recognize as multi-statements that start with a function, like "fn(), x:=4".  v1.0.47.03: Removed the following check to allow a close-brace to be followed by a comma-less function-call: strchr(action_args, g_delimiter).
			{
				aActionType = ACT_EXPRESSION; // Mark this line as a stand-alone expression.
				action_args = aLineText; // Since this is a function-call followed by a comma and some other expression, use the line's full text for later parsing.
			}
			else
				// v1.0.40: Give a more specific error message now now that hotkeys can make it here due to
				// the change that avoids the need to escape double-colons:
				return ScriptError(const_cast<char*>(strstr(aLineText, HOTKEY_FLAG) ? "Invalid hotkey." : ERR_UNRECOGNIZED_ACTION, aLineText));
		}
	}

	Action &this_action = aActionType ? g_act[aActionType] : g_old_act[aOldActionType];

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Handle escaped-sequences (escaped delimiters and all others except variable deref symbols).
	// This section must occur after all other changes to the pointer value action_args have
	// occurred above.
	//////////////////////////////////////////////////////////////////////////////////////////////
	// The size of this relies on the fact that caller made sure that aLineText isn't
	// longer than LINE_SIZE.  Also, it seems safer to use char rather than bool, even
	// though on most compilers they're the same size.  Char is always of size 1, but bool
	// can be bigger depending on platform/compiler:
	char literal_map[LINE_SIZE];
	ZeroMemory(literal_map, sizeof(literal_map));  // Must be fully zeroed for this purpose.
	if (aLiteralMap)
	{
		// Since literal map is NOT a string, just an array of char values, be sure to
		// use memcpy() vs. strcpy() on it.  Also, caller's aLiteralMap starts at aEndMarker,
		// so adjust it so that it starts at the newly found position of action_args instead:
		int map_offset = (int)(action_args - end_marker);  // end_marker is known not to be NULL when aLiteralMap is non-NULL.
		int map_length = (int)(aLiteralMapLength - map_offset);
		if (map_length > 0)
			memcpy(literal_map, aLiteralMap + map_offset, map_length);
	}
	else
	{
		// Resolve escaped sequences and make a map of which characters in the string should
		// be interpreted literally rather than as their native function.  In other words,
		// convert any escape sequences in order from left to right (this order is important,
		// e.g. ``% should evaluate to `g_DerefChar not `LITERAL_PERCENT.  This part must be
		// done *after* checking for comment-flags that appear to the right of a valid line, above.
		// How literal comment-flags (e.g. semicolons) work:
		//string1; string2 <-- not a problem since string2 won't be considered a comment by the above.
		//string1 ; string2  <-- this would be a user mistake if string2 wasn't supposed to be a comment.
		//string1 `; string 2  <-- since esc seq. is resolved *after* checking for comments, this behaves as intended.
		// Current limitation: a comment-flag longer than 1 can't be escaped, so if "//" were used,
		// as a comment flag, it could never have whitespace to the left of it if it were meant to be literal.
		// Note: This section resolves all escape sequences except those involving g_DerefChar, which
		// are handled by a later section.
		char c;
		int i;
		for (i = 0; ; ++i)  // Increment to skip over the symbol just found by the inner for().
		{
			for (; action_args[i] && action_args[i] != g_EscapeChar; ++i);  // Find the next escape char.
			if (!action_args[i]) // end of string.
				break;
			c = action_args[i + 1];
			switch (c)
			{
				// Only lowercase is recognized for these:
				case 'a': action_args[i + 1] = '\a'; break;  // alert (bell) character
				case 'b': action_args[i + 1] = '\b'; break;  // backspace
				case 'f': action_args[i + 1] = '\f'; break;  // formfeed
				case 'n': action_args[i + 1] = '\n'; break;  // newline
				case 'r': action_args[i + 1] = '\r'; break;  // carriage return
				case 't': action_args[i + 1] = '\t'; break;  // horizontal tab
				case 'v': action_args[i + 1] = '\v'; break;  // vertical tab
			}
			// Replace escape-sequence with its single-char value.  This is done event if the pair isn't
			// a recognizable escape sequence (e.g. `? becomes ?), which is the Microsoft approach
			// and might not be a bad way of handing things.  There are some exceptions, however.
			// The first of these exceptions (g_DerefChar) is mandatory because that char must be
			// handled at a later stage or escaped g_DerefChars won't work right.  The others are
			// questionable, and might be worth further consideration.  UPDATE: g_DerefChar is now
			// done here because otherwise, examples such as this fail:
			// - The escape char is backslash.
			// - any instances of \\%, such as c:\\%var% , will not work because the first escape
			// sequence (\\) is resolved to a single literal backslash.  But then when \% is encountered
			// by the section that resolves escape sequences for g_DerefChar, the backslash is seen
			// as an escape char rather than a literal backslash, which is not correct.  Thus, we
			// resolve all escapes sequences HERE in one go, from left to right.

			// AutoIt2 definitely treats an escape char that occurs at the very end of
			// a line as literal.  It seems best to also do it for these other cases too.
			// UPDATE: I cannot reproduce the above behavior in AutoIt2.  Maybe it only
			// does it for some commands or maybe I was mistaken.  So for now, this part
			// is disabled:
			//if (c == '\0' || c == ' ' || c == '\t')
			//	literal_map[i] = 1;  // In the map, mark this char as literal.
			//else
			{
				// So these are also done as well, and don't need an explicit check:
				// g_EscapeChar , g_delimiter , (when g_CommentFlagLength > 1 ??): *g_CommentFlag
				// Below has a final +1 to include the terminator:
				MoveMemory(action_args + i, action_args + i + 1, strlen(action_args + i + 1) + 1);
				literal_map[i] = 1;  // In the map, mark this char as literal.
			}
			// else: Do nothing, even if the value is zero (the string's terminator).
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////
	// Do some special preparsing of the MsgBox command, since it is so frequently used and
	// it is also the source of problem areas going from AutoIt2 to 3 and also due to the
	// new numeric parameter at the end.  Whenever possible, we want to avoid the need for
	// the user to have to escape commas that are intended to be literal.
	///////////////////////////////////////////////////////////////////////////////////////
	int mark, max_params_override = 0; // Set default.
	if (aActionType == ACT_MSGBOX)
	{
		// First find out how many non-literal (non-escaped) delimiters are present.
		// Use a high maximum so that we can almost always find and analyze the command's
		// last apparent parameter.  This helps error-checking be more informative in a
		// case where the command specifies a timeout as its last param but it's next-to-last
		// param contains delimiters that the user forgot to escape.  In other words, this
		// helps detect more often when the user is trying to use the timeout feature.
		// If this weren't done, the command would more often forgive improper syntax
		// and not report a load-time error, even though it's pretty obvious that a load-time
		// error should have been reported:
		#define MAX_MSGBOX_DELIMITERS 20
		char *delimiter[MAX_MSGBOX_DELIMITERS];
		int delimiter_count;
		for (mark = delimiter_count = 0; action_args[mark] && delimiter_count < MAX_MSGBOX_DELIMITERS;)
		{
			for (; action_args[mark]; ++mark)
				if (action_args[mark] == g_delimiter && !literal_map[mark]) // Match found: a non-literal delimiter.
				{
					delimiter[delimiter_count++] = action_args + mark;
					++mark; // Skip over this delimiter for the next iteration of the outer loop.
					break;
				}
		}
		// If it has only 1 arg (i.e. 0 delimiters within the arg list) no override is needed.
		// Otherwise do more checking:
		if (delimiter_count)
		{
			// If the first apparent arg is not a non-blank pure number or there are apparently
			// only 2 args present (i.e. 1 delimiter in the arg list), assume the command is being
			// used in its 1-parameter mode:
			if (delimiter_count <= 1) // 2 parameters or less.
				// Force it to be 1-param mode.  In other words, we want to make MsgBox a very forgiving
				// command and have it rarely if ever report syntax errors:
				max_params_override = 1;
			else // It has more than 3 apparent params, but is the first param even numeric?
			{
				*delimiter[0] = '\0'; // Temporarily terminate action_args at the first delimiter.
				// Note: If it's a number inside a variable reference, it's still considered 1-parameter
				// mode to avoid ambiguity (unlike the new deref checking for param #4 mentioned below,
				// there seems to be too much ambiguity in this case to justify trying to figure out
				// if the first parameter is a pure deref, and thus that the command should use
				// 3-param or 4-param mode instead).
				if (!IsPureNumeric(action_args)) // No floats allowed.  Allow all-whitespace for aut2 compatibility.
					max_params_override = 1;
				*delimiter[0] = g_delimiter; // Restore the string.
				if (!max_params_override)
				{
					// IMPORATANT: The MsgBox cmd effectively has 3 parameter modes:
					// 1-parameter (where all commas in the 1st parameter are automatically literal)
					// 3-parameter (where all commas in the 3rd parameter are automatically literal)
					// 4-parameter (whether the 4th parameter is the timeout value)
					// Thus, the below must be done in a way that recognizes & supports all 3 modes.
					// The above has determined that the cmd isn't in 1-parameter mode.
					// If at this point it has exactly 3 apparent params, allow the command to be
					// processed normally without an override.  Otherwise, do more checking:
					if (delimiter_count == 3) // i.e. 3 delimiters, which means 4 params.
					{
						// If the 4th parameter isn't blank or pure numeric (i.e. even if it's a pure
						// deref, since trying to figure out what's a pure deref is somewhat complicated
						// at this early stage of parsing), assume the user didn't intend it to be the
						// MsgBox timeout (since that feature is rarely used), instead intending it
						// to be part of parameter #3.
						if (!IsPureNumeric(delimiter[2] + 1, false, true, true))
						{
							// Not blank and not a int or float.  Update for v1.0.20: Check if it's a
							// single deref.  If so, assume that deref contains the timeout and thus
							// 4-param mode is in effect.  This allows the timeout to be contained in
							// a variable, which was requested by one user:
							char *cp = omit_leading_whitespace(delimiter[2] + 1);
							// Relies on short-circuit boolean order:
							if (*cp != g_DerefChar || literal_map[cp - action_args]) // not a proper deref char.
								max_params_override = 3;
							// else since it does start with a real deref symbol, it must end with one otherwise
							// that will be caught later on as a syntax error anyway.  Therefore, don't override
							// max_params, just let it be parsed as 4 parameters.
						}
						// If it has more than 4 params or it has exactly 4 but the 4th isn't blank,
						// pure numeric, or a deref: assume it's being used in 3-parameter mode and
						// that all the other delimiters were intended to be literal.
					}
					else if (delimiter_count > 3) // i.e. 4 or more delimiters, which means 5 or more params.
						// Since it has too many delimiters to be 4-param mode, Assume it's 3-param mode
						// so that non-escaped commas in parameters 4 and beyond will be all treated as
						// strings that are part of parameter #3.
						max_params_override = 3;
					//else if 3 params or less: Don't override via max_params_override, just parse it normally.
				}
			}
		}
	} // end of special handling for MsgBox.


	/////////////////////////////////////////////////////////////
	// Parse the parameter string into a list of separate params.
	/////////////////////////////////////////////////////////////
	// MaxParams has already been verified as being <= MAX_ARGS.
	// Any g_delimiter-delimited items beyond MaxParams will be included in a lump inside the last param:
	int nArgs, nArgs_plus_one;
	char *arg[MAX_ARGS], *arg_map[MAX_ARGS];
	ActionTypeType subaction_type = ACT_INVALID; // Must init these.
	ActionTypeType suboldaction_type = OLD_INVALID;
	char subaction_name[MAX_VAR_NAME_LENGTH + 1], *subaction_end_marker = NULL, *subaction_start = NULL;
	int max_params = max_params_override ? max_params_override
		: (mIsAutoIt2 ? (this_action.MaxParamsAu2WithHighBit & 0x7F) // 0x7F removes the high-bit from consideration; that bit is used for an unrelated purpose.
			: this_action.MaxParams);
	int max_params_minus_one = max_params - 1;
	bool is_expression;
	ActionTypeType *np;

	for (nArgs = mark = 0; action_args[mark] && nArgs < max_params; ++nArgs)
	{
		if (nArgs == 2) // i.e. the 3rd arg is about to be added.
		{
			switch (aActionType) // will be ACT_INVALID if this_action is an old-style command.
			{
			case ACT_IFWINEXIST:
			case ACT_IFWINNOTEXIST:
			case ACT_IFWINACTIVE:
			case ACT_IFWINNOTACTIVE:
				subaction_start = action_args + mark;
				if (subaction_end_marker = ParseActionType(subaction_name, subaction_start, false))
					if (   !(subaction_type = ConvertActionType(subaction_name))   )
						suboldaction_type = ConvertOldActionType(subaction_name);
				break;
			}
			if (subaction_type || suboldaction_type)
				// A valid command was found (i.e. AutoIt2-style) in place of this commands Exclude Title
				// parameter, so don't add this item as a param to the command.
				break;
		}
		arg[nArgs] = action_args + mark;
		arg_map[nArgs] = literal_map + mark;
		if (nArgs == max_params_minus_one)
		{
			// Don't terminate the last param, just put all the rest of the line
			// into it.  This avoids the need for the user to escape any commas
			// that may appear in the last param.  i.e. any commas beyond this
			// point can't be delimiters because we've already reached MaxArgs
			// for this command:
			++nArgs;
			break;
		}
		// The above does not need the in_quotes and in_parens checks because commas in the last arg
		// are always literal, so there's no problem even in expressions.

		// The following implements the "% " prefix as a means of forcing an expression:
		is_expression = *arg[nArgs] == g_DerefChar && !*arg_map[nArgs] // It's a non-literal deref character.
			&& IS_SPACE_OR_TAB(arg[nArgs][1]); // Followed by a space or tab.

		// Find the end of the above arg:
		for (in_quotes = false, open_parens = 0; action_args[mark]; ++mark)
		{
			switch (action_args[mark])
			{
			case '"':
				// The simple method below is sufficient for our purpose even if a quoted string contains
				// pairs of double-quotes to represent a single literal quote, e.g. "quoted ""word""".
				// In other words, it relies on the fact that there must be an even number of quotes
				// inside any mandatory-numeric arg that is an expression such as x=="red,blue"
				in_quotes = !in_quotes;
				break;
			case '(':
				if (!in_quotes) // Literal parentheses inside a quoted string should not be counted for this purpose.
					++open_parens;
				break;
			case ')':
				if (!in_quotes)
					--open_parens;
				break;
			}

			if (action_args[mark] == g_delimiter && !literal_map[mark])  // A non-literal delimiter (unless its within double-quotes of a mandatory-numeric arg) is a match.
			{
				// If we're inside a pair of quotes or parentheses and this arg is known to be an expression, this
				// delimiter is part this arg and thus not to be used as a delimiter between command args:
				if (in_quotes || open_parens > 0)
				{
					if (is_expression)
						continue;
					if (aActionType == ACT_TRANSFORM && (nArgs == 2 || nArgs == 3)) // i.e. the 3rd or 4th arg is about to be added.
					{
						// Somewhat inefficient in the case where it has to be called for both Arg#2 and Arg#3,
						// but that is pretty rare.  Overall, expressions and quoted strings in these args
						// is rare too, so the inefficiency of redundant calls to ConvertTransformCmd() is
						// very small on average, and seems worth the benefit in terms of code simplification.
						// Note that the following might return TRANS_CMD_INVALID just because the sub-command
						// is containined in a variable reference.  That is why TRANS_CMD_INVALID does not
						// produce an error at this stage, but only later when the line has been constructed
						// far enough to call ArgHasDeref():
						// i.e. Not the first param, only the third and fourth, which currently are either both numeric or both non-numeric for all cases.
						switch(Line::ConvertTransformCmd(arg[1])) // arg[1] is the second arg.
						{
						// See comment above for why TRANS_CMD_INVALID isn't yet reported as an error:
						#define TRANSFORM_NON_EXPRESSION_CASES \
						case TRANS_CMD_INVALID:\
						case TRANS_CMD_ASC:\
						case TRANS_CMD_UNICODE:\
						case TRANS_CMD_DEREF:\
						case TRANS_CMD_HTML:\
							break; // Do nothing.  Leave this_new_arg.is_expression set to its default of false.
						TRANSFORM_NON_EXPRESSION_CASES
						default:
							// For all other sub-commands, Arg #3 and #4 are expression-capable.  It doesn't
							// seem necessary to call LegacyArgIsExpression() because the mere fact that
							// we're inside a pair of quotes or parentheses seems enough to indicate that this
							// really is an expression.
							continue;
						}
					}
					// v1.0.43.07: Fixed below to use this_action instead of g_act[aActionType] so that the
					// numeric params of legacy commands like EnvAdd/Sub/LeftClick can be detected.  Without
					// this fix, the last comma in a line like "EnvSub, var, Add(2, 3)" is seen as a parameter
					// delimiter, which causes a loadtime syntax error.
					if (np = this_action.NumericParams) // This command has at least one numeric parameter.
					{
						// As of v1.0.25, pure numeric parameters can optionally be numeric expressions, so check for that:
						nArgs_plus_one = nArgs + 1;
						for (; *np; ++np)
							if (*np == nArgs_plus_one) // This arg is enforced to be purely numeric.
								break;
						if (*np) // Match found, so this is a purely numeric arg.
							continue; // This delimiter is disqualified, so look for the next one.
					}
				} // if in quotes or parentheses
				// Since above didn't "continue", this is a real delimiter.
				action_args[mark] = '\0';  // Terminate the previous arg.
				// Trim any whitespace from the previous arg.  This operation
				// will not alter the contents of anything beyond action_args[i],
				// so it should be safe.  In addition, even though it changes
				// the contents of the arg[nArgs] substring, we don't have to
				// update literal_map because the map is still accurate due
				// to the nature of rtrim).  UPDATE: Note that this version
				// of rtrim() specifically avoids trimming newline characters,
				// since the user may have included literal newlines at the end
				// of the string by using an escape sequence:
				rtrim(arg[nArgs]);
				// Omit the leading whitespace from the next arg:
				for (++mark; IS_SPACE_OR_TAB(action_args[mark]); ++mark);
				// Now <mark> marks the end of the string, the start of the next arg,
				// or a delimiter-char (if the next arg is blank).
				break;  // Arg was found, so let the outer loop handle it.
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////////
	// Ensure there are sufficient parameters for this command.  Note: If MinParams
	// is greater than 0, the param numbers 1 through MinParams are required to be
	// non-blank.
	///////////////////////////////////////////////////////////////////////////////
	char error_msg[1024];
	if (nArgs < this_action.MinParams)
	{
		snprintf(error_msg, sizeof(error_msg), "\"%s\" requires at least %d parameter%s."
			, this_action.Name, this_action.MinParams
			, this_action.MinParams > 1 ? "s" : "");
		return ScriptError(error_msg, aLineText);
	}
	for (int i = 0; i < this_action.MinParams; ++i) // It's only safe to do this after the above.
		if (!*arg[i])
		{
			snprintf(error_msg, sizeof(error_msg), "\"%s\" requires that parameter #%u be non-blank."
				, this_action.Name, i + 1);
			return ScriptError(error_msg, aLineText);
		}

	////////////////////////////////////////////////////////////////////////
	// Handle legacy commands that are supported for backward compatibility.
	////////////////////////////////////////////////////////////////////////
	if (aOldActionType)
	{
		switch(aOldActionType)
		{
		case OLD_LEFTCLICK:
		case OLD_RIGHTCLICK:
			// Insert an arg at the beginning of the list to indicate the mouse button.
			arg[2] = arg[1];  arg_map[2] = arg_map[1];
			arg[1] = arg[0];  arg_map[1] = arg_map[0];
			arg[0] = const_cast<char*>(aOldActionType == OLD_LEFTCLICK ? "" : "Right");  arg_map[0] = NULL; // "" is treated the same as "Left"
			return AddLine(ACT_MOUSECLICK, arg, ++nArgs, arg_map);
		case OLD_LEFTCLICKDRAG:
		case OLD_RIGHTCLICKDRAG:
			// Insert an arg at the beginning of the list to indicate the mouse button.
			arg[4] = arg[3];  arg_map[4] = arg_map[3]; // Set the 5th arg to be the 4th, etc.
			arg[3] = arg[2];  arg_map[3] = arg_map[2];
			arg[2] = arg[1];  arg_map[2] = arg_map[1];
			arg[1] = arg[0];  arg_map[1] = arg_map[0];
			arg[0] = const_cast<char*>((aOldActionType == OLD_LEFTCLICKDRAG) ? "Left" : "Right");  arg_map[0] = NULL;
			return AddLine(ACT_MOUSECLICKDRAG, arg, ++nArgs, arg_map);
		case OLD_HIDEAUTOITWIN:
			// This isn't a perfect mapping because the word "on" or "off" might be contained
			// in a variable reference, in which case this conversion will be incorrect.
			// However, variable ref. is exceedingly rare.
			arg[1] = const_cast<char*>(stricmp(arg[0], "On") ? "Icon" : "NoIcon");
			arg[0] = "Tray"; // Assign only after we're done using the old arg[0] value above.
			return AddLine(ACT_MENU, arg, 2, arg_map);
		case OLD_REPEAT:
			if (!AddLine(ACT_REPEAT, arg, nArgs, arg_map))
				return FAIL;
			// For simplicity, always enclose repeat-loop's contents in in a block rather
			// than trying to detect if it has only one line:
			return AddLine(ACT_BLOCK_BEGIN);
		case OLD_ENDREPEAT:
			return AddLine(ACT_BLOCK_END);
		case OLD_WINGETACTIVETITLE:
			arg[nArgs] = "A";  arg_map[nArgs] = NULL; // "A" signifies the active window.
			++nArgs;
			return AddLine(ACT_WINGETTITLE, arg, nArgs, arg_map);
		case OLD_WINGETACTIVESTATS:
		{
			// Convert OLD_WINGETACTIVESTATS into *two* new commands:
			// Command #1: WinGetTitle, OutputVar, A
			char *width = arg[1];  // Temporary placeholder.
			arg[1] = "A";  arg_map[1] = NULL;  // Signifies the active window.
			if (!AddLine(ACT_WINGETTITLE, arg, 2, arg_map))
				return FAIL;
			// Command #2: WinGetPos, XPos, YPos, Width, Height, A
			// Reassign args in the new command's ordering.  These lines must occur
			// in this exact order for the copy to work properly:
			arg[0] = arg[3];  arg_map[0] = arg_map[3];  // xpos
			arg[3] = arg[2];  arg_map[3] = arg_map[2];  // height
			arg[2] = width;   arg_map[2] = arg_map[1];  // width
			arg[1] = arg[4];  arg_map[1] = arg_map[4];  // ypos
			arg[4] = "A";  arg_map[4] = NULL;  // "A" signifies the active window.
			return AddLine(ACT_WINGETPOS, arg, 5, arg_map);
		}

		case OLD_SETENV:
			return AddLine(ACT_ASSIGN, arg, nArgs, arg_map);
		case OLD_ENVADD:
			return AddLine(ACT_ADD, arg, nArgs, arg_map);
		case OLD_ENVSUB:
			return AddLine(ACT_SUB, arg, nArgs, arg_map);
		case OLD_ENVMULT:
			return AddLine(ACT_MULT, arg, nArgs, arg_map);
		case OLD_ENVDIV:
			return AddLine(ACT_DIV, arg, nArgs, arg_map);

		// For these, break rather than return so that further processing can be done:
		case OLD_IFEQUAL:
			aActionType = ACT_IFEQUAL;
			break;
		case OLD_IFNOTEQUAL:
			aActionType = ACT_IFNOTEQUAL;
			break;
		case OLD_IFGREATER:
			aActionType = ACT_IFGREATER;
			break;
		case OLD_IFGREATEROREQUAL:
			aActionType = ACT_IFGREATEROREQUAL;
			break;
		case OLD_IFLESS:
			aActionType = ACT_IFLESS;
			break;
		case OLD_IFLESSOREQUAL:
			aActionType = ACT_IFLESSOREQUAL;
			break;
#ifdef _DEBUG
		default:
			return ScriptError("DEBUG: Unhandled Old-Command.", action_name);
#endif
		} // switch()
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////
	// Handle AutoIt2-style IF-statements (i.e. the IF's action is on the same line as the condition).
	//////////////////////////////////////////////////////////////////////////////////////////////////
	// The check below: Don't bother if this IF (e.g. IfWinActive) has zero params or if the
	// subaction was already found above:
	if (nArgs && !subaction_type && !suboldaction_type && ACT_IS_IF_OLD(aActionType, aOldActionType))
	{
		char *delimiter;
		char *last_arg = arg[nArgs - 1];
		for (mark = (int)(last_arg - action_args); action_args[mark]; ++mark)
		{
			if (action_args[mark] == g_delimiter && !literal_map[mark])  // Match found: a non-literal delimiter.
			{
				delimiter = action_args + mark; // save the location of this delimiter
				// Omit the leading whitespace from the next arg:
				for (++mark; IS_SPACE_OR_TAB(action_args[mark]); ++mark);
				// Now <mark> marks the end of the string, the start of the next arg,
				// or a delimiter-char (if the next arg is blank).
				subaction_start = action_args + mark;
				if (subaction_end_marker = ParseActionType(subaction_name, subaction_start, false))
				{
					if (   !(subaction_type = ConvertActionType(subaction_name))   )
						suboldaction_type = ConvertOldActionType(subaction_name);
					if (subaction_type || suboldaction_type) // A valid sub-action (command) was found.
					{
						// Remove this subaction from its parent line; we want it separate:
						*delimiter = '\0';
						rtrim(last_arg);
					}
					// else leave it as-is, i.e. as part of the last param, because the delimiter
					// found above is probably being used as a literal char even though it isn't
					// escaped, e.g. "ifequal, var1, string with embedded, but non-escaped, commas"
				}
				// else, do nothing; reasoning perhaps similar to above comment.
				break;
			}
		}
	}

	// In v1.0.41, the following one-true-brace styles are also supported:
	// Loop {   ; Known limitation: Overlaps with file-pattern loop that retrieves single file of name "{".
	// Loop 5 { ; Also overlaps, this time with file-pattern loop that retrieves numeric filename ending in '{'.
	// Loop %Var% {  ; Similar, but like the above seems acceptable given extreme rarity of user intending a file pattern.
	if (aActionType == ACT_LOOP && nArgs == 1 && arg[0][0])  // A loop with exactly one, non-blank arg.
	{
		char *arg1 = arg[0]; // For readability and possibly performance.
		// A loop with the above criteria (exactly one arg) can only validly be a normal/counting loop or
		// a file-pattern loop if its parameter's last character is '{'.  For the following reasons, any
		// single-parameter loop that ends in '{' is considered to be one-true brace:
		// 1) Extremely rare that a file-pattern loop such as "Loop filename {" would ever be used,
		//    and even if it is, the syntax checker will report an unclosed block, making it apparent
		//    to the user that a workaround is needed, such as putting the filename into a variable first.
		// 2) Difficulty and code size of distinguishing all possible valid-one-true-braces from those
		//    that aren't.  For example, the following are ambiguous, so it seems best for consistency
		//    and code size reduction just to treat them as one-truce-brace, which will immediately alert
		//    the user if the brace isn't closed:
		//    a) Loop % (expression) {   ; Ambiguous because expression could resolve to a string, thus it would be seen as a file-pattern loop.
		//    b) Loop %Var% {            ; Similar as above, which means all three of these unintentionally support
		//    c) Loop filename{          ; OTB for some types of file loops because it's not worth the code size to "unsupport" them.
		//    d) Loop *.txt {            ; Like the above: Unintentionally supported, but not documnented.
		//
		// Insist that no characters follow the '{' in case the user intended it to be a file-pattern loop
		// such as "Loop {literal-filename".
		char *arg1_last_char = arg1 + strlen(arg1) - 1;
		if (*arg1_last_char == '{')
		{
			add_openbrace_afterward = true;
			*arg1_last_char = '\0';  // Since it will be fully handled here, remove the brace from further consideration.
			if (!rtrim(arg1)) // Trimmed down to nothing, so only a brace was present: remove the arg completely.
				nArgs = 0;    // This makes later stages recognize it as an infinite loop rather than a zero-iteration loop.
		}
	}

	if (!AddLine(aActionType, arg, nArgs, arg_map))
		return FAIL;
	if (add_openbrace_afterward)
		if (!AddLine(ACT_BLOCK_BEGIN))
			return FAIL;
	if (!subaction_type && !suboldaction_type) // There is no subaction in this case.
		return OK;
	// Otherwise, recursively add the subaction, and any subactions it might have, beneath
	// the line just added.  The following example:
	// IfWinExist, x, y, IfWinNotExist, a, b, Gosub, Sub1
	// would break down into these lines:
	// IfWinExist, x, y
	//    IfWinNotExist, a, b
	//       Gosub, Sub1
	return ParseAndAddLine(subaction_start, subaction_type, suboldaction_type, subaction_name, subaction_end_marker
		, literal_map + (subaction_end_marker - action_args) // Pass only the relevant substring of literal_map.
		, strlen(subaction_end_marker));
}



inline char *Script::ParseActionType(char *aBufTarget, char *aBufSource, bool aDisplayErrors)
// inline since it's called so often.
// aBufTarget should be at least MAX_VAR_NAME_LENGTH + 1 in size.
// Returns NULL if a failure condition occurs; otherwise, the address of the last
// character of the action name in aBufSource.
{
	////////////////////////////////////////////////////////
	// Find the action name and the start of the param list.
	////////////////////////////////////////////////////////
	// Allows the delimiter between action-type-name and the first param to be optional by
	// relying on the fact that action-type-names can't contain spaces. Find first char in
	// aLineText that is a space, a delimiter, or a tab. Also search for operator symbols
	// so that assignments and IFs without whitespace are supported, e.g. var1=5,
	// if var2<%var3%.  Not static in case g_delimiter is allowed to vary:
	DEFINE_END_FLAGS
	char *end_marker = StrChrAny(aBufSource, end_flags);
	if (end_marker) // Found a delimiter.
	{
		if (*end_marker == '=' && end_marker > aBufSource && end_marker[-1] == '.') // Relies on short-circuit boolean order.
			--end_marker; // v1.0.46.01: Support .=, but not any use of '.' because that is reserved as a struct/member operator.
		if (end_marker > aBufSource) // The delimiter isn't very first char in aBufSource.
			--end_marker;
		// else we allow it to be the first char to support "++i" etc.
	}
	else // No delimiter found, so set end_marker to the location of the last char in string.
		end_marker = aBufSource + strlen(aBufSource) - 1;
	// Now end_marker is the character just prior to the first delimiter or whitespace,
	// or (in the case of ++ and --) the first delimiter itself.  Find the end of
	// the action-type name by omitting trailing whitespace:
	end_marker = omit_trailing_whitespace(aBufSource, end_marker);
	// If first char in aBufSource is a delimiter, action_name will consist of just that first char:
	size_t action_name_length = end_marker - aBufSource + 1;
	if (action_name_length > MAX_VAR_NAME_LENGTH)
	{
		if (aDisplayErrors)
			ScriptError(ERR_UNRECOGNIZED_ACTION, aBufSource); // Short/vague message since so rare.
		return NULL;
	}
	strlcpy(aBufTarget, aBufSource, action_name_length + 1);
	return end_marker;
}



inline ActionTypeType Script::ConvertActionType(char *aActionTypeString)
// inline since it's called so often, but don't keep it in the .h due to #include issues.
{
	// For the loop's index:
	// Use an int rather than ActionTypeType since it's sure to be large enough to go beyond
	// 256 if there happen to be exactly 256 actions in the array:
 	for (int action_type = ACT_FIRST_COMMAND; action_type < g_ActionCount; ++action_type)
		if (!stricmp(aActionTypeString, g_act[action_type].Name)) // Match found.
			return action_type;
	return ACT_INVALID;  // On failure to find a match.
}



inline ActionTypeType Script::ConvertOldActionType(char *aActionTypeString)
// inline since it's called so often, but don't keep it in the .h due to #include issues.
{
 	for (int action_type = OLD_INVALID + 1; action_type < g_OldActionCount; ++action_type)
		if (!stricmp(aActionTypeString, g_old_act[action_type].Name)) // Match found.
			return action_type;
	return OLD_INVALID;  // On failure to find a match.
}



bool LegacyArgIsExpression(char *aArgText, char *aArgMap)
// Helper function for AddLine
{
	// The section below is here in light of rare legacy cases such as the below:
	// -%y%   ; i.e. make it negative.
	// +%y%   ; might happen with up/down adjustments on SoundSet, GuiControl progress/slider, etc?
	// Although the above are detected as non-expressions and thus non-double-derefs,
	// the following are not because they're too rare or would sacrifice too much flexibility:
	// 1%y%.0 ; i.e. at a tens/hundreds place and make it a floating point.  In addition,
	//          1%y% could be an array, so best not to tag that as non-expression.
	//          For that matter, %y%.0 could be an obscure kind of reverse-notation array itself.
	//          However, as of v1.0.29, things like %y%000 are allowed, e.g. Sleep %Seconds%000
	// 0x%y%  ; i.e. make it hex (too rare to check for, plus it could be an array).
	// %y%%z% ; i.e. concatenate two numbers to make a larger number (too rare to check for)
	char *cp = aArgText + (*aArgText == '-' || *aArgText == '+'); // i.e. +1 if second term evaluates to true.
	return *cp != g_DerefChar // If no deref, for simplicity assume it's an expression since any such non-numeric item would be extremely rare in pre-expression era.
		|| !aArgMap || *(aArgMap + (cp != aArgText)) // There's no literal-map or this deref char is not really a deref char because it's marked as a literal.
		|| !(cp = strchr(cp + 1, g_DerefChar)) // There is no next deref char.
		|| (cp[1] && !IsPureNumeric(cp + 1, false, true, true)); // But that next deref char is not the last char, which means this is not a single isolated deref. v1.0.29: Allow things like Sleep %Var%000.
		// Above does not need to check whether last deref char is marked literal in the
		// arg map because if it is, it would mean the first deref char lacks a matching
		// close-symbol, which will be caught as a syntax error below regardless of whether
		// this is an expression.
}



ResultType Script::AddLine(ActionTypeType aActionType, char *aArg[], ArgCountType aArgc, char *aArgMap[])
// aArg must be a collection of pointers to memory areas that are modifiable, and there
// must be at least aArgc number of pointers in the aArg array.  In v1.0.40, a caller (namely
// the "macro expansion" for remappings such as "a::b") is allowed to pass a non-NULL value for
// aArg but a NULL value for aArgMap.
// Returns OK or FAIL.
{
#ifdef _DEBUG
	if (aActionType == ACT_INVALID)
		return ScriptError("DEBUG: BAD AddLine", const_cast<char*>(aArgc > 0 ? aArg[0] : ""));
#endif

	bool do_update_labels;
	if (!aArg && aArgc == UCHAR_MAX) // Special signal from caller to avoid pointing any pending labels to this particular line.
	{
		aArgc = 0;
		do_update_labels = false;
	}
	else
		do_update_labels = true;

	Var *target_var;
	DerefType deref[MAX_DEREFS_PER_ARG];  // Will be used to temporarily store the var-deref locations in each arg.
	int deref_count;  // How many items are in deref array.
	ArgStruct *new_arg;  // We will allocate some dynamic memory for this, then hang it onto the new line.
	size_t operand_length;
	char *op_begin, *op_end, orig_char;
	char *this_aArgMap, *this_aArg, *cp;
	int open_parens;
	ActionTypeType *np;
	TransformCmds trans_cmd;
	bool is_function;

	//////////////////////////////////////////////////////////
	// Build the new arg list in dynamic memory.
	// The allocated structs will be attached to the new line.
	//////////////////////////////////////////////////////////
	if (!aArgc)
		new_arg = NULL;  // Just need an empty array in this case.
	else
	{
		if (   !(new_arg = (ArgStruct *)SimpleHeap::Malloc(aArgc * sizeof(ArgStruct)))   )
			return ScriptError(ERR_OUTOFMEM);

		int i, j, i_plus_one;
		bool in_quotes;

		for (i = 0; i < aArgc; ++i)
		{
			////////////////
			// FOR EACH ARG:
			////////////////
			this_aArg = aArg[i];                        // For performance and convenience.
			this_aArgMap = aArgMap ? aArgMap[i] : NULL; // Same.
			ArgStruct &this_new_arg = new_arg[i];       // Same.
			this_new_arg.is_expression = false;         // Set default early, for maintainability.

			if (aActionType == ACT_TRANSFORM)
			{
				if (i == 1) // The second parameter (since the first is the OutputVar).
					// Note that the following might return TRANS_CMD_INVALID just because the sub-command
					// is containined in a variable reference.  That is why TRANS_CMD_INVALID does not
					// produce an error at this stage, but only later when the line has been constructed
					// far enough to call ArgHasDeref():
					trans_cmd = Line::ConvertTransformCmd(this_aArg);
					// The value of trans_cmd is also used by the syntax checker further below.
				else if (i > 1) // i.e. Not the first param, only the third and fourth, which currently are either both numeric or both non-numeric for all cases.
				{
					switch(trans_cmd)
					{
					TRANSFORM_NON_EXPRESSION_CASES
					default:
						// For all other sub-commands, Arg #3 and #4 are expression-capable and will be made so
						// if they pass the following check:
						this_new_arg.is_expression = LegacyArgIsExpression(this_aArg, this_aArgMap);
					}
				}
			}

			// Before allocating memory for this Arg's text, first check if it's a pure
			// variable.  If it is, we store it differently (and there's no need to resolve
			// escape sequences in these cases, since var names can't contain them):
			if (aActionType == ACT_LOOP && i == 1 && aArg[0] && !stricmp(aArg[0], "Parse")) // Verified.
				// i==1 --> 2nd arg's type is based on 1st arg's text.
				this_new_arg.type = ARG_TYPE_INPUT_VAR;
			else
				this_new_arg.type = Line::ArgIsVar(aActionType, i);
			// Since some vars are optional, the below allows them all to be blank or
			// not present in the arg list.  If a mandatory var is blank at this stage,
			// it's okay because all mandatory args are validated to be non-blank elsewhere:
			if (this_new_arg.type != ARG_TYPE_NORMAL)
			{
				if (!*this_aArg)
					// An optional input or output variable has been omitted, so indicate
					// that this arg is not a variable, just a normal empty arg.  Functions
					// such as ListLines() rely on this having been done because they assume,
					// for performance reasons, that args marked as variables really are
					// variables.  In addition, ExpandArgs() relies on this having been done
					// as does the load-time validation for ACT_DRIVEGET:
					this_new_arg.type = ARG_TYPE_NORMAL;
				else
				{
					// Does this input or output variable contain a dereference?  If so, it must
					// be resolved at runtime (to support arrays, etc.).
					// Find the first non-escaped dereference symbol:
					for (j = 0; this_aArg[j] && (this_aArg[j] != g_DerefChar || (this_aArgMap && this_aArgMap[j])); ++j);
					if (!this_aArg[j])
					{
						// A non-escaped deref symbol wasn't found, therefore this variable does not
						// appear to be something that must be resolved dynamically at runtime.
						if (   !(target_var = FindOrAddVar(this_aArg))   )
							return FAIL;  // The above already displayed the error.
						// If this action type is something that modifies the contents of the var, ensure the var
						// isn't a special/reserved one:
						if (this_new_arg.type == ARG_TYPE_OUTPUT_VAR && VAR_IS_READONLY(*target_var))
							return ScriptError(ERR_VAR_IS_READONLY, this_aArg);
						// Rather than removing this arg from the list altogether -- which would distrub
						// the ordering and hurt the maintainability of the code -- the next best thing
						// in terms of saving memory is to store an empty string in place of the arg's
						// text if that arg is a pure variable (i.e. since the name of the variable is already
						// stored in the Var object, we don't need to store it twice):
						this_new_arg.text = "";
						this_new_arg.length = 0;
						this_new_arg.deref = (DerefType *)target_var;
						continue;
					}
					// else continue on to the below so that this input or output variable name's dynamic part
					// (e.g. array%i%) can be partially resolved.
				}
			}
			else // this_new_arg.type == ARG_TYPE_NORMAL (excluding those input/output_vars that were converted to normal because they were blank, above).
			{
				// v1.0.29: Allow expressions in any parameter that starts with % followed by a space
				// or tab. This should be unambiguous because spaces and tabs are illegal in variable names.
				// Since there's little if any benefit to allowing input and output variables to be
				// dynamically built via expression, for now it is disallowed.  If ever allow it,
				// need to review other sections to ensure they will tolerate it.  Also, the following
				// would probably need revision to get it to be detected as an output-variable:
				// % Array%i% = value
				if (*this_aArg == g_DerefChar && !(this_aArgMap && *this_aArgMap) // It's a non-literal deref character.
					&& IS_SPACE_OR_TAB(this_aArg[1])) // Followed by a space or tab.
				{
					this_new_arg.is_expression = true;
					// Omit the percent sign and the space after it from further consideration.
					this_aArg += 2;
					if (this_aArgMap)
						this_aArgMap += 2;
					// ACT_ASSIGN isn't capable of dealing with expressions because ExecUntil() does not
					// call ExpandArgs() automatically for it.  Thus its function, PerformAssign(), would
					// not be given the expanded result of the expression.
					if (aActionType == ACT_ASSIGN)
						aActionType = ACT_ASSIGNEXPR;
				}
			}

			// Below will set the new var to be the constant empty string if the
			// source var is NULL or blank.
			// e.g. If WinTitle is unspecified (blank), but WinText is non-blank.
			// Using empty string is much safer than NULL because these args
			// will be frequently accessed by various functions that might
			// not be equipped to handle NULLs.  Rather than having to remember
			// to check for NULL in every such case, setting it to a constant
			// empty string here should make things a lot more maintainable
			// and less bug-prone.  If there's ever a need for the contents
			// of this_new_arg to be modifiable (perhaps some obscure API calls require
			// modifiable strings?) can malloc a single-char to contain the empty string.
			//
			// So that it can be passed to Malloc(), first update the length to match what the text will be
			// (if the alloc fails, an inaccurate length won't matter because it's an program-abort situation).
			// The length must fit into a WORD, which it will since each arg is literal text from a script's line,
			// which is limited to LINE_SIZE. The length member was added in v1.0.44.14 to boost runtime performance.
			this_new_arg.length = (WORD)strlen(this_aArg);
			if (   !(this_new_arg.text = SimpleHeap::Malloc(this_aArg, this_new_arg.length))   )
				return FAIL;  // It already displayed the error for us.

			////////////////////////////////////////////////////
			// Build the list of dereferenced vars for this arg.
			////////////////////////////////////////////////////
			// Now that any escaped g_DerefChars have been marked, scan new_arg.text to
			// determine where the variable dereferences are (if any).  In addition to helping
			// runtime performance, this also serves to validate the script at load-time
			// so that some errors can be caught early.  Note: this_new_arg.text is scanned rather
			// than this_aArg because we want to establish pointers to the correct area of
			// memory:
			deref_count = 0;  // Init for each arg.

			if (np = g_act[aActionType].NumericParams) // This command has at least one numeric parameter.
			{
				// As of v1.0.25, pure numeric parameters can optionally be numeric expressions, so check for that:
				i_plus_one = i + 1;
				for (; *np; ++np)
				{
					if (*np == i_plus_one) // This arg is enforced to be purely numeric.
					{
						if (aActionType == ACT_WINMOVE)
						{
							if (i > 1)
							{
								// i indicates this is Arg #3 or beyond, which is one of the args that is
								// either the word "default" or a number/expression.
								if (!stricmp(this_new_arg.text, "default")) // It's not an expression.
									break; // The loop is over because this arg was found in the list.
							}
							else // This is the first or second arg, which are title/text vs. X/Y when aArgc > 2.
								if (aArgc > 2) // Title/text are not numeric/expressions.
									break; // The loop is over because this arg was found in the list.
						}
						// Otherwise, it might be an expression so do the final checks.
						// Override the original false default of is_expression unless an exception applies.
						// Since ACT_ASSIGNEXPR is not a legacy command, none of the legacy exceptions need
						// to be applied to it.  For other commands, if any telltale character is present
						// it's definitely an expression and the complex check after this one isn't needed:
						if (aActionType == ACT_ASSIGNEXPR || StrChrAny(this_new_arg.text, EXPR_TELLTALES))
							this_new_arg.is_expression = true;
						else
							this_new_arg.is_expression = LegacyArgIsExpression(this_new_arg.text, this_aArgMap);
						break; // The loop is over if this arg is found in the list of mandatory-numeric args.
					} // i is a mandatory-numeric arg
				} // for each mandatory-numeric arg of this command, see if this arg matches its number.
			} // this command has a list of mandatory numeric-args.

			// To help runtime performance, the below changes an ACT_ASSIGNEXPR, ACT_TRANSFORM, and
			// perhaps others in the future, to become non-expressions if they contain only a single
			// numeric literal (or are entirely blank). At runtime, such args are expanded normally
			// rather than having to run them through the expression evaluator:
			if (this_new_arg.is_expression && IsPureNumeric(this_new_arg.text, true, true, true))
				this_new_arg.is_expression = false;

			if (this_new_arg.is_expression)
			{
				// Ensure parentheses are balanced:
				for (cp = this_new_arg.text, in_quotes = false, open_parens = 0; *cp; ++cp)
				{
					switch (*cp)
					{
					// The simple method below is sufficient for our purpose even if a quoted string contains
					// pairs of double-quotes to represent a single literal quote, e.g. "quoted ""word""".
					// In other words, it relies on the fact that there must be an even number of quotes
					// inside any mandatory-numeric arg that is an expression such as x=="red,blue"
					case '"':
						in_quotes = !in_quotes;
						break;
					case '(':
						if (!in_quotes) // Literal parentheses inside a quoted string should not be counted for this purpose.
							++open_parens;
						break;
					case ')':
						if (!in_quotes)
						{
							if (!open_parens)
								return ScriptError(ERR_MISSING_OPEN_PAREN, cp); // And indicate cp as the exact spot.
							--open_parens;
						}
						break;
					}
				}
				if (open_parens) // At least one '(' is never closed.
					return ScriptError(ERR_MISSING_CLOSE_PAREN, this_new_arg.text);

				#define ERR_EXP_ILLEGAL_CHAR "The leftmost character above is illegal in an expression." // "above" refers to the layout of the error dialog.
				// ParseDerefs() won't consider escaped percent signs to be illegal, but in this case
				// they should be since they have no meaning in expressions.  UPDATE for v1.0.44.11: The following
				// is now commented out because it causes false positives (and fixing that probably isn't worth the
				// performance & code size).  Specifically, the section below reports an error for escaped delimiters
				// inside quotes such as x := "`%".  More importantly, it defeats the continuation section's %
				// option; for example:
				//   MsgBox %
				//   (%  ; <<< This option here is defeated because it causes % to be replaced with `% at an early stage.
				//   "%"
				//   )
				//if (this_aArgMap) // This arg has an arg map indicating which chars are escaped/literal vs. normal.
				//	for (j = 0; this_new_arg.text[j]; ++j)
				//		if (this_aArgMap[j] && this_new_arg.text[j] == g_DerefChar)
				//			return ScriptError(ERR_EXP_ILLEGAL_CHAR, this_new_arg.text + j);

				// Resolve all operands that aren't numbers into variable references.  Doing this here at
				// load-time greatly improves runtime performance, especially for scripts that have a lot
				// of variables.
				for (op_begin = this_new_arg.text; *op_begin; op_begin = op_end)
				{
					if (*op_begin == '.' && op_begin[1] == '=') // v1.0.46.01: Support .=, but not any use of '.' because that is reserved as a struct/member operator.
						op_begin += 2;
					for (; *op_begin && strchr(EXPR_OPERAND_TERMINATORS, *op_begin); ++op_begin); // Skip over whitespace, operators, and parentheses.
					if (!*op_begin) // The above loop reached the end of the string: No operands remaining.
						break;

					// Now op_begin is the start of an operand, which might be a variable reference, a numeric
					// literal, or a string literal.  If it's a string literal, it is left as-is:
					if (*op_begin == '"')
					{
						// Find the end of this string literal, noting that a pair of double quotes is
						// a literal double quote inside the string:
						for (op_end = op_begin + 1;; ++op_end)
						{
							if (!*op_end)
								return ScriptError(ERR_MISSING_CLOSE_QUOTE, op_begin);
							if (*op_end == '"') // If not followed immediately by another, this is the end of it.
							{
								++op_end;
								if (*op_end != '"') // String terminator or some non-quote character.
									break;  // The previous char is the ending quote.
								//else a pair of quotes, which resolves to a single literal quote.
								// This pair is skipped over and the loop continues until the real end-quote is found.
							}
						}
						// op_end is now set correctly to allow the outer loop to continue.
						continue; // Ignore this literal string, letting the runtime expression parser recognize it.
					}

					// Find the end of this operand (if *op_end is '\0', strchr() will find that too):
					for (op_end = op_begin + 1; !strchr(EXPR_OPERAND_TERMINATORS, *op_end); ++op_end); // Find first whitespace, operator, or paren.
					if (*op_end == '=' && op_end[-1] == '.') // v1.0.46.01: Support .=, but not any use of '.' because that is reserved as a struct/member operator.
						--op_end;
					// Now op_end marks the end of this operand.  The end might be the zero terminator, an operator, etc.

					// Must be done only after op_end has been set above (since loop uses op_end):
					if (*op_begin == '.' && strchr(" \t=", op_begin[1])) // If true, it can't be something like "5." because the dot inside would never be parsed separately in that case.  Also allows ".=" operator.
						continue;
					//else any '.' not followed by a space, tab, or '=' is likely a number without a leading zero,
					// so continue on below to process it.

					operand_length = op_end - op_begin;

					// Check if it's AND/OR/NOT:
					if (operand_length < 4 && operand_length > 1) // Ordered for short-circuit performance.
					{
						if (operand_length == 2)
						{
							if ((*op_begin == 'o' || *op_begin == 'O') && (op_begin[1] == 'r' || op_begin[1] == 'R'))
							{	// "OR" was found.
								op_begin[0] = '|'; // v1.0.45: Transform into easier-to-parse symbols for improved
								op_begin[1] = '|'; // runtime performance and reduced code size.
								continue;
							}
						}
						else // operand_length must be 3
						{
							switch (*op_begin)
							{
							case 'a':
							case 'A':
								if (   (op_begin[1] == 'n' || op_begin[1] == 'N') // Relies on short-circuit boolean order.
									&& (op_begin[2] == 'd' || op_begin[2] == 'D')   )
								{	// "AND" was found.
									op_begin[0] = '&'; // v1.0.45: Transform into easier-to-parse symbols for
									op_begin[1] = '&'; // improved runtime performance and reduced code size.
									op_begin[2] = ' '; // A space is used lieu of the complexity of the below.
									// Above seems better than below even though below would make it look a little
									// nicer in ListLines.  BELOW CAN'T WORK because this_new_arg.deref[] can contain
									// offsets that would also need to be adjusted:
									//memmove(op_begin + 2, op_begin + 3, strlen(op_begin+3)+1 ... or some expression involving this_new_arg.length this_new_arg.text);
									//--this_new_arg.length;
									//--op_end; // Ensure op_end is set up properly for the for-loop's post-iteration action.
									continue;
								}
								break;

							case 'n': // v1.0.45: Unlike "AND" and "OR" above, this one is not given a substitute
							case 'N': // because it's not the same as the "!" operator. See SYM_LOWNOT for comments.
								if (   (op_begin[1] == 'o' || op_begin[1] == 'O') // Relies on short-circuit boolean order.
									&& (op_begin[2] == 't' || op_begin[2] == 'T')   )
									continue; // "NOT" was found.
								break;
							}
						}
					} // End of check for AND/OR/NOT.

					// Temporarily terminate, which avoids at least the below issue:
					// Two or more extremely long var names together could exceed MAX_VAR_NAME_LENGTH
					// e.g. LongVar%LongVar2% would be too long to store in a buffer of size MAX_VAR_NAME_LENGTH.
					// This seems pretty darn unlikely, but perhaps doubling it would be okay.
					// UPDATE: Above is now not an issue since caller's string is temporarily terminated rather
					// than making a copy of it.
					orig_char = *op_end;
					*op_end = '\0';

					// Illegal characters are legal when enclosed in double quotes.  So the following is
					// done only after the above has ensured this operand is not one enclosed entirely in
					// double quotes.
					// The following characters are either illegal in expressions or reserved for future use.
					// Rather than forbidding g_delimiter and g_DerefChar, it seems best to assume they are at
					// their default values for this purpose.  Otherwise, if g_delimiter is an operator, that
					// operator would then become impossible inside the expression.
					if (cp = StrChrAny(op_begin, EXPR_ILLEGAL_CHARS))
						return ScriptError(ERR_EXP_ILLEGAL_CHAR, cp);

					// Below takes care of recognizing hexadecimal integers, which avoids the 'x' character
					// inside of something like 0xFF from being detected as the name of a variable:
					if (   !IsPureNumeric(op_begin, true, false, true) // Not a numeric literal...
						&& !(*op_begin == '?' && !op_begin[1])   ) // ...and not an isolated '?' operator.  Relies on short-circuit boolean order.
					{
						is_function = (orig_char == '(');
						// This operand must be a variable/function reference or string literal, otherwise it's
						// a syntax error.
						// Check explicitly for derefs since the vast majority don't have any, and this
						// avoids the function call in those cases:
						if (strchr(op_begin, g_DerefChar)) // This operand contains at least one double dereference.
						{
							// v1.0.47.06: Dynamic function calls are now supported.
							//if (is_function)
							//	return ScriptError("Dynamic function calls are not supported.", op_begin);
							int first_deref = deref_count;

							// The derefs are parsed and added to the deref array at this stage (on a
							// per-operand basis) rather than all at once for the entire arg because
							// the deref array must be ordered according to the physical position of
							// derefs inside the arg.  In the following example, the order of derefs
							// must be x,i,y: if (x = Array%i% and y = 3)
							if (!ParseDerefs(op_begin, this_aArgMap ? this_aArgMap + (op_begin - this_new_arg.text) : NULL
								, deref, deref_count))
								return FAIL; // It already displayed the error.  No need to undo temp. termination.
							// And now leave this operand "raw" so that it will later be dereferenced again.
							// In the following example, i made into a deref but the result (Array33) must be
							// dereferenced during a second stage at runtime: if (x = Array%i%).

							if (is_function)
							{
								int param_count = 0;
								// Determine how many parameters there are.
								cp = omit_leading_whitespace(op_end + 1);
								if (*cp != ')')
								{
									int open_parens;
									bool in_quote = false;
									for (++param_count, open_parens = 1; *cp && open_parens; ++cp)
									{
										if (*cp == '"')
											in_quote = !in_quote;
										if (in_quote)
											continue;
										switch (*cp)
										{
										case '(':
											++open_parens;
											break;
										case ')':
											--open_parens;
											break;
										case ',':
											if (open_parens == 1)
												++param_count;
											break;
										}
									}
								}
								// Store param_count in the first deref. This will be picked up by the expression
								// infix processing code.
								deref[first_deref].param_count = param_count;
							}
						}
						else // This operand is a variable name or function name (single deref).
						{
							#define TOO_MANY_REFS "Too many var/func refs." // Short msg since so rare.
							if (deref_count >= MAX_DEREFS_PER_ARG)
								return ScriptError(TOO_MANY_REFS, op_begin); // Indicate which operand it ran out of space at.
							// Store the deref's starting location, even for functions (leave it set to the start
							// of the function's name for use when doing error reporting at other stages -- i.e.
							// don't set it to the address of the first param or closing-paren-if-no-params):
							deref[deref_count].marker = op_begin;
							deref[deref_count].length = (DerefLengthType)operand_length;
							if (deref[deref_count].is_function = is_function) // It's a function not a variable.
								// Set to NULL to catch bugs.  It must and will be filled in at a later stage
								// because the setting of each function's mJumpToLine relies upon the fact that
								// functions are added to the linked list only upon being formally defined
								// so that the most recently defined function is always last in the linked
								// list, awaiting its mJumpToLine that will appear beneath it.
								deref[deref_count].func = NULL;
							else // It's a variable (or a scientific-notation literal) rather than a function.
							{
								if (toupper(op_end[-1]) == 'E' && (orig_char == '+' || orig_char == '-') // Listed first for short-circuit performance with the below.
									&& strchr(op_begin, '.')) // v1.0.46.11: This item appears to be a scientific-notation literal with the OPTIONAL +/- sign PRESENT on the exponent (e.g. 1.0e+001), so check that before checking if it's a variable name.
								{
									*op_end = orig_char; // Undo the temporary termination.
									do // Skip over the sign and its exponent; e.g. the "+1" in "1.0e+1".  There must be a sign in this particular sci-notation number or we would never have arrived here.
										++op_end;
									while (*op_end >= '0' && *op_end <= '9'); // Avoid isdigit() because it sometimes causes a debug assertion failure at: (unsigned)(c + 1) <= 256 (probably only in debug mode), and maybe only when bad data got in it due to some other bug.
									// No need to do the following because a number can't validly be followed by the ".=" operator:
									//if (*op_end == '=' && op_end[-1] == '.') // v1.0.46.01: Support .=, but not any use of '.' because that is reserved as a struct/member operator.
									//	--op_end;
									continue; // Pure number, which doesn't need any processing at this stage.
								}
								// Since above didn't "continue", treat this item as a variable name:
								if (   !(deref[deref_count].var = FindOrAddVar(op_begin, operand_length))   )
									return FAIL; // The called function already displayed the error.
							}
							++deref_count; // Since above didn't "continue" or "return".
						}
					}
					//else purely numeric or '?'.  Do nothing since pure numbers and '?' don't need any
					// processing at this stage.
					*op_end = orig_char; // Undo the temporary termination.
				} // expression pre-parsing loop.

				// Now that the derefs have all been recognized above, simplify any special cases --
				// such as single isolated derefs -- to enhance runtime performance.
				// Make args that consist only of a quoted string-literal into non-expressions also:
				if (!deref_count && *this_new_arg.text == '"')
				{
					// It has no derefs (e.g. x:="string" or x:=1024*1024), but since it's a single
					// string literal, convert into a non-expression.  This is mainly for use by
					// ACT_ASSIGNEXPR, but it seems slightly beneficial for other things in case
					// they ever use quoted numeric ARGS such as "13", etc.  It's also simpler
					// to do it unconditionally.
					// Find the end of this string literal, noting that a pair of double quotes is
					// a literal double quote inside the string:
					for (cp = this_new_arg.text + 1;; ++cp)
					{
						if (!*cp) // No matching end-quote. Probably impossible due to validation further above.
							return FAIL; // Force a silent failure so that the below can continue with confidence.
						if (*cp == '"') // If not followed immediately by another, this is the end of it.
						{
							++cp;
							if (*cp != '"') // String terminator or some non-quote character.
								break;  // The previous char is the ending quote.
							//else a pair of quotes, which resolves to a single literal quote.
							// This pair is skipped over and the loop continues until the real end-quote is found.
						}
					}
					// cp is now the character after the first literal string's ending quote.
					// If that char is the terminator, that first string is the only string and this
					// is a simple assignment of a string literal to be converted here.
					// v1.0.40.05: Leave Send/PostMessage args (all of them, but specifically
					// wParam and lParam) as expressions so that at runtime, the leading '"' in a
					// quoted numeric string such as "123" can be used to differentiate that string
					// from a numeric value/expression such as 123 or 122+1.
					if (!*cp && aActionType != ACT_SENDMESSAGE && aActionType != ACT_POSTMESSAGE)
					{
						this_new_arg.is_expression = false;
						// Bugfix for 1.0.25.06: The below has been disabled because:
						// 1) It yields inconsistent results due to AutoTrim.  For example, the assignment
						//    x := "  string" should retain the leading spaces unconditionally, since any
						//    more complex expression would.  But if := were converted to = in this case,
						//    AutoTrim would be in effect for it, which is undesirable.
						// 2) It's not necessary in since ASSIGNEXPR handles both expressions and non-expressions.
						//if (aActionType == ACT_ASSIGNEXPR)
						//	aActionType = ACT_ASSIGN; // Convert to simple assignment.
						*(--cp) = '\0'; // Remove the ending quote.
						memmove(this_new_arg.text, this_new_arg.text + 1, cp - this_new_arg.text); // Remove the starting quote.
						// Convert all pairs of quotes into single literal quotes:
						StrReplace(this_new_arg.text, "\"\"", "\"", SCS_SENSITIVE);
						// Above relies on the fact that StrReplace() does not do cascading replacements,
						// meaning that a series of characters such as """" would be correctly converted into
						// two double quotes rather than collapsing into only one.
						this_new_arg.length = (WORD)strlen(this_new_arg.text); // Update length to reflect changes made above.
					}
				}
				// Make things like "Sleep Var" and "Var := X" into non-expressions.  At runtime,
				// such args are expanded normally rather than having to run them through the
				// expression evaluator.  A simple test script shows that this one change can
				// double the runtime performance of certain commands such as EnvAdd:
				// Below is somewhat obsolete but kept for reference:
				// This policy is basically saying that expressions are allowed to evaluate to strings
				// everywhere appropriate, but that at the moment the only appropriate place is x := y
				// because all other expressions should resolve to a numeric value by virtue of the fact
				// that they *are* numeric parameters.  ValidateName() serves to eliminate cases where
				// a single deref is accompanied by literal numbers, strings, or operators, e.g.
				// Var := X + 1 ... Var := Var2 "xyz" ... Var := -Var2
				else if (deref_count == 1 && Var::ValidateName(this_new_arg.text, false, DISPLAY_NO_ERROR)) // Single isolated deref.
				{
					// For the future, could consider changing ACT_ASSIGN here to ACT_ASSIGNEXPR because
					// the latter probably performs better in this case.  However, the way ValidateName()
					// is used above is probably not correct/sufficient to exclude cases to which this
					// method should not be applied, such as Var := abc%Var2%.  In any case, some careful
					// review of PerformAssign() should be done to gauge side-effects and determine
					// whether the performance boost is really that signficant given that PerformAssign()
					// is already boosted by the fact that it's exempt from automatic ExpandArgs() in
					// ExecUntil().
					this_new_arg.is_expression = false;
					// But aActionType is left as ACT_ASSIGNEXPR because it probably performs better than
					// ACT_ASSIGN in these cases.
				}
				else if (deref_count && !StrChrAny(this_new_arg.text, EXPR_OPERAND_TERMINATORS)) // No spaces, tabs, etc.
				{
					// Adjust if any of the following special cases apply:
					// x := y  -> Mark as non-expression (after expression-parsing set up parsed derefs above)
					//            so that the y deref will be only a single-deref to be directly stored in x.
					//            This is done in case y contains a string.  Since an expression normally
					//            evaluates to a number, without this workaround, x := y would be useless for
					//            a simple assignment of a string.  This case is handled above.
					// x := %y% -> Mark the right-side arg as an input variable so that it will be doubly
					//             dereferenced, similar to StringTrimRight, Out, %y%, 0.  This seems best
					//             because there would be little or no point to having it behave identically
					//             to x := y.  It might even be confusing in light of the next case below.
					// CASE #3:
					// x := Literal%y%Literal%z%Literal -> Same as above.  This is done mostly to support
					// retrieving array elements whose contents are *non-numeric* without having to use
					// something like StringTrimRight.

					// Now we know it has at least one deref.  But if any operators or other characters disallowed
					// in variables are present, it all three cases are disqualified and kept as expressions.
					// This check is necessary for all three cases:

					// No operators of any kind anywhere.  Not even +/- prefix, since those imply a numeric
					// expression.  No chars illegal in var names except the percent signs themselves,
					// e.g. *no* whitespace.
					// Also, the first deref (indeed, all of them) should point to a percent sign, since
					// there should not be any way for non-percent derefs to get mixed in with cases
					// 2 or 3.
					if (!deref[0].is_function && *deref[0].marker == g_DerefChar) // This appears to be case #2 or #3.
					{
						// Set it up so that x:=Array%i% behaves the same as StringTrimRight, Out, Array%i%, 0.
						this_new_arg.is_expression = false;
						this_new_arg.type = ARG_TYPE_INPUT_VAR;
					}
				}
			} // if (this_new_arg.is_expression)
			else // this arg does not contain an expression.
				if (!ParseDerefs(this_new_arg.text, this_aArgMap, deref, deref_count))
					return FAIL; // It already displayed the error.

			//////////////////////////////////////////////////////////////
			// Allocate mem for this arg's list of dereferenced variables.
			//////////////////////////////////////////////////////////////
			if (deref_count)
			{
				// +1 for the "NULL-item" terminator:
				if (   !(this_new_arg.deref = (DerefType *)SimpleHeap::Malloc((deref_count + 1) * sizeof(DerefType)))   )
					return ScriptError(ERR_OUTOFMEM);
				memcpy(this_new_arg.deref, deref, deref_count * sizeof(DerefType));
				// Terminate the list of derefs with a deref that has a NULL marker:
				this_new_arg.deref[deref_count].marker = NULL;
			}
			else
				this_new_arg.deref = NULL;
		} // for each arg.
	} // else there are more than zero args.

	//////////////////////////////////////////////////////////////////////////////////////
	// Now the above has allocated some dynamic memory, the pointers to which we turn over
	// to Line's constructor so that they can be anchored to the new line.
	//////////////////////////////////////////////////////////////////////////////////////
	Line *the_new_line = new Line(mCurrFileIndex, mCombinedLineNumber, aActionType, new_arg, aArgc);
	if (!the_new_line)
		return ScriptError(ERR_OUTOFMEM);

	Line &line = *the_new_line;  // For performance and convenience.

	line.mPrevLine = mLastLine;  // Whether NULL or not.
	if (mFirstLine == NULL)
		mFirstLine = the_new_line;
	else
		mLastLine->mNextLine = the_new_line;
	// This must be done after the above:
	mLastLine = the_new_line;
	mCurrLine = the_new_line;  // To help error reporting.

	///////////////////////////////////////////////////////////////////
	// Do any post-add validation & handling for specific action types.
	///////////////////////////////////////////////////////////////////
#ifndef AUTOHOTKEYSC // For v1.0.35.01, some syntax checking is removed in compiled scripts to reduce their size.
	int value;    // For temp use during validation.
	double value_float;
	SYSTEMTIME st;  // same.
#endif
	// v1.0.38: The following should help reduce code size, and for some commands helps load-time
	// performance by avoiding multiple resolutions of a given macro:
	char *new_raw_arg1 = const_cast<char*>(NEW_RAW_ARG1);
	char *new_raw_arg2 = const_cast<char*>(NEW_RAW_ARG2);
	char *new_raw_arg3 = const_cast<char*>(NEW_RAW_ARG3);
	char *new_raw_arg4 = const_cast<char*>(NEW_RAW_ARG4);

	switch(aActionType)
	{
	// Fix for v1.0.35.02:
	// THESE FIRST FEW CASES MUST EXIT IN BOTH SELF-CONTAINED AND NORMAL VERSION since they alter the
	// attributes/members of some types of lines:
	case ACT_LOOP:
		// If possible, determine the type of loop so that the preparser can better
		// validate some things:
		switch (aArgc)
		{
		case 0:
			line.mAttribute = ATTR_LOOP_NORMAL;
			break;
		case 1: // With only 1 arg, it must be a normal loop, file-pattern loop, or registry loop.
			// v1.0.43.07: Added check for new_arg[0].is_expression so that an expression without any variables
			// it it works (e.g. Loop % 1+1):
			if (line.ArgHasDeref(1) || new_arg[0].is_expression) // Impossible to know now what type of loop (only at runtime).
				line.mAttribute = ATTR_LOOP_UNKNOWN;
			else
			{
				if (IsPureNumeric(new_raw_arg1, false))
					line.mAttribute = ATTR_LOOP_NORMAL;
				else
					line.mAttribute = line.RegConvertRootKey(new_raw_arg1) ? ATTR_LOOP_REG : ATTR_LOOP_FILEPATTERN;
			}
			break;
		default:  // has 2 or more args.
			if (line.ArgHasDeref(1)) // Impossible to know now what type of loop (only at runtime).
				line.mAttribute = ATTR_LOOP_UNKNOWN;
			else if (!stricmp(new_raw_arg1, "Read"))
				line.mAttribute = ATTR_LOOP_READ_FILE;
			else if (!stricmp(new_raw_arg1, "Parse"))
				line.mAttribute = ATTR_LOOP_PARSE;
			else // the 1st arg can either be a Root Key or a File Pattern, depending on the type of loop.
			{
				line.mAttribute = line.RegConvertRootKey(new_raw_arg1) ? ATTR_LOOP_REG : ATTR_LOOP_FILEPATTERN;
				if (line.mAttribute == ATTR_LOOP_FILEPATTERN)
				{
					// Validate whatever we can rather than waiting for runtime validation:
					if (!line.ArgHasDeref(2) && Line::ConvertLoopMode(new_raw_arg2) == FILE_LOOP_INVALID)
						return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
					if (*new_raw_arg3 && !line.ArgHasDeref(3))
						if (strlen(new_raw_arg3) > 1 || (*new_raw_arg3 != '0' && *new_raw_arg3 != '1'))
							return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
				}
				else // Registry loop.
				{
					if (aArgc > 2 && !line.ArgHasDeref(3) && Line::ConvertLoopMode(new_raw_arg3) == FILE_LOOP_INVALID)
						return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
					if (*new_raw_arg4 && !line.ArgHasDeref(4))
						if (strlen(new_raw_arg4) > 1 || (*new_raw_arg4 != '0' && *new_raw_arg4 != '1'))
							return ScriptError(ERR_PARAM4_INVALID, new_raw_arg4);
				}
			}
		}
		break; // Outer switch().

	case ACT_REPEAT: // These types of loops are always "NORMAL".
		line.mAttribute = ATTR_LOOP_NORMAL;
		break;

	// This one alters g_persistent so is present in its entirety (for simplicity) in both SC an non-SC version.
	case ACT_GUI:
		// By design, scripts that use the GUI cmd anywhere are persistent.  Doing this here
		// also allows WinMain() to later detect whether this script should become #SingleInstance.
		// Note: Don't directly change g_AllowOnlyOneInstance here in case the remainder of the
		// script-loading process comes across any explicit uses of #SingleInstance, which would
		// override the default set here.
		g_persistent = true;
#ifndef AUTOHOTKEYSC // For v1.0.35.01, some syntax checking is removed in compiled scripts to reduce their size.
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			GuiCommands gui_cmd = line.ConvertGuiCommand(new_raw_arg1);

			switch (gui_cmd)
			{
			case GUI_CMD_INVALID:
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
			case GUI_CMD_ADD:
				if (aArgc > 1 && !line.ArgHasDeref(2))
				{
					GuiControls control_type;
					if (   !(control_type = line.ConvertGuiControl(new_raw_arg2))   )
						return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
					if (control_type == GUI_CONTROL_TREEVIEW && aArgc > 3) // Reserve it for future use such as a tab-indented continuation section that lists the tree hierarchy.
						return ScriptError(ERR_PARAM4_OMIT, new_raw_arg4);
				}
				break;
			case GUI_CMD_CANCEL:
			case GUI_CMD_MINIMIZE:
			case GUI_CMD_MAXIMIZE:
			case GUI_CMD_RESTORE:
			case GUI_CMD_DESTROY:
			case GUI_CMD_DEFAULT:
			case GUI_CMD_OPTIONS:
				if (aArgc > 1)
					return ScriptError("Parameter #2 and beyond should be omitted in this case.", new_raw_arg2);
				break;
			case GUI_CMD_SUBMIT:
			case GUI_CMD_MENU:
			case GUI_CMD_LISTVIEW:
			case GUI_CMD_TREEVIEW:
			case GUI_CMD_FLASH:
				if (aArgc > 2)
					return ScriptError("Parameter #3 and beyond should be omitted in this case.", new_raw_arg3);
				break;
			// No action for these since they have a varying number of optional params:
			//case GUI_CMD_SHOW:
			//case GUI_CMD_FONT:
			//case GUI_CMD_MARGIN:
			//case GUI_CMD_TAB:
			//case GUI_CMD_COLOR: No load-time param validation to avoid larger EXE size.
			}
		}
#endif
		break;

	case ACT_GROUPADD:
	case ACT_GROUPACTIVATE:
	case ACT_GROUPDEACTIVATE:
	case ACT_GROUPCLOSE:
		// For all these, store a pointer to the group to help performance.
		// We create a non-existent group even for ACT_GROUPACTIVATE, ACT_GROUPDEACTIVATE
		// and ACT_GROUPCLOSE because we can't rely on the ACT_GROUPADD commands having
		// been parsed prior to them (e.g. something like "Gosub, DefineGroups" may appear
		// in the auto-execute portion of the script).
		if (!line.ArgHasDeref(1))
			if (   !(line.mAttribute = FindGroup(new_raw_arg1, true))   ) // Create-if-not-found so that performance is enhanced at runtime.
				return FAIL;  // The above already displayed the error.
		if (aActionType == ACT_GROUPACTIVATE || aActionType == ACT_GROUPDEACTIVATE)
		{
			if (*new_raw_arg2 && !line.ArgHasDeref(2))
				if (strlen(new_raw_arg2) > 1 || toupper(*new_raw_arg2) != 'R')
					return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		}
		else if (aActionType == ACT_GROUPCLOSE)
			if (*new_raw_arg2 && !line.ArgHasDeref(2))
				if (strlen(new_raw_arg2) > 1 || !strchr("RA", toupper(*new_raw_arg2)))
					return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		break;

#ifndef AUTOHOTKEYSC // For v1.0.35.01, some syntax checking is removed in compiled scripts to reduce their size.
	case ACT_RETURN:
		if (aArgc > 0 && !g.CurrentFunc)
			return ScriptError("Return's parameter should be blank except inside a function.");
		break;

	case ACT_AUTOTRIM:
	case ACT_DETECTHIDDENWINDOWS:
	case ACT_DETECTHIDDENTEXT:
	case ACT_SETSTORECAPSLOCKMODE:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertOnOff(new_raw_arg1))
			return ScriptError(ERR_ON_OFF, new_raw_arg1);
		break;

	case ACT_STRINGCASESENSE:
		if (aArgc > 0 && !line.ArgHasDeref(1) && line.ConvertStringCaseSense(new_raw_arg1) == SCS_INVALID)
			return ScriptError(ERR_ON_OFF_LOCALE, new_raw_arg1);
		break;

	case ACT_SETBATCHLINES:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			if (!strcasestr(new_raw_arg1, "ms") && !IsPureNumeric(new_raw_arg1, true, false)) // For simplicity and due to rarity, new_arg[0].is_expression isn't checked, so a line with no variables or function-calls like "SetBatchLines % 1+1" will be wrongly seen as a syntax error.
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		}
		break;

	case ACT_SUSPEND:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertOnOffTogglePermit(new_raw_arg1))
			return ScriptError(ERR_ON_OFF_TOGGLE_PERMIT, new_raw_arg1);
		break;

	case ACT_BLOCKINPUT:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertBlockInput(new_raw_arg1))
			return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		break;

	case ACT_SENDMODE:
		if (aArgc > 0 && !line.ArgHasDeref(1) && line.ConvertSendMode(new_raw_arg1, SM_INVALID) == SM_INVALID)
			return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		break;

	case ACT_PAUSE:
	case ACT_KEYHISTORY:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertOnOffToggle(new_raw_arg1))
			return ScriptError(ERR_ON_OFF_TOGGLE, new_raw_arg1);
		break;

	case ACT_SETNUMLOCKSTATE:
	case ACT_SETSCROLLLOCKSTATE:
	case ACT_SETCAPSLOCKSTATE:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertOnOffAlways(new_raw_arg1))
			return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		break;

	case ACT_STRINGMID:
		if (aArgc > 4 && !line.ArgHasDeref(5) && stricmp(NEW_RAW_ARG5, "L"))
			return ScriptError(ERR_PARAM5_INVALID, const_cast<char*>(NEW_RAW_ARG5));
		break;

	case ACT_STRINGGETPOS:
		if (*new_raw_arg4 && !line.ArgHasDeref(4) && !strchr("LR1", toupper(*new_raw_arg4)))
			return ScriptError(ERR_PARAM4_INVALID, new_raw_arg4);
		break;

	case ACT_STRINGSPLIT:
		if (*new_raw_arg1 && !line.ArgHasDeref(1)) // The output array must be a legal name.
		{
			// 1.0.46.10: Fixed to look up ArrayName0 in advance (here at loadtime) so that runtime can
			// know whether it's local or global.  This is necessary because only here at loadtime
			// is there any awareness of the current function's list of declared variables (to conserve
			// memory, that list is longer available at runtime).
			char temp_var_name[MAX_VAR_NAME_LENGTH + 10]; // Provide extra room for trailing "0", and to detect names that are too long.
			snprintf(temp_var_name, sizeof(temp_var_name), "%s0", new_raw_arg1);
			if (   !(the_new_line->mAttribute = FindOrAddVar(temp_var_name))   )
				return FAIL;  // The above already displayed the error.
		}
		//else it's a dynamic array name.  Since that's very rare, just use the old runtime behavior for
		// backward compatibility.
		break;

	case ACT_REGREAD:
		// The below has two checks in case the user is using the 5-param method with the 5th parameter
		// being blank to indicate that the key's "default" value should be read.  For example:
		// RegRead, OutVar, REG_SZ, HKEY_CURRENT_USER, Software\Winamp,
		if (aArgc > 4 || line.RegConvertValueType(new_raw_arg2))
		{
			// The obsolete 5-param method is being used, wherein ValueType is the 2nd param.
			if (*new_raw_arg3 && !line.ArgHasDeref(3) && !line.RegConvertRootKey(new_raw_arg3))
				return ScriptError(ERR_REG_KEY, new_raw_arg3);
		}
		else // 4-param method.
			if (*new_raw_arg2 && !line.ArgHasDeref(2) && !line.RegConvertRootKey(new_raw_arg2))
				return ScriptError(ERR_REG_KEY, new_raw_arg2);
		break;

	case ACT_REGWRITE:
		// Both of these checks require that at least two parameters be present.  Otherwise, the command
		// is being used in its registry-loop mode and is validated elsewhere:
		if (aArgc > 1)
		{
			if (*new_raw_arg1 && !line.ArgHasDeref(1) && !line.RegConvertValueType(new_raw_arg1))
				return ScriptError(ERR_REG_VALUE_TYPE, new_raw_arg1);
			if (*new_raw_arg2 && !line.ArgHasDeref(2) && !line.RegConvertRootKey(new_raw_arg2))
				return ScriptError(ERR_REG_KEY, new_raw_arg2);
		}
		break;

	case ACT_REGDELETE:
		if (*new_raw_arg1 && !line.ArgHasDeref(1) && !line.RegConvertRootKey(new_raw_arg1))
			return ScriptError(ERR_REG_KEY, new_raw_arg1);
		break;

	case ACT_SOUNDGET:
	case ACT_SOUNDSET:
		if (aActionType == ACT_SOUNDSET && aArgc > 0 && !line.ArgHasDeref(1))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 300-250 as invalid.
			value_float = ATOF(new_raw_arg1);
			if (value_float < -100 || value_float > 100)
				return ScriptError(ERR_PERCENT, new_raw_arg1);
		}
		if (*new_raw_arg2 && !line.ArgHasDeref(2) && !line.SoundConvertComponentType(new_raw_arg2))
			return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		if (*new_raw_arg3 && !line.ArgHasDeref(3) && line.SoundConvertControlType(new_raw_arg3) == MIXERCONTROL_CONTROLTYPE_INVALID)
			return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
		break;

	case ACT_SOUNDSETWAVEVOLUME:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 300-250 as invalid.
			value_float = ATOF(new_raw_arg1);
			if (value_float < -100 || value_float > 100)
				return ScriptError(ERR_PERCENT, new_raw_arg1);
		}
		break;

	case ACT_SOUNDPLAY:
		if (*new_raw_arg2 && !line.ArgHasDeref(2) && stricmp(new_raw_arg2, "wait") && stricmp(new_raw_arg2, "1"))
			return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		break;

	case ACT_PIXELSEARCH:
	case ACT_IMAGESEARCH:
		if (!*new_raw_arg3 || !*new_raw_arg4 || !*NEW_RAW_ARG5 || !*NEW_RAW_ARG6 || !*NEW_RAW_ARG7)
			return ScriptError("Parameters 3 through 7 must not be blank.");
		if (aActionType != ACT_IMAGESEARCH)
		{
			if (*NEW_RAW_ARG8 && !line.ArgHasDeref(8))
			{
				// The value of catching syntax errors at load-time seems to outweigh the fact that this check
				// sees a valid no-deref expression such as 300-200 as invalid.
				value = ATOI(const_cast<char*>(NEW_RAW_ARG8));
				if (value < 0 || value > 255)
					return ScriptError(ERR_PARAM8_INVALID, const_cast<char*>(NEW_RAW_ARG8));
			}
		}
		break;

	case ACT_COORDMODE:
		if (*new_raw_arg1 && !line.ArgHasDeref(1) && !line.ConvertCoordModeAttrib(new_raw_arg1))
			return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		break;

	case ACT_SETDEFAULTMOUSESPEED:
		if (*new_raw_arg1 && !line.ArgHasDeref(1))

		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 1+2 as invalid.
			value = ATOI(new_raw_arg1);
			if (value < 0 || value > MAX_MOUSE_SPEED)
				return ScriptError(ERR_MOUSE_SPEED, new_raw_arg1);
		}
		break;

	case ACT_MOUSEMOVE:
		if (*new_raw_arg3 && !line.ArgHasDeref(3))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 200-150 as invalid.
			value = ATOI(new_raw_arg3);
			if (value < 0 || value > MAX_MOUSE_SPEED)
				return ScriptError(ERR_MOUSE_SPEED, new_raw_arg3);
		}
		if (*new_raw_arg4 && !line.ArgHasDeref(4) && toupper(*new_raw_arg4) != 'R')
			return ScriptError(ERR_PARAM4_INVALID, new_raw_arg4);
		if (!line.ValidateMouseCoords(new_raw_arg1, new_raw_arg2))
			return ScriptError(ERR_MOUSE_COORD, new_raw_arg1);
		break;

	case ACT_MOUSECLICK:
		if (*NEW_RAW_ARG5 && !line.ArgHasDeref(5))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 200-150 as invalid.
			value = ATOI(const_cast<char*>(NEW_RAW_ARG5));
			if (value < 0 || value > MAX_MOUSE_SPEED)
				return ScriptError(ERR_MOUSE_SPEED, const_cast<char*>(NEW_RAW_ARG5));
		}
		if (*NEW_RAW_ARG6 && !line.ArgHasDeref(6))
			if (strlen(NEW_RAW_ARG6) > 1 || !strchr("UD", toupper(*NEW_RAW_ARG6)))  // Up / Down
				return ScriptError(ERR_PARAM6_INVALID, const_cast<char*>(NEW_RAW_ARG6));
		if (*NEW_RAW_ARG7 && !line.ArgHasDeref(7) && toupper(*NEW_RAW_ARG7) != 'R')
			return ScriptError(ERR_PARAM7_INVALID, const_cast<char*>(NEW_RAW_ARG7));
		// Check that the button is valid (e.g. left/right/middle):
		if (*new_raw_arg1 && !line.ArgHasDeref(1) && !line.ConvertMouseButton(new_raw_arg1)) // Treats blank as "Left".
			return ScriptError(ERR_MOUSE_BUTTON, new_raw_arg1);
		if (!line.ValidateMouseCoords(new_raw_arg2, new_raw_arg3))
			return ScriptError(ERR_MOUSE_COORD, new_raw_arg2);
		break;

	case ACT_MOUSECLICKDRAG:
		// Even though we check for blanks here at load-time, we don't bother to do so at runtime
		// (i.e. if a dereferenced var resolved to blank, it will be treated as a zero):
		if (!*new_raw_arg4 || !*NEW_RAW_ARG5)
			return ScriptError("Parameter #4 and 5 required.");
		if (*NEW_RAW_ARG6 && !line.ArgHasDeref(6))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 200-150 as invalid.
			value = ATOI(const_cast<char*>(NEW_RAW_ARG6));
			if (value < 0 || value > MAX_MOUSE_SPEED)
				return ScriptError(ERR_MOUSE_SPEED, const_cast<char*>(NEW_RAW_ARG6));
		}
		if (*NEW_RAW_ARG7 && !line.ArgHasDeref(7) && toupper(*NEW_RAW_ARG7) != 'R')
			return ScriptError(ERR_PARAM7_INVALID, const_cast<char*>(NEW_RAW_ARG7));
		if (!line.ArgHasDeref(1))
			if (!line.ConvertMouseButton(new_raw_arg1, false))
				return ScriptError(ERR_MOUSE_BUTTON, new_raw_arg1);
		if (!line.ValidateMouseCoords(new_raw_arg2, new_raw_arg3))
			return ScriptError(ERR_MOUSE_COORD, new_raw_arg2);
		if (!line.ValidateMouseCoords(new_raw_arg4, const_cast<char*>(NEW_RAW_ARG5)))
			return ScriptError(ERR_MOUSE_COORD, new_raw_arg4);
		break;

	case ACT_CONTROLSEND:
	case ACT_CONTROLSENDRAW:
		// Window params can all be blank in this case, but characters to send should
		// be non-blank (but it's ok if its a dereferenced var that resolves to blank
		// at runtime):
		if (!*new_raw_arg2)
			return ScriptError(ERR_PARAM2_REQUIRED);
		break;

	case ACT_CONTROLCLICK:
		// Check that the button is valid (e.g. left/right/middle):
		if (*new_raw_arg4 && !line.ArgHasDeref(4)) // i.e. it's allowed to be blank (defaults to left).
			if (!line.ConvertMouseButton(new_raw_arg4)) // Treats blank as "Left".
				return ScriptError(ERR_MOUSE_BUTTON, new_raw_arg4);
		break;

	case ACT_ADD:
	case ACT_SUB:
		if (aArgc > 2)
		{
			if (*new_raw_arg3 && !line.ArgHasDeref(3))
				if (!strchr("SMHD", toupper(*new_raw_arg3)))  // (S)econds, (M)inutes, (H)ours, or (D)ays
					return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
			if (aActionType == ACT_SUB && *new_raw_arg2 && !line.ArgHasDeref(2))
				if (!YYYYMMDDToSystemTime(new_raw_arg2, st, true))
					return ScriptError(ERR_INVALID_DATETIME, new_raw_arg2);
		}
		break;

	case ACT_FILEINSTALL:
	case ACT_FILECOPY:
	case ACT_FILEMOVE:
	case ACT_FILECOPYDIR:
	case ACT_FILEMOVEDIR:
		if (*new_raw_arg3 && !line.ArgHasDeref(3))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 2-1 as invalid.
			value = ATOI(new_raw_arg3);
			bool is_pure_numeric = IsPureNumeric(new_raw_arg3, false, true); // Consider negatives to be non-numeric.
			if (aActionType == ACT_FILEMOVEDIR)
			{
				if (!is_pure_numeric && toupper(*new_raw_arg3) != 'R'
					|| is_pure_numeric && value > 2) // IsPureNumeric() already checked if value < 0.
					return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
			}
			else
			{
				if (!is_pure_numeric || value > 1) // IsPureNumeric() already checked if value < 0.
					return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
			}
		}
		if (aActionType == ACT_FILEINSTALL)
		{
			if (aArgc > 0 && line.ArgHasDeref(1))
				return ScriptError("Must not contain variables.", new_raw_arg1);
		}
		break;

	case ACT_FILEREMOVEDIR:
		if (*new_raw_arg2 && !line.ArgHasDeref(2))
		{
			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 3-2 as invalid.
			value = ATOI(new_raw_arg2);
			if (!IsPureNumeric(new_raw_arg2, false, true) || value > 1) // IsPureNumeric() prevents negatives.
				return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		}
		break;

	case ACT_FILESETATTRIB:
		if (*new_raw_arg1 && !line.ArgHasDeref(1))
		{
			for (char *cp = new_raw_arg1; *cp; ++cp)
				if (!strchr("+-^RASHNOT", toupper(*cp)))
					return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		}
		// For the next two checks:
		// The value of catching syntax errors at load-time seems to outweigh the fact that this check
		// sees a valid no-deref expression such as 300-200 as invalid.
		if (aArgc > 2 && !line.ArgHasDeref(3) && line.ConvertLoopMode(new_raw_arg3) == FILE_LOOP_INVALID)
			return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
		if (*new_raw_arg4 && !line.ArgHasDeref(4))
			if (strlen(new_raw_arg4) > 1 || (*new_raw_arg4 != '0' && *new_raw_arg4 != '1'))
				return ScriptError(ERR_PARAM4_INVALID, new_raw_arg4);
		break;

	case ACT_FILEGETTIME:
		if (*new_raw_arg3 && !line.ArgHasDeref(3))
			if (strlen(new_raw_arg3) > 1 || !strchr("MCA", toupper(*new_raw_arg3)))
				return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
		break;

	case ACT_FILESETTIME:
		if (*new_raw_arg1 && !line.ArgHasDeref(1))
			if (!YYYYMMDDToSystemTime(new_raw_arg1, st, true))
				return ScriptError(ERR_INVALID_DATETIME, new_raw_arg1);
		if (*new_raw_arg3 && !line.ArgHasDeref(3))
			if (strlen(new_raw_arg3) > 1 || !strchr("MCA", toupper(*new_raw_arg3)))
				return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
		// For the next two checks:
		// The value of catching syntax errors at load-time seems to outweigh the fact that this check
		// sees a valid no-deref expression such as 300-200 as invalid.
		if (aArgc > 3 && !line.ArgHasDeref(4) && line.ConvertLoopMode(new_raw_arg4) == FILE_LOOP_INVALID)
			return ScriptError(ERR_PARAM4_INVALID, new_raw_arg4);
		if (*NEW_RAW_ARG5 && !line.ArgHasDeref(5))
			if (strlen(NEW_RAW_ARG5) > 1 || (*NEW_RAW_ARG5 != '0' && *NEW_RAW_ARG5 != '1'))
				return ScriptError(ERR_PARAM5_INVALID, const_cast<char*>(NEW_RAW_ARG5));
		break;

	case ACT_FILEGETSIZE:
		if (*new_raw_arg3 && !line.ArgHasDeref(3))
			if (strlen(new_raw_arg3) > 1 || !strchr("BKM", toupper(*new_raw_arg3))) // Allow B=Bytes as undocumented.
				return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
		break;

	case ACT_SETTITLEMATCHMODE:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertTitleMatchMode(new_raw_arg1))
			return ScriptError(ERR_TITLEMATCHMODE, new_raw_arg1);
		break;

	case ACT_SETFORMAT:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
            if (!stricmp(new_raw_arg1, "Float"))
			{
				if (aArgc > 1 && !line.ArgHasDeref(2))
				{
					if (!IsPureNumeric(new_raw_arg2, true, false, true, true) // v1.0.46.11: Allow impure numbers to support scientific notation; e.g. 0.6e or 0.6E.
						|| strlen(new_raw_arg2) >= sizeof(g.FormatFloat) - 2)
						return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
				}
			}
			else if (!stricmp(new_raw_arg1, "Integer"))
			{
				if (aArgc > 1 && !line.ArgHasDeref(2) && toupper(*new_raw_arg2) != 'H' && toupper(*new_raw_arg2) != 'D')
					return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
			}
			else
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		}
		// Size must be less than sizeof() minus 2 because need room to prepend the '%' and append
		// the 'f' to make it a valid format specifier string:
		break;

	case ACT_TRANSFORM:
		if (aArgc > 1 && !line.ArgHasDeref(2))
		{
			// The value of trans_cmd was already set at an earlier stage, but only here can the error
			// for new_raw_arg3 be displayed because only here was it finally possible to call
			// ArgHasDeref() [above].
			if (trans_cmd == TRANS_CMD_INVALID)
				return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
			if (trans_cmd == TRANS_CMD_UNICODE && !*line.mArg[0].text) // blank text means output-var is not a dynamically built one.
			{
				// If the output var isn't the clipboard, the mode is "retrieve clipboard text as UTF-8".
				// Therefore, Param#3 should be blank in that case to avoid unnecessary fetching of the
				// entire clipboard contents as plain text when in fact the command itself will be
				// directly accessing the clipboard rather than relying on the automatic parameter and
				// deref handling.
				if (VAR(line.mArg[0])->Type() == VAR_CLIPBOARD)
				{
					if (aArgc < 3)
						return ScriptError("Parameter #3 must not be blank in this case.");
				}
				else
					if (aArgc > 2)
						return ScriptError(ERR_PARAM3_MUST_BE_BLANK, new_raw_arg3);
				break; // This type has been fully checked above.
			}

			// The value of catching syntax errors at load-time seems to outweigh the fact that this check
			// sees a valid no-deref expression such as 1+2 as invalid.
			if (!line.ArgHasDeref(3)) // "true" since it might have been made into an InputVar due to being a simple expression.
			{
				switch(trans_cmd)
				{
				case TRANS_CMD_CHR:
				case TRANS_CMD_BITNOT:
				case TRANS_CMD_BITSHIFTLEFT:
				case TRANS_CMD_BITSHIFTRIGHT:
				case TRANS_CMD_BITAND:
				case TRANS_CMD_BITOR:
				case TRANS_CMD_BITXOR:
					if (!IsPureNumeric(new_raw_arg3, true, false))
						return ScriptError("Parameter #3 must be an integer in this case.", new_raw_arg3);
					break;

				case TRANS_CMD_MOD:
				case TRANS_CMD_EXP:
				case TRANS_CMD_ROUND:
				case TRANS_CMD_CEIL:
				case TRANS_CMD_FLOOR:
				case TRANS_CMD_ABS:
				case TRANS_CMD_SIN:
				case TRANS_CMD_COS:
				case TRANS_CMD_TAN:
				case TRANS_CMD_ASIN:
				case TRANS_CMD_ACOS:
				case TRANS_CMD_ATAN:
					if (!IsPureNumeric(new_raw_arg3, true, false, true))
						return ScriptError("Parameter #3 must be a number in this case.", new_raw_arg3);
					break;

				case TRANS_CMD_POW:
				case TRANS_CMD_SQRT:
				case TRANS_CMD_LOG:
				case TRANS_CMD_LN:
					if (!IsPureNumeric(new_raw_arg3, false, false, true))
						return ScriptError("Parameter #3 must be a positive integer in this case.", new_raw_arg3);
					break;

				// The following are not listed above because no validation of Parameter #3 is needed at this stage:
				// TRANS_CMD_ASC
				// TRANS_CMD_UNICODE
				// TRANS_CMD_HTML
				// TRANS_CMD_DEREF
				}
			}

			switch(trans_cmd)
			{
			case TRANS_CMD_ASC:
			case TRANS_CMD_CHR:
			case TRANS_CMD_DEREF:
			case TRANS_CMD_UNICODE:
			case TRANS_CMD_HTML:
			case TRANS_CMD_EXP:
			case TRANS_CMD_SQRT:
			case TRANS_CMD_LOG:
			case TRANS_CMD_LN:
			case TRANS_CMD_CEIL:
			case TRANS_CMD_FLOOR:
			case TRANS_CMD_ABS:
			case TRANS_CMD_SIN:
			case TRANS_CMD_COS:
			case TRANS_CMD_TAN:
			case TRANS_CMD_ASIN:
			case TRANS_CMD_ACOS:
			case TRANS_CMD_ATAN:
			case TRANS_CMD_BITNOT:
				if (*new_raw_arg4)
					return ScriptError(ERR_PARAM4_OMIT, new_raw_arg4);
				break;

			case TRANS_CMD_BITAND:
			case TRANS_CMD_BITOR:
			case TRANS_CMD_BITXOR:
				if (!line.ArgHasDeref(4) && !IsPureNumeric(new_raw_arg4, true, false))
					return ScriptError("Parameter #4 must be an integer in this case.", new_raw_arg4);
				break;

			case TRANS_CMD_BITSHIFTLEFT:
			case TRANS_CMD_BITSHIFTRIGHT:
				if (!line.ArgHasDeref(4) && !IsPureNumeric(new_raw_arg4, false, false))
					return ScriptError("Parameter #4 must be a positive integer in this case.", new_raw_arg4);
				break;

			case TRANS_CMD_ROUND:
				if (*new_raw_arg4 && !line.ArgHasDeref(4) && !IsPureNumeric(new_raw_arg4, true, false))
					return ScriptError("Parameter #4 must be blank or an integer in this case.", new_raw_arg4);
				break;

			case TRANS_CMD_MOD:
			case TRANS_CMD_POW:
				if (!line.ArgHasDeref(4) && !IsPureNumeric(new_raw_arg4, true, false, true))
					return ScriptError("Parameter #4 must be a number in this case.", new_raw_arg4);
				break;
#ifdef _DEBUG
			default:
				return ScriptError("DEBUG: Unhandled", new_raw_arg2);  // To improve maintainability.
#endif
			}

			switch(trans_cmd)
			{
			case TRANS_CMD_CHR:
				if (!line.ArgHasDeref(3))
				{
					value = ATOI(new_raw_arg3);
					if (!IsPureNumeric(new_raw_arg3, false, false) || value > 255) // IsPureNumeric() checks for value < 0 too.
						return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
				}
				break;
			case TRANS_CMD_MOD:
				if (!line.ArgHasDeref(4) && !ATOF(new_raw_arg4)) // Parameter is omitted or something that resolves to zero.
					return ScriptError(ERR_DIVIDEBYZERO, new_raw_arg4);
				break;
			}
		}
		break;

	case ACT_MENU:
		if (aArgc > 1 && !line.ArgHasDeref(2))
		{
			MenuCommands menu_cmd = line.ConvertMenuCommand(new_raw_arg2);

			switch(menu_cmd)
			{
			case MENU_CMD_TIP:
			case MENU_CMD_ICON:
			case MENU_CMD_NOICON:
			case MENU_CMD_MAINWINDOW:
			case MENU_CMD_NOMAINWINDOW:
			case MENU_CMD_CLICK:
			{
				bool is_tray = true;  // Assume true if unknown.
				if (aArgc > 0 && !line.ArgHasDeref(1))
					if (stricmp(new_raw_arg1, "tray"))
						is_tray = false;
				if (!is_tray)
					return ScriptError(ERR_MENUTRAY, new_raw_arg1);
				break;
			}
			}

			switch (menu_cmd)
			{
			case MENU_CMD_INVALID:
				return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);

			case MENU_CMD_NODEFAULT:
			case MENU_CMD_STANDARD:
			case MENU_CMD_NOSTANDARD:
			case MENU_CMD_DELETEALL:
			case MENU_CMD_NOICON:
			case MENU_CMD_MAINWINDOW:
			case MENU_CMD_NOMAINWINDOW:
				if (*new_raw_arg3 || *new_raw_arg4 || *NEW_RAW_ARG5 || *NEW_RAW_ARG6)
					return ScriptError("Parameter #3 and beyond should be omitted in this case.", new_raw_arg3);
				break;

			case MENU_CMD_RENAME:
			case MENU_CMD_USEERRORLEVEL:
			case MENU_CMD_CHECK:
			case MENU_CMD_UNCHECK:
			case MENU_CMD_TOGGLECHECK:
			case MENU_CMD_ENABLE:
			case MENU_CMD_DISABLE:
			case MENU_CMD_TOGGLEENABLE:
			case MENU_CMD_DEFAULT:
			case MENU_CMD_DELETE:
			case MENU_CMD_TIP:
			case MENU_CMD_CLICK:
				if (   menu_cmd != MENU_CMD_RENAME && (*new_raw_arg4 || *NEW_RAW_ARG5 || *NEW_RAW_ARG6)   )
					return ScriptError("Parameter #4 and beyond should be omitted in this case.", new_raw_arg4);
				switch(menu_cmd)
				{
				case MENU_CMD_USEERRORLEVEL:
				case MENU_CMD_TIP:
				case MENU_CMD_DEFAULT:
				case MENU_CMD_DELETE:
					break;  // i.e. for commands other than the above, do the default below.
				default:
					if (!*new_raw_arg3)
						return ScriptError("Parameter #3 must not be blank in this case.");
				}
				break;

			// These have a highly variable number of parameters, or are too rarely used
			// to warrant detailed load-time checking, so are not validated here:
			//case MENU_CMD_SHOW:
			//case MENU_CMD_ADD:
			//case MENU_CMD_COLOR:
			//case MENU_CMD_ICON:
			}
		}
		break;

	case ACT_THREAD:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertThreadCommand(new_raw_arg1))
			return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		break;

	case ACT_CONTROL:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			ControlCmds control_cmd = line.ConvertControlCmd(new_raw_arg1);
			switch (control_cmd)
			{
			case CONTROL_CMD_INVALID:
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
			case CONTROL_CMD_STYLE:
			case CONTROL_CMD_EXSTYLE:
			case CONTROL_CMD_TABLEFT:
			case CONTROL_CMD_TABRIGHT:
			case CONTROL_CMD_ADD:
			case CONTROL_CMD_DELETE:
			case CONTROL_CMD_CHOOSE:
			case CONTROL_CMD_CHOOSESTRING:
			case CONTROL_CMD_EDITPASTE:
				if (control_cmd != CONTROL_CMD_TABLEFT && control_cmd != CONTROL_CMD_TABRIGHT && !*new_raw_arg2)
					return ScriptError("Parameter #2 must not be blank in this case.");
				break;
			default: // All commands except the above should have a blank Value parameter.
				if (*new_raw_arg2)
					return ScriptError(ERR_PARAM2_MUST_BE_BLANK, new_raw_arg2);
			}
		}
		break;

	case ACT_CONTROLGET:
		if (aArgc > 1 && !line.ArgHasDeref(2))
		{
			ControlGetCmds control_get_cmd = line.ConvertControlGetCmd(new_raw_arg2);
			switch (control_get_cmd)
			{
			case CONTROLGET_CMD_INVALID:
				return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
			case CONTROLGET_CMD_FINDSTRING:
			case CONTROLGET_CMD_LINE:
				if (!*new_raw_arg3)
					return ScriptError("Parameter #3 must not be blank in this case.");
				break;
			case CONTROLGET_CMD_LIST:
				break; // Simply break for any sub-commands that have an optional parameter 3.
			default: // All commands except the above should have a blank Value parameter.
				if (*new_raw_arg3)
					return ScriptError(ERR_PARAM3_MUST_BE_BLANK, new_raw_arg3);
			}
		}
		break;

	case ACT_GUICONTROL:
		if (!*new_raw_arg2) // ControlID
			return ScriptError(ERR_PARAM2_REQUIRED);
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			GuiControlCmds guicontrol_cmd = line.ConvertGuiControlCmd(new_raw_arg1);
			switch (guicontrol_cmd)
			{
			case GUICONTROL_CMD_INVALID:
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
			case GUICONTROL_CMD_CONTENTS:
			case GUICONTROL_CMD_TEXT:
				break; // Do nothing for the above commands since Param3 is optional.
			case GUICONTROL_CMD_MOVE:
			case GUICONTROL_CMD_MOVEDRAW:
			case GUICONTROL_CMD_CHOOSE:
			case GUICONTROL_CMD_CHOOSESTRING:
				if (!*new_raw_arg3)
					return ScriptError("Parameter #3 must not be blank in this case.");
				break;
			default: // All commands except the above should have a blank Text parameter.
				if (*new_raw_arg3)
					return ScriptError(ERR_PARAM3_MUST_BE_BLANK, new_raw_arg3);
			}
		}
		break;

	case ACT_GUICONTROLGET:
		if (aArgc > 1 && !line.ArgHasDeref(2))
		{
			GuiControlGetCmds guicontrolget_cmd = line.ConvertGuiControlGetCmd(new_raw_arg2);
			// This first check's error messages take precedence over the next check's:
			switch (guicontrolget_cmd)
			{
			case GUICONTROLGET_CMD_INVALID:
				return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
			case GUICONTROLGET_CMD_CONTENTS:
				break; // Do nothing, since Param4 is optional in this case.
			default: // All commands except the above should have a blank parameter here.
				if (*new_raw_arg4) // Currently true for all, since it's a FutureUse param.
					return ScriptError(ERR_PARAM4_MUST_BE_BLANK, new_raw_arg4);
			}
			if (guicontrolget_cmd == GUICONTROLGET_CMD_FOCUS || guicontrolget_cmd == GUICONTROLGET_CMD_FOCUSV)
			{
				if (*new_raw_arg3)
					return ScriptError(ERR_PARAM3_MUST_BE_BLANK, new_raw_arg3);
			}
			// else it can be optionally blank, in which case the output variable is used as the
			// ControlID also.
		}
		break;

	case ACT_DRIVE:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			DriveCmds drive_cmd = line.ConvertDriveCmd(new_raw_arg1);
			if (!drive_cmd)
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
			if (drive_cmd != DRIVE_CMD_EJECT && !*new_raw_arg2)
				return ScriptError("Parameter #2 must not be blank in this case.");
			// For DRIVE_CMD_LABEL: Note that is is possible and allowed for the new label to be blank.
			// Not currently done since all sub-commands take a mandatory or optional ARG3:
			//if (drive_cmd != ... && *new_raw_arg3)
			//	return ScriptError(ERR_PARAM3_MUST_BE_BLANK, new_raw_arg3);
		}
		break;

	case ACT_DRIVEGET:
		if (!line.ArgHasDeref(2))  // Don't check "aArgc > 1" because of DRIVEGET_CMD_SETLABEL's param format.
		{
			DriveGetCmds drive_get_cmd = line.ConvertDriveGetCmd(new_raw_arg2);
			if (!drive_get_cmd)
				return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
			if (drive_get_cmd != DRIVEGET_CMD_LIST && drive_get_cmd != DRIVEGET_CMD_STATUSCD && !*new_raw_arg3)
				return ScriptError("Parameter #3 must not be blank in this case.");
			if (drive_get_cmd != DRIVEGET_CMD_SETLABEL && (aArgc < 1 || line.mArg[0].type == ARG_TYPE_NORMAL))
				// The output variable has been omitted.
				return ScriptError("Parameter #1 must not be blank in this case.");
		}
		break;

	case ACT_PROCESS:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			ProcessCmds process_cmd = line.ConvertProcessCmd(new_raw_arg1);
			if (process_cmd != PROCESS_CMD_PRIORITY && process_cmd != PROCESS_CMD_EXIST && !*new_raw_arg2)
				return ScriptError("Parameter #2 must not be blank in this case.");
			switch (process_cmd)
			{
			case PROCESS_CMD_INVALID:
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
			case PROCESS_CMD_EXIST:
			case PROCESS_CMD_CLOSE:
				if (*new_raw_arg3)
					return ScriptError(ERR_PARAM3_MUST_BE_BLANK, new_raw_arg3);
				break;
			case PROCESS_CMD_PRIORITY:
				if (!*new_raw_arg3 || (!line.ArgHasDeref(3) && !strchr(PROCESS_PRIORITY_LETTERS, toupper(*new_raw_arg3))))
					return ScriptError(ERR_PARAM3_INVALID, new_raw_arg3);
				break;
			case PROCESS_CMD_WAIT:
			case PROCESS_CMD_WAITCLOSE:
				if (*new_raw_arg3 && !line.ArgHasDeref(3) && !IsPureNumeric(new_raw_arg3, false, true, true))
					return ScriptError("If present, parameter #3 must be a positive number in this case.", new_raw_arg3);
				break;
			}
		}
		break;

	// For ACT_WINMOVE, don't validate anything for mandatory args so that its two modes of
	// operation can be supported: 2-param mode and normal-param mode.
	// For these, although we validate that at least one is non-blank here, it's okay at
	// runtime for them all to resolve to be blank, without an error being reported.
	// It's probably more flexible that way since the commands are equipped to handle
	// all-blank params.
	// Not these because they can be used with the "last-used window" mode:
	//case ACT_IFWINEXIST:
	//case ACT_IFWINNOTEXIST:
	// Not these because they can have their window params all-blank to work in "last-used window" mode:
	//case ACT_IFWINACTIVE:
	//case ACT_IFWINNOTACTIVE:
	//case ACT_WINACTIVATE:
	//case ACT_WINWAITCLOSE:
	//case ACT_WINWAITACTIVE:
	//case ACT_WINWAITNOTACTIVE:
	case ACT_WINACTIVATEBOTTOM:
		if (!*new_raw_arg1 && !*new_raw_arg2 && !*new_raw_arg3 && !*new_raw_arg4)
			return ScriptError(ERR_WINDOW_PARAM);
		break;

	case ACT_WINWAIT:
		if (!*new_raw_arg1 && !*new_raw_arg2 && !*new_raw_arg4 && !*NEW_RAW_ARG5) // ARG3 is omitted because it's the timeout.
			return ScriptError(ERR_WINDOW_PARAM);
		break;

	case ACT_WINMENUSELECTITEM:
		// Window params can all be blank in this case, but the first menu param should
		// be non-blank (but it's ok if its a dereferenced var that resolves to blank
		// at runtime):
		if (!*new_raw_arg3)
			return ScriptError(ERR_PARAM3_REQUIRED);
		break;

	case ACT_WINSET:
		if (aArgc > 0 && !line.ArgHasDeref(1))
		{
			switch(line.ConvertWinSetAttribute(new_raw_arg1))
			{
			case WINSET_TRANSPARENT:
				if (aArgc > 1 && !line.ArgHasDeref(2))
				{
					value = ATOI(new_raw_arg2);
					if (value < 0 || value > 255)
						return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
				}
				break;
			case WINSET_TRANSCOLOR:
				if (!*new_raw_arg2)
					return ScriptError("Parameter #2 must not be blank in this case.");
				break;
			case WINSET_ALWAYSONTOP:
				if (aArgc > 1 && !line.ArgHasDeref(2) && !line.ConvertOnOffToggle(new_raw_arg2))
					return ScriptError(ERR_ON_OFF_TOGGLE, new_raw_arg2);
				break;
			case WINSET_BOTTOM:
			case WINSET_TOP:
			case WINSET_REDRAW:
			case WINSET_ENABLE:
			case WINSET_DISABLE:
				if (*new_raw_arg2)
					return ScriptError(ERR_PARAM2_MUST_BE_BLANK);
				break;
			case WINSET_INVALID:
				return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
			}
		}
		break;

	case ACT_WINGET:
		if (!line.ArgHasDeref(2) && !line.ConvertWinGetCmd(new_raw_arg2)) // It's okay if ARG2 is blank.
			return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		break;

	case ACT_SYSGET:
		if (!line.ArgHasDeref(2) && !line.ConvertSysGetCmd(new_raw_arg2))
			return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		break;

	case ACT_INPUTBOX:
		if (*NEW_RAW_ARG9)  // && !line.ArgHasDeref(9)
			return ScriptError("Parameter #9 must be blank.", const_cast<char*>(NEW_RAW_ARG9));
		break;

	case ACT_MSGBOX:
		if (aArgc > 1) // i.e. this MsgBox is using the 3-param or 4-param style.
			if (!line.ArgHasDeref(1)) // i.e. if it's a deref, we won't try to validate it now.
				if (!IsPureNumeric(new_raw_arg1)) // Allow it to be entirely whitespace to indicate 0, like Aut2.
					return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		if (aArgc > 3) // EVEN THOUGH IT'S NUMERIC, due to MsgBox's smart-comma handling, this cannot be an expression because it would never have been detected as the fourth parameter to begin with.
			if (!line.ArgHasDeref(4)) // i.e. if it's a deref, we won't try to validate it now.
				if (!IsPureNumeric(new_raw_arg4, false, true, true))
					return ScriptError(ERR_PARAM4_INVALID, new_raw_arg4);
		break;

	case ACT_IFMSGBOX:
		if (aArgc > 0 && !line.ArgHasDeref(1) && !line.ConvertMsgBoxResult(new_raw_arg1))
			return ScriptError(ERR_PARAM1_INVALID, new_raw_arg1);
		break;

	case ACT_IFIS:
	case ACT_IFISNOT:
		if (aArgc > 1 && !line.ArgHasDeref(2) && !line.ConvertVariableTypeName(new_raw_arg2))
			// Don't refer to it as "Parameter #2" because this command isn't formatted/displayed that way.
			// Update: Param2 is more descriptive than the other (short) alternatives:
			return ScriptError(ERR_PARAM2_INVALID, new_raw_arg2);
		break;

	case ACT_GETKEYSTATE:
		// v1.0.44.03: Don't validate single-character key names because although a character like � might have no
		// matching VK in system's default layout, that layout could change to something which does have a VK for it.
		if (aArgc > 1 && !line.ArgHasDeref(2) && strlen(new_raw_arg2) > 1 && !TextToVK(new_raw_arg2) && !ConvertJoy(new_raw_arg2))
			return ScriptError(ERR_INVALID_KEY_OR_BUTTON, new_raw_arg2);
		break;

	case ACT_KEYWAIT: // v1.0.44.03: See comment above.
		if (aArgc > 0 && !line.ArgHasDeref(1) && strlen(new_raw_arg1) > 1 && !TextToVK(new_raw_arg1) && !ConvertJoy(new_raw_arg1))
			return ScriptError(ERR_INVALID_KEY_OR_BUTTON, new_raw_arg1);
		break;

	case ACT_DIV:
		if (!line.ArgHasDeref(2) && !new_arg[1].is_expression) // i.e. don't validate the following until runtime:
			if (!ATOF(new_raw_arg2))                           // x/=y ... x/=(4/4)/4 (v1.0.46.01: added is_expression check for expressions with no variables or function-calls).
				return ScriptError(ERR_DIVIDEBYZERO, new_raw_arg2);
		break;
#endif  // The above section is in place only if when not AUTOHOTKEYSC.
	}

	if (mNextLineIsFunctionBody)
	{
		mLastFunc->mJumpToLine = the_new_line;
		mNextLineIsFunctionBody = false;
		if (g.CurrentFunc->mDefaultVarType == VAR_ASSUME_NONE)
			g.CurrentFunc->mDefaultVarType = VAR_ASSUME_LOCAL;  // Set default since no override was discovered at the top of the body.
	}

	// No checking for unbalanced blocks is done here.  That is done by PreparseBlocks() because
	// it displays more informative error messages:
	if (aActionType == ACT_BLOCK_BEGIN)
	{
		++mOpenBlockCount;
		// It's only necessary to check mLastFunc, not the one(s) that come before it, to see if its
		// mJumpToLine is NULL.  This is because our caller has made it impossible for a function
		// to ever have been defined in the first place if it lacked its opening brace.  Search on
		// "consecutive function" for more comments.  In addition, the following does not check
		// that mOpenBlockCount is exactly 1, because: 1) Want to be able to support function
		// definitions inside of other function definitions (to help script maintainability); 2) If
		// mOpenBlockCount is 0 or negative, that will be caught as a syntax error by PreparseBlocks(),
		// which yields a more informative error message that we could here.
		if (mLastFunc && !mLastFunc->mJumpToLine) // If this stmt is true, caller has ensured that g.CurrentFunc isn't NULL.
		{
			// The above check relies upon the fact that mLastFunc->mIsBuiltIn must be false at this stage,
			// which is the case because any non-overridden built-in function won't get added until after all
			// lines have been added, namely PreparseBlocks().
			line.mAttribute = ATTR_TRUE;  // Flag this ACT_BLOCK_BEGIN as the opening brace of the function's body.
			// For efficiency, and to prevent ExecUntil from starting a new recursion layer for the function's
			// body, the function's execution should begin at the first line after its open-brace (even if that
			// first line is another open-brace or the function's close-brace (i.e. an empty function):
			mNextLineIsFunctionBody = true;
		}
	}
	else if (aActionType == ACT_BLOCK_END)
	{
		--mOpenBlockCount;
		if (g.CurrentFunc && !mOpenBlockCount) // Any negative mOpenBlockCount is caught by a different stage.
		{
			line.mAttribute = ATTR_TRUE;  // Flag this ACT_BLOCK_END as the ending brace of a function's body.
			g.CurrentFunc = NULL;
			mFuncExceptionVar = NULL;  // Notify FindVar() that there is no exception list to search.
		}
	}

	// Above must be done prior to the below, since it sometimes sets mAttribute for use below.

	///////////////////////////////////////////////////////////////
	// Update any labels that should refer to the newly added line.
	///////////////////////////////////////////////////////////////
	// If the label most recently added doesn't yet have an anchor in the script, provide it.
	// UPDATE: In addition, keep searching backward through the labels until a non-NULL
	// mJumpToLine is found.  All the ones with a NULL should point to this new line to
	// support cases where one label immediately follows another in the script.
	// Example:
	// #a::  <-- don't leave this label with a NULL jumppoint.
	// LaunchA:
	// ...
	// return
	if (do_update_labels)
	{
		for (Label *label = mLastLabel; label != NULL && label->mJumpToLine == NULL; label = label->mPrevLabel)
		{
			if (line.mActionType == ACT_BLOCK_BEGIN && line.mAttribute) // Non-zero mAttribute signfies the open-brace of a function body.
				return ScriptError("A label must not point to a function.");
			if (line.mActionType == ACT_ELSE)
				return ScriptError("A label must not point to an ELSE.");
			// Don't allow this because it may cause problems in a case such as this because
			// label1 points to the end-block which is at the same level (and thus normally
			// an allowable jumppoint) as the goto.  But we don't want to allow jumping into
			// a block that belongs to a control structure.  In this case, it would probably
			// result in a runtime error when the execution unexpectedly encounters the ELSE
			// after performing the goto:
			// goto, label1
			// if x
			// {
			//    ...
			//    label1:
			// }
			// else
			//    ...
			//
			// An alternate way to deal with the above would be to make each block-end be owned
			// by its block-begin rather than the block that encloses them both.
			if (line.mActionType == ACT_BLOCK_END)
				return ScriptError("A label must not point to the end of a block. For loops, use Continue vs. Goto.");
			label->mJumpToLine = the_new_line;
		}
	}

	++mLineCount;  // Right before returning "success", increment our count.
	return OK;
}



ResultType Script::ParseDerefs(char *aArgText, char *aArgMap, DerefType *aDeref, int &aDerefCount)
// Caller provides modifiable aDerefCount, which might be non-zero to indicate that there are already
// some items in the aDeref array.
// Returns FAIL or OK.
{
	size_t deref_string_length; // So that overflow can be detected, this is not of type DerefLengthType.

	// For each dereference found in aArgText:
	for (int j = 0;; ++j)  // Increment to skip over the symbol just found by the inner for().
	{
		// Find next non-literal g_DerefChar:
		for (; aArgText[j] && (aArgText[j] != g_DerefChar || (aArgMap && aArgMap[j])); ++j);
		if (!aArgText[j])
			break;
		// else: Match was found; this is the deref's open-symbol.
		if (aDerefCount >= MAX_DEREFS_PER_ARG)
			return ScriptError(TOO_MANY_REFS, aArgText); // Short msg since so rare.
		DerefType &this_deref = aDeref[aDerefCount];  // For performance.
		this_deref.marker = aArgText + j;  // Store the deref's starting location.
		// Find next g_DerefChar, even if it's a literal.
		for (++j; aArgText[j] && aArgText[j] != g_DerefChar; ++j);
		if (!aArgText[j])
			return ScriptError("This parameter contains a variable name missing its ending percent sign.", aArgText);
		// Otherwise: Match was found; this should be the deref's close-symbol.
		if (aArgMap && aArgMap[j])  // But it's mapped as literal g_DerefChar.
			return ScriptError("Invalid `%.", aArgText); // Short msg. since so rare.
		deref_string_length = aArgText + j - this_deref.marker + 1;
		if (deref_string_length == 2) // The percent signs were empty, e.g. %%
			return ScriptError("Empty variable reference (%%).", aArgText); // Short msg. since so rare.
		if (deref_string_length - 2 > MAX_VAR_NAME_LENGTH) // -2 for the opening & closing g_DerefChars
			return ScriptError("Variable name too long.", aArgText); // Short msg. since so rare.
		this_deref.is_function = false;
		this_deref.length = (DerefLengthType)deref_string_length;
		if (   !(this_deref.var = FindOrAddVar(this_deref.marker + 1, this_deref.length - 2))   )
			return FAIL;  // The called function already displayed the error.
		++aDerefCount;
	} // for each dereference.

	return OK;
}



ResultType Script::DefineFunc(char *aBuf, Var *aFuncExceptionVar[])
// Returns OK or FAIL.
// Caller has already called ValidateName() on the function, and it is known that this valid name
// is followed immediately by an open-paren.  aFuncExceptionVar is the address of an array on
// the caller's stack that will hold the list of exception variables (those that must be explicitly
// declared as either local or global) within the body of the function.
{
	char *param_end, *param_start = strchr(aBuf, '('); // Caller has ensured that this will return non-NULL.

	Func *found_func = FindFunc(aBuf, param_start - aBuf);
	if (found_func)
	{
		if (!found_func->mIsBuiltIn)
			return ScriptError("Duplicate function definition.", aBuf); // Seems more descriptive than "Function already defined."
		else // It's a built-in function that the user wants to override with a custom definition.
		{
			found_func->mIsBuiltIn = false;  // Override built-in with custom.
			found_func->mParamCount = 0; // Revert to the default appropriate for non-built-in functions.
			found_func->mMinParams = 0;  //
			found_func->mJumpToLine = NULL; // Fixed for v1.0.35.12: Must reset for detection elsewhere.
			g.CurrentFunc = found_func;
		}
	}
	else
		// The value of g.CurrentFunc must be set here rather than by our caller since AddVar(), which we call,
		// relies upon it having been done.
		if (   !(g.CurrentFunc = AddFunc(aBuf, param_start - aBuf, false))   )
			return FAIL; // It already displayed the error.

	Func &func = *g.CurrentFunc; // For performance and convenience.
	int insert_pos;
	size_t param_length, value_length;
	FuncParam param[MAX_FUNCTION_PARAMS];
	int param_count = 0;
	char buf[LINE_SIZE], *target;
	bool param_must_have_default = false;

	for (param_start = omit_leading_whitespace(param_start + 1);;)
	{
		if (*param_start == ')') // No more params.
			break;

		// Must start the search at param_start, not param_start+1, so that something like fn(, x) will be properly handled:
		if (   !*param_start || !(param_end = StrChrAny(param_start, ", \t=)"))   ) // Look for first comma, space, tab, =, or close-paren.
			return ScriptError(ERR_MISSING_CLOSE_PAREN, aBuf);

		if (param_count >= MAX_FUNCTION_PARAMS)
			return ScriptError("Too many params.", param_start); // Short msg since so rare.
		FuncParam &this_param = param[param_count]; // For performance and convenience.

		// To enhance syntax error catching, consider ByRef to be a keyword; i.e. that can never be the name
		// of a formal parameter:
		if (this_param.is_byref = !strlicmp(param_start, "ByRef", (UINT)(param_end - param_start))) // ByRef.
		{
			// Omit the ByRef keyword from further consideration:
			param_start = omit_leading_whitespace(param_end);
			if (   !*param_start || !(param_end = StrChrAny(param_start, ", \t=)"))   ) // Look for first comma, space, tab, =, or close-paren.
				return ScriptError(ERR_MISSING_CLOSE_PAREN, aBuf);
		}

		if (   !(param_length = param_end - param_start)   )
			return ScriptError(ERR_BLANK_PARAM, aBuf); // Reporting aBuf vs. param_start seems more informative since Vicinity isn't shown.

		// This will search for local variables, never globals, by virtue of the fact that this
		// new function's mDefaultVarType is always VAR_ASSUME_NONE at this early stage of its creation:
		if (this_param.var = FindVar(param_start, param_length, &insert_pos))  // Assign.
			return ScriptError("Duplicate parameter.", param_start);
		if (   !(this_param.var = AddVar(param_start, param_length, insert_pos, 2))   ) // Pass 2 as last parameter to mean "it's a local but more specifically a function's parameter".
			return FAIL; // It already displayed the error, including attempts to have reserved names as parameter names.

		// v1.0.35: Check if a default value is specified for this parameter and set up for the next iteration.
		// The following section is similar to that used to support initializers for static variables.
		// So maybe maintain them together.
		this_param.default_type = PARAM_DEFAULT_NONE;  // Set default.
		param_start = omit_leading_whitespace(param_end);
		if (*param_start == '=') // This is the default value of the param just added.
		{
			param_start = omit_leading_whitespace(param_start + 1); // Start of the default value.
			if (*param_start == '"') // Quoted literal string, or the empty string.
			{
				// v1.0.46.13: Adde support for quoted/literal strings beyond simply "".
				// The following section is nearly identical to one in ExpandExpression().
				// Find the end of this string literal, noting that a pair of double quotes is
				// a literal double quote inside the string.
				for (target = buf, param_end = param_start + 1;;) // Omit the starting-quote from consideration, and from the resulting/built string.
				{
					if (!*param_end) // No matching end-quote. Probably impossible due to load-time validation.
						return ScriptError(ERR_MISSING_CLOSE_QUOTE, param_start); // Reporting param_start vs. aBuf seems more informative in the case of quoted/literal strings.
					if (*param_end == '"') // And if it's not followed immediately by another, this is the end of it.
					{
						++param_end;
						if (*param_end != '"') // String terminator or some non-quote character.
							break;  // The previous char is the ending quote.
						//else a pair of quotes, which resolves to a single literal quote. So fall through
						// to the below, which will copy of quote character to the buffer. Then this pair
						// is skipped over and the loop continues until the real end-quote is found.
					}
					//else some character other than '\0' or '"'.
					*target++ = *param_end++;
				}
				*target = '\0'; // Terminate it in the buffer.
				// The above has also set param_end for use near the bottom of the loop.
				ConvertEscapeSequences(buf, g_EscapeChar, false); // Raw escape sequences like `n haven't been converted yet, so do it now.
				this_param.default_type = PARAM_DEFAULT_STR;
				this_param.default_str = const_cast<char*>(*buf ? SimpleHeap::Malloc(buf, target-buf) : "");
			}
			else // A default value other than a quoted/literal string.
			{
				if (!(param_end = StrChrAny(param_start, ", \t=)"))) // Somewhat debatable but stricter seems better.
					return ScriptError(ERR_MISSING_COMMA, aBuf); // Reporting aBuf vs. param_start seems more informative since Vicinity isn't shown.
				value_length = param_end - param_start;
				if (value_length > MAX_FORMATTED_NUMBER_LENGTH) // Too rare to justify elaborate handling or error reporting.
					value_length = MAX_FORMATTED_NUMBER_LENGTH;
				strlcpy(buf, param_start, value_length + 1);  // Make a temp copy to simplify the below (especially IsPureNumeric).
				if (!stricmp(buf, "false"))
				{
					this_param.default_type = PARAM_DEFAULT_INT;
					this_param.default_int64 = 0;
				}
				else if (!stricmp(buf, "true"))
				{
					this_param.default_type = PARAM_DEFAULT_INT;
					this_param.default_int64 = 1;
				}
				else // The only things supported other than the above are integers and floats.
				{
					// Vars could be supported here via FindVar(), but only globals ABOVE this point in
					// the script would be supported (since other globals don't exist yet). So it seems
					// best to wait until full/comprehesive support for expressions is studied/designed
					// for both static initializers and parameter-default-values.
					switch(IsPureNumeric(buf, true, false, true))
					{
					case PURE_INTEGER:
						this_param.default_type = PARAM_DEFAULT_INT;
						this_param.default_int64 = ATOI64(buf);
						break;
					case PURE_FLOAT:
						this_param.default_type = PARAM_DEFAULT_FLOAT;
						this_param.default_double = ATOF(buf);
						break;
					default: // Not numeric (and also not a quoted string because that was handled earlier).
						return ScriptError("Unsupported parameter default.", aBuf);
					}
				}
			}
			param_must_have_default = true;  // For now, all other params after this one must also have default values.
			// Set up for the next iteration:
			param_start = omit_leading_whitespace(param_end);
		}
		else // This parameter does not have a default value specified.
		{
			if (param_must_have_default)
				return ScriptError("Parameter default required.", this_param.var->mName);
			++func.mMinParams;
		}
		++param_count;

		if (*param_start != ',' && *param_start != ')') // Something like "fn(a, b c)" (missing comma) would cause this.
			return ScriptError(ERR_MISSING_COMMA, aBuf); // Reporting aBuf vs. param_start seems more informative since Vicinity isn't shown.
		if (*param_start == ',')
		{
			param_start = omit_leading_whitespace(param_start + 1);
			if (*param_start == ')') // If *param_start is ',' it will be caught as an error by the next iteration.
				return ScriptError(ERR_BLANK_PARAM, aBuf); // Reporting aBuf vs. param_start seems more informative since Vicinity isn't shown.
		}
		//else it's ')', in which case the next iteration will handle it.
		// Above has ensured that param_start now points to the next parameter, or ')' if none.
	} // for() each formal parameter.

	if (param_count)
	{
		// Allocate memory only for the actual number of parameters actually present.
		size_t size = param_count * sizeof(param[0]);
		if (   !(func.mParam = (FuncParam *)SimpleHeap::Malloc(size))   )
			return ScriptError(ERR_OUTOFMEM);
		func.mParamCount = param_count;
		memcpy(func.mParam, param, size);
	}
	//else leave func.mParam/mParamCount set to their NULL/0 defaults.

	// Indicate success:
	mFuncExceptionVar = aFuncExceptionVar; // Give mFuncExceptionVar its address, to be used for any var declarations inside this function's body.
	mFuncExceptionVarCount = 0;  // Reset in preparation of declarations that appear beneath this function's definition.
	return OK;
}



#ifndef AUTOHOTKEYSC
struct FuncLibrary
{
	char *path;
	DWORD length;
};

Func *Script::FindFuncInLibrary(char *aFuncName, size_t aFuncNameLength, bool &aErrorWasShown)
// Caller must ensure that aFuncName doesn't already exist as a defined function.
// If aFuncNameLength is 0, the entire length of aFuncName is used.
{
	aErrorWasShown = false; // Set default for this output parameter.

	int i;
	char *char_after_last_backslash, *terminate_here;
	DWORD attr;

	#define FUNC_LIB_EXT ".ahk"
	#define FUNC_LIB_EXT_LENGTH 4
	#define FUNC_USER_LIB "\\AutoHotkey\\Lib\\" // Needs leading and trailing backslash.
	#define FUNC_USER_LIB_LENGTH 16
	#define FUNC_STD_LIB "Lib\\" // Needs trailing but not leading backslash.
	#define FUNC_STD_LIB_LENGTH 4

	#define FUNC_LIB_COUNT 2
	static FuncLibrary sLib[FUNC_LIB_COUNT] = {0};

	if (!sLib[0].path) // Allocate & discover paths only upon first use because many scripts won't use anything from the library. This saves a bit of memory and performance.
	{
		for (i = 0; i < FUNC_LIB_COUNT; ++i)
			if (   !(sLib[i].path = SimpleHeap::Malloc(MAX_PATH))   ) // Need MAX_PATH for to allow room for appending each candidate file/function name.
				return NULL; // Due to rarity, simply pass the failure back to caller.

		// DETERMINE PATH TO "USER" LIBRARY:
		FuncLibrary *this_lib = sLib; // For convenience and maintainability.
		this_lib->length = BIV_MyDocuments(this_lib->path, "");
		if (this_lib->length < MAX_PATH-FUNC_USER_LIB_LENGTH)
		{
			strcpy(this_lib->path + this_lib->length, FUNC_USER_LIB);
			this_lib->length += FUNC_USER_LIB_LENGTH;
		}
		else // Insufficient room to build the path name.
		{
			*this_lib->path = '\0'; // Mark this library as disabled.
			this_lib->length = 0;   //
		}

		// DETERMINE PATH TO "STANDARD" LIBRARY:
		this_lib = sLib + 1; // For convenience and maintainability.
		GetModuleFileName(NULL, this_lib->path, MAX_PATH); // The full path to the currently-running AutoHotkey.exe.
		char_after_last_backslash = 1 + strrchr(this_lib->path, '\\'); // Should always be found, so failure isn't checked.
		this_lib->length = (DWORD)(char_after_last_backslash - this_lib->path); // The length up to and including the last backslash.
		if (this_lib->length < MAX_PATH-FUNC_STD_LIB_LENGTH)
		{
			strcpy(this_lib->path + this_lib->length, FUNC_STD_LIB);
			this_lib->length += FUNC_STD_LIB_LENGTH;
		}
		else // Insufficient room to build the path name.
		{
			*this_lib->path = '\0'; // Mark this library as disabled.
			this_lib->length = 0;   //
		}

		for (i = 0; i < FUNC_LIB_COUNT; ++i)
		{
			attr = GetFileAttributes(sLib[i].path); // Seems to accept directories that have a trailing backslash, which is good because it simplifies the code.
			if (attr == 0xFFFFFFFF || !(attr & FILE_ATTRIBUTE_DIRECTORY)) // Directory doesn't exist or it's a file vs. directory. Relies on short-circuit boolean order.
			{
				*sLib[i].path = '\0'; // Mark this library as disabled.
				sLib[i].length = 0;   //
			}
		}
	}
	// Above must ensure that all sLib[].path elements are non-NULL (but they can be "" to indicate "no library").

	if (!aFuncNameLength) // Caller didn't specify, so use the entire string.
		aFuncNameLength = strlen(aFuncName);

	char *dest, *first_underscore, class_name_buf[MAX_VAR_NAME_LENGTH + 1];
	char *naked_filename = aFuncName;               // Set up for the first iteration.
	size_t naked_filename_length = aFuncNameLength; //

	for (int second_iteration = 0; second_iteration < 2; ++second_iteration)
	{
		for (i = 0; i < FUNC_LIB_COUNT; ++i)
		{
			if (!*sLib[i].path) // Library is marked disabled, so skip it.
				continue;

			if (sLib[i].length + naked_filename_length >= MAX_PATH-FUNC_LIB_EXT_LENGTH)
				continue; // Path too long to match in this library, but try others.
			dest = (char *)memcpy(sLib[i].path + sLib[i].length, naked_filename, naked_filename_length); // Append the filename to the library path.
			strcpy(dest + naked_filename_length, FUNC_LIB_EXT); // Append the file extension.

			attr = GetFileAttributes(sLib[i].path); // Testing confirms that GetFileAttributes() doesn't support wildcards; which is good because we want filenames containing question marks to be "not found" rather than being treated as a match-pattern.
			if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) // File doesn't exist or it's a directory. Relies on short-circuit boolean order.
				continue;

			// Since above didn't "continue", a file exists whose name matches that of the requested function.
			// Before loading/including that file, set the working directory to its folder so that if it uses
			// #Include, it will be able to use more convenient/intuitive relative paths.  This is similar to
			// the "#Include DirName" feature.
			// Call SetWorkingDir() vs. SetCurrentDirectory() so that it succeeds even for a root drive like
			// C: that lacks a backslash (see SetWorkingDir() for details).
			terminate_here = sLib[i].path + sLib[i].length - 1; // The trailing backslash in the full-path-name to this library.
			*terminate_here = '\0'; // Temporarily terminate it for use with SetWorkingDir().
			SetWorkingDir(sLib[i].path); // See similar section in the #Include directive.
			*terminate_here = '\\'; // Undo the termination.

			if (!LoadIncludedFile(sLib[i].path, false, false)) // Fix for v1.0.47.05: Pass false for allow-dupe because otherwise, it's possible for a stdlib file to attempt to include itself (especially via the LibNamePrefix_ method) and thus give a misleading "duplicate function" vs. "func does not exist" error message.  Obsolete: For performance, pass true for allow-dupe so that it doesn't have to check for a duplicate file (seems too rare to worry about duplicates since by definition, the function doesn't yet exist so it's file shouldn't yet be included).
			{
				aErrorWasShown = true; // Above has just displayed its error (e.g. syntax error in a line, failed to open the include file, etc).  So override the default set earlier.
				return NULL;
			}

			if (mIncludeLibraryFunctionsThenExit)
			{
				// For each included library-file, write out two #Include lines:
				// 1) Use #Include in its "change working directory" mode so that any explicit #include directives
				//    or FileInstalls inside the library file itself will work consistently and properly.
				// 2) Use #IncludeAgain (to improve performance since no dupe-checking is needed) to include
				//    the library file itself.
				// We don't directly append library files onto the main script here because:
				// 1) ahk2exe needs to be able to see and act upon FileInstall and #Include lines (i.e. library files
				//    might contain #Include even though it's rare).
				// 2) #IncludeAgain and #Include directives that bring in fragments rather than entire functions or
				//    subroutines wouldn't work properly if we resolved such includes in AutoHotkey.exe because they
				//    wouldn't be properly interleaved/asynchronous, but instead brought out of their library file
				//    and deposited separately/synchronously into the temp-include file by some new logic at the
				//    AutoHotkey.exe's code for the #Include directive.
				// 3) ahk2exe prefers to omit comments from included files to minimize size of compiled scripts.
				fprintf(mIncludeLibraryFunctionsThenExit, "#Include %-0.*s\n#IncludeAgain %s\n"
					, sLib[i].length, sLib[i].path, sLib[i].path);
				// Now continue on normally so that our caller can continue looking for syntax errors.
			}

			// Now that a matching filename has been found, it seems best to stop searching here even if that
			// file doesn't actually contain the requested function.  This helps library authors catch bugs/typos.
			return FindFunc(aFuncName, aFuncNameLength);
		} // for() each library directory.

		// Now that the first iteration is done, set up for the second one that searches by class/prefix.
		// Notes about ambiguity and naming collisions:
		// By the time it gets to the prefix/class search, it's almost given up.  Even if it wrongly finds a
		// match in a filename that isn't really a class, it seems inconsequential because at worst it will
		// still not find the function and will then say "call to nonexistent function".  In addition, the
		// ability to customize which libraries are searched is planned.  This would allow a publicly
		// distributed script to turn off all libraries except stdlib.
		if (   !(first_underscore = strchr(aFuncName, '_'))   ) // No second iteration needed.
			break; // All loops are done because second iteration is the last possible attempt.
		naked_filename_length = first_underscore - aFuncName;
		if (naked_filename_length >= sizeof(class_name_buf)) // Class name too long (probably impossible currently).
			break; // All loops are done because second iteration is the last possible attempt.
		naked_filename = class_name_buf; // Point it to a buffer for use below.
		memcpy(naked_filename, aFuncName, naked_filename_length);
		naked_filename[naked_filename_length] = '\0';
	} // 2-iteration for().

	// Since above didn't return, no match found in any library.
	return NULL;
}
#endif



Func *Script::FindFunc(char *aFuncName, size_t aFuncNameLength)
// Returns the Function whose name matches aFuncName (which caller has ensured isn't NULL).
// If it doesn't exist, NULL is returned.
{
	if (!aFuncNameLength) // Caller didn't specify, so use the entire string.
		aFuncNameLength = strlen(aFuncName);

	// For the below, no error is reported because callers don't want that.  Instead, simply return
	// NULL to indicate that names that are illegal or too long are not found.  If the caller later
	// tries to add the function, it will get an error then:
	if (aFuncNameLength > MAX_VAR_NAME_LENGTH)
		return NULL;

	// The following copy is made because it allows the name searching to use stricmp() instead of
	// strlicmp(), which close to doubles the performance.  The copy includes only the first aVarNameLength
	// characters from aVarName:
	char func_name[MAX_VAR_NAME_LENGTH + 1];
	strlcpy(func_name, aFuncName, aFuncNameLength + 1);  // +1 to convert length to size.

	Func *pfunc;
	for (pfunc = mFirstFunc; pfunc; pfunc = pfunc->mNextFunc)
		if (!stricmp(func_name, pfunc->mName)) // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
			return pfunc; // Match found.

	// Since above didn't return, there is no match.  See if it's a built-in function that hasn't yet
	// been added to the function list.

	// Set defaults to be possibly overridden below:
	int min_params = 1;
	int max_params = 1;
	BuiltInFunctionType bif;
	char *suffix = func_name + 3;

	if (!strnicmp(func_name, "LV_", 3)) // It's a ListView function.
	{
		suffix = func_name + 3;
		if (!stricmp(suffix, "GetNext"))
		{
			bif = BIF_LV_GetNextOrCount;
			min_params = 0;
			max_params = 2;
		}
		else if (!stricmp(suffix, "GetCount"))
		{
			bif = BIF_LV_GetNextOrCount;
			min_params = 0; // But leave max at its default of 1.
		}
		else if (!stricmp(suffix, "GetText"))
		{
			bif = BIF_LV_GetText;
			min_params = 2;
			max_params = 3;
		}
		else if (!stricmp(suffix, "Add"))
		{
			bif = BIF_LV_AddInsertModify;
			min_params = 0; // 0 params means append a blank row.
			max_params = 10000; // An arbitrarily high limit that will never realistically be reached.
		}
		else if (!stricmp(suffix, "Insert"))
		{
			bif = BIF_LV_AddInsertModify;
			// Leave min_params at 1.  Passing only 1 param to it means "insert a blank row".
			max_params = 10000; // An arbitrarily high limit that will never realistically be reached.
		}
		else if (!stricmp(suffix, "Modify"))
		{
			bif = BIF_LV_AddInsertModify; // Although it shares the same function with "Insert", it can still have its own min/max params.
			min_params = 2;
			max_params = 10000; // An arbitrarily high limit that will never realistically be reached.
		}
		else if (!stricmp(suffix, "Delete"))
		{
			bif = BIF_LV_Delete;
			min_params = 0; // Leave max at its default of 1.
		}
		else if (!stricmp(suffix, "InsertCol"))
		{
			bif = BIF_LV_InsertModifyDeleteCol;
			// Leave min_params at 1 because inserting a blank column ahead of the first column
			// does not seem useful enough to sacrifice the no-parameter mode, which might have
			// potential future uses.
			max_params = 3;
		}
		else if (!stricmp(suffix, "ModifyCol"))
		{
			bif = BIF_LV_InsertModifyDeleteCol;
			min_params = 0;
			max_params = 3;
		}
		else if (!stricmp(suffix, "DeleteCol"))
			bif = BIF_LV_InsertModifyDeleteCol; // Leave min/max set to 1.
		else if (!stricmp(suffix, "SetImageList"))
		{
			bif = BIF_LV_SetImageList;
			max_params = 2; // Leave min at 1.
		}
		else
			return NULL;
	}
	else if (!strnicmp(func_name, "TV_", 3)) // It's a TreeView function.
	{
		suffix = func_name + 3;
		if (!stricmp(suffix, "Add"))
		{
			bif = BIF_TV_AddModifyDelete;
			max_params = 3; // Leave min at its default of 1.
		}
		else if (!stricmp(suffix, "Modify"))
		{
			bif = BIF_TV_AddModifyDelete;
			max_params = 3; // One-parameter mode is "select specified item".
		}
		else if (!stricmp(suffix, "Delete"))
		{
			bif = BIF_TV_AddModifyDelete;
			min_params = 0;
		}
		else if (!stricmp(suffix, "GetParent") || !stricmp(suffix, "GetChild") || !stricmp(suffix, "GetPrev"))
			bif = BIF_TV_GetRelatedItem;
		else if (!stricmp(suffix, "GetCount") || !stricmp(suffix, "GetSelection"))
		{
			bif = BIF_TV_GetRelatedItem;
			min_params = 0;
			max_params = 0;
		}
		else if (!stricmp(suffix, "GetNext")) // Unlike "Prev", Next also supports 0 or 2 parameters.
		{
			bif = BIF_TV_GetRelatedItem;
			min_params = 0;
			max_params = 2;
		}
		else if (!stricmp(suffix, "Get") || !stricmp(suffix, "GetText"))
		{
			bif = BIF_TV_Get;
			min_params = 2;
			max_params = 2;
		}
		else
			return NULL;
	}
	else if (!strnicmp(func_name, "IL_", 3)) // It's an ImageList function.
	{
		suffix = func_name + 3;
		if (!stricmp(suffix, "Create"))
		{
			bif = BIF_IL_Create;
			min_params = 0;
			max_params = 3;
		}
		else if (!stricmp(suffix, "Destroy"))
		{
			bif = BIF_IL_Destroy; // Leave Min/Max set to 1.
		}
		else if (!stricmp(suffix, "Add"))
		{
			bif = BIF_IL_Add;
			min_params = 2;
			max_params = 4;
		}
		else
			return NULL;
	}
	else if (!stricmp(func_name, "SB_SetText"))
	{
		bif = BIF_StatusBar;
		max_params = 3; // Leave min_params at its default of 1.
	}
	else if (!stricmp(func_name, "SB_SetParts"))
	{
		bif = BIF_StatusBar;
		min_params = 0;
		max_params = 255; // 255 params alllows for up to 256 parts, which is SB's max.
	}
	else if (!stricmp(func_name, "SB_SetIcon"))
	{
		bif = BIF_StatusBar;
		max_params = 3; // Leave min_params at its default of 1.
	}
	else if (!stricmp(func_name, "StrLen"))
		bif = BIF_StrLen;
	else if (!stricmp(func_name, "SubStr"))
	{
		bif = BIF_SubStr;
		min_params = 2;
		max_params = 3;
	}
			else if (!stricmp(func_name, "Import"))  // addFile() Naveen v8.
	{
		bif = BIF_Import;
		min_params = 1;
		max_params = 3;
	}
else if (!stricmp(func_name, "Static"))  // lowlevel() Naveen v9.
	{
		bif = BIF_Static;
		min_params = 1;
		max_params = 1;
	}
	else if (!stricmp(func_name, "Alias"))  // lowlevel() Naveen v9.
	{
		bif = BIF_Alias;
		min_params = 1;
		max_params = 2;
	}
else if (!stricmp(func_name, "GetTokenValue"))  // lowlevel() Naveen v9.
	{
		bif = BIF_GetTokenValue;
		min_params = 1;
		max_params = 1;
	}

else if (!stricmp(func_name, "CacheEnable"))  // lowlevel() Naveen v9.
	{
		bif = BIF_CacheEnable;
		min_params = 1;
		max_params = 1;
	}

	else if (!stricmp(func_name, "Getvar"))  // lowlevel() Naveen v9.
	{
		bif = BIF_Getvar;
		min_params = 1;
		max_params = 1;
	}
	else if (!stricmp(func_name, "InStr"))
	{
		bif = BIF_InStr;
		min_params = 2;
		max_params = 4;
	}
	else if (!stricmp(func_name, "RegExMatch"))
	{
		bif = BIF_RegEx;
		min_params = 2;
		max_params = 4;
	}
	else if (!stricmp(func_name, "RegExReplace"))
	{
		bif = BIF_RegEx;
		min_params = 2;
		max_params = 6;
	}
	else if (!stricmp(func_name, "GetKeyState"))
	{
		bif = BIF_GetKeyState;
		max_params = 2;
	}
	else if (!stricmp(func_name, "Asc"))
		bif = BIF_Asc;
	else if (!stricmp(func_name, "Chr"))
		bif = BIF_Chr;
	else if (!stricmp(func_name, "NumGet"))
	{
		bif = BIF_NumGet;
		max_params = 3;
	}
	else if (!stricmp(func_name, "NumPut"))
	{
		bif = BIF_NumPut;
		min_params = 2;
		max_params = 4;
	}
	else if (!stricmp(func_name, "IsLabel"))
		bif = BIF_IsLabel;
	else if (!stricmp(func_name, "DllCall"))
	{
		bif = BIF_DllCall;
		max_params = 10000; // An arbitrarily high limit that will never realistically be reached.
	}
	else if (!stricmp(func_name, "VarSetCapacity"))
	{
		bif = BIF_VarSetCapacity;
		max_params = 3;
	}
	else if (!stricmp(func_name, "FileExist"))
		bif = BIF_FileExist;
	else if (!stricmp(func_name, "WinExist") || !stricmp(func_name, "WinActive"))
	{
		bif = BIF_WinExistActive;
		min_params = 0;
		max_params = 4;
	}
	else if (!stricmp(func_name, "Round"))
	{
		bif = BIF_Round;
		max_params = 2;
	}
	else if (!stricmp(func_name, "Floor") || !stricmp(func_name, "Ceil"))
		bif = BIF_FloorCeil;
	else if (!stricmp(func_name, "Mod"))
	{
		bif = BIF_Mod;
		min_params = 2;
		max_params = 2;
	}
	else if (!stricmp(func_name, "Abs"))
		bif = BIF_Abs;
	else if (!stricmp(func_name, "Sin"))
		bif = BIF_Sin;
	else if (!stricmp(func_name, "Cos"))
		bif = BIF_Cos;
	else if (!stricmp(func_name, "Tan"))
		bif = BIF_Tan;
	else if (!stricmp(func_name, "ASin") || !stricmp(func_name, "ACos"))
		bif = BIF_ASinACos;
	else if (!stricmp(func_name, "ATan"))
		bif = BIF_ATan;
	else if (!stricmp(func_name, "Exp"))
		bif = BIF_Exp;
	else if (!stricmp(func_name, "Sqrt") || !stricmp(func_name, "Log") || !stricmp(func_name, "Ln"))
		bif = BIF_SqrtLogLn;
	else if (!stricmp(func_name, "OnMessage"))
	{
		bif = BIF_OnMessage;
		max_params = 3;  // Leave min at 1.
		// By design, scripts that use OnMessage are persistent by default.  Doing this here
		// also allows WinMain() to later detect whether this script should become #SingleInstance.
		// Note: Don't directly change g_AllowOnlyOneInstance here in case the remainder of the
		// script-loading process comes across any explicit uses of #SingleInstance, which would
		// override the default set here.
		g_persistent = true;
	}
	else if (!stricmp(func_name, "RegisterCallback"))
	{
		bif = BIF_RegisterCallback;
		max_params = 4; // Leave min_params at 1.
	}
	else
		return NULL; // Maint: There may be other lines above that also return NULL.

	// Since above didn't return, this is a built-in function that hasn't yet been added to the list.
	// Add it now:
	if (   !(pfunc = AddFunc(func_name, aFuncNameLength, true))   )
		return NULL;

	pfunc->mBIF = bif;
	pfunc->mMinParams = min_params;
	pfunc->mParamCount = max_params;

	return pfunc;
}



Func *Script::AddFunc(char *aFuncName, size_t aFuncNameLength, bool aIsBuiltIn)
// This function should probably not be called by anyone except FindOrAddFunc, which has already done
// the dupe-checking.
// Returns the address of the new function or NULL on failure.
// The caller must already have verified that this isn't a duplicate function.
{
	if (!aFuncNameLength) // Caller didn't specify, so use the entire string.
		aFuncNameLength = strlen(aFuncName);

	if (aFuncNameLength > MAX_VAR_NAME_LENGTH)
	{
		// Dynamic function-calls such as MyFuncArray%i%() aren't currently supported, so the first
		// item below is commented out:
		// Load-time callers should check for this.  But at runtime, it's possible for a dynamically
		// resolved function name to be too long.  Note that aFuncName should be the exact variable
		// name and does not need to be truncated to aFuncNameLength whenever this error occurs
		// (i.e. at runtime):
		//if (mIsReadyToExecute) // Runtime error.
		//	ScriptError("Function name too long." ERR_ABORT, aFuncName);
		//else
			ScriptError("Function name too long.", aFuncName);
		return NULL;
	}

	// Make a temporary copy that includes only the first aFuncNameLength characters from aFuncName:
	char func_name[MAX_VAR_NAME_LENGTH + 1];
	strlcpy(func_name, aFuncName, aFuncNameLength + 1);  // See explanation above.  +1 to convert length to size.

	// In the future, it might be best to add another check here to disallow function names that consist
	// entirely of numbers.  However, this hasn't been done yet because:
	// 1) Not sure if there will ever be a good enough reason.
	// 2) Even if it's done in the far future, it won't break many scripts (pure-numeric functions should be very rare).
	// 3) Those scripts that are broken are not broken in a bad way because the pre-parser will generate a
	//    load-time error, which is easy to fix (unlike runtime errors, which require that part of the script
	//    to actually execute).
	if (!Var::ValidateName(func_name, mIsReadyToExecute, DISPLAY_FUNC_ERROR))  // Variable and function names are both validated the same way.
		// Above already displayed error for us.  This can happen at loadtime or runtime (e.g. StringSplit).
		return NULL;

	// Allocate some dynamic memory to pass to the constructor:
	char *new_name = SimpleHeap::Malloc(func_name, aFuncNameLength);
	if (!new_name)
		// It already displayed the error for us.  These mem errors are so unusual that we're not going
		// to bother varying the error message to include ERR_ABORT if this occurs during runtime.
		return NULL;

	Func *the_new_func = new Func(new_name, aIsBuiltIn);
	if (!the_new_func)
	{
		ScriptError(ERR_OUTOFMEM);
		return NULL;
	}

	// v1.0.47: The following ISN'T done because it would slow down commonly used functions. This is because
	// commonly-called functions like InStr() tend to be added first (since they appear so often throughout
	// the script); thus subsequent lookups are fast if they are kept at the beginning of the list rather
	// than being displaced to the end by all other functions).
	// NOT DONE for the reason above:
	// Unlike most of the other link lists, attach new items at the beginning of the list because
	// that allows the standard/user library feature to perform much better for scripts that have hundreds
	// of functions.  This is because functions brought in dynamically from a library will then be at the
	// beginning of the list, which allows the function lookup that immediately follows library-loading to
	// find a match almost immediately.
	if (!mFirstFunc) // The list is empty, so this will be the first and last item.
		mFirstFunc = the_new_func;
	else
		mLastFunc->mNextFunc = the_new_func;
	// This must be done after the above:
	mLastFunc = the_new_func; // There's at least one spot in the code that relies on mLastFunc being the most recently added function.

	return the_new_func;
}



size_t Line::ArgLength(int aArgNum)
// "ArgLength" is the arg's fully resolved, dereferenced length during runtime.
// Callers must call this only at times when sArgDeref and sArgVar are defined/meaningful.
// Caller must ensure that aArgNum should be 1 or greater.
// ArgLength() was added in v1.0.44.14 to help its callers improve performance by avoiding
// costly calls to strlen() (which is especially beneficial for huge strings).
{
#ifdef _DEBUG
	if (aArgNum < 1)
	{
		LineError("DEBUG: BAD", WARN);
		aArgNum = 1;  // But let it continue.
	}
#endif
	if (aArgNum > mArgc) // Arg doesn't exist, so don't try accessing sArgVar (unlike sArgDeref, it wouldn't be valid to do so).
		return 0; // i.e. treat it as the empty string.
	// The length is not known and must be calculcated in the following situations:
	// - The arg consists of more than just a single isolated variable name (not possible if the arg is
	//   ARG_TYPE_INPUT_VAR).
	// - The arg is a built-in variable, in which case the length isn't known, so it must be derived from
	//   the string copied into sArgDeref[] by an earlier stage.
	// - The arg is a normal variable but it's VAR_ATTRIB_BINARY_CLIP. In such cases, our callers do not
	//   recognize/support binary-clipboard as binary and want the apparent length of the string returned
	//   (i.e. strlen(), which takes into account the position of the first binary zero wherever it may be).
	--aArgNum; // Convert to zero-based index (squeeze a little more performance out of it by avoiding a new variable).
	if (sArgVar[aArgNum])
	{
		Var &var = *sArgVar[aArgNum]; // For performance and convenience.
		if (var.Type() == VAR_NORMAL && (g_NoEnv || var.Length())) // v1.0.46.02: Recognize environment variables (when g_NoEnv==false) by falling through to strlen() for them.
			return var.LengthIgnoreBinaryClip(); // Do it the fast way (unless it's binary clipboard, in which case this call will internally call strlen()).
	}
	// Otherwise, length isn't known due to no variable, a built-in variable, or an environment variable.
	// So do it the slow way.
	return strlen(sArgDeref[aArgNum]);
}



Var *Line::ResolveVarOfArg(int aArgIndex, bool aCreateIfNecessary)
// Returns NULL on failure.  Caller has ensured that none of this arg's derefs are function-calls.
// Args that are input or output variables are normally resolved at load-time, so that
// they contain a pointer to their Var object.  This is done for performance.  However,
// in order to support dynamically resolved variables names like AutoIt2 (e.g. arrays),
// we have to do some extra work here at runtime.
// Callers specify false for aCreateIfNecessary whenever the contents of the variable
// they're trying to find is unimportant.  For example, dynamically built input variables,
// such as "StringLen, length, array%i%", do not need to be created if they weren't
// previously assigned to (i.e. they weren't previously used as an output variable).
// In the above example, the array element would never be created here.  But if the output
// variable were dynamic, our call would have told us to create it.
{
	// The requested ARG isn't even present, so it can't have a variable.  Currently, this should
	// never happen because the loading procedure ensures that input/output args are not marked
	// as variables if they are blank (and our caller should check for this and not call in that case):
	if (aArgIndex >= mArgc)
		return NULL;
	ArgStruct &this_arg = mArg[aArgIndex]; // For performance and convenience.

	// Since this function isn't inline (since it's called so frequently), there isn't that much more
	// overhead to doing this check, even though it shouldn't be needed since it's the caller's
	// responsibility:
	if (this_arg.type == ARG_TYPE_NORMAL) // Arg isn't an input or output variable.
		return NULL;
	if (!*this_arg.text) // The arg's variable is not one that needs to be dynamically resolved.
		return VAR(this_arg); // Return the var's address that was already determined at load-time.
	// The above might return NULL in the case where the arg is optional (i.e. the command allows
	// the var name to be omitted).  But in that case, the caller should either never have called this
	// function or should check for NULL upon return.  UPDATE: This actually never happens, see
	// comment above the "if (aArgIndex >= mArgc)" line.

	// Static to correspond to the static empty_var further below.  It needs the memory area
	// to support resolving dynamic environment variables.  In the following example,
	// the result will be blank unless the first line is present (without this fix here):
	//null = %SystemRoot%  ; bogus line as a required workaround in versions prior to v1.0.16
	//thing = SystemRoot
	//StringTrimLeft, output, %thing%, 0
	//msgbox %output%

	static char sVarName[MAX_VAR_NAME_LENGTH + 1];  // Will hold the dynamically built name.

	// At this point, we know the requested arg is a variable that must be dynamically resolved.
	// This section is similar to that in ExpandArg(), so they should be maintained together:
	char *pText = this_arg.text; // Start at the begining of this arg's text.
	int var_name_length = 0;

	if (this_arg.deref) // There's at least one deref.
	{
		// Caller has ensured that none of these derefs are function calls (i.e. deref->is_function is alway false).
		for (DerefType *deref = this_arg.deref  // Start off by looking for the first deref.
			; deref->marker; ++deref)  // A deref with a NULL marker terminates the list.
		{
			// FOR EACH DEREF IN AN ARG (if we're here, there's at least one):
			// Copy the chars that occur prior to deref->marker into the buffer:
			for (; pText < deref->marker && var_name_length < MAX_VAR_NAME_LENGTH; sVarName[var_name_length++] = *pText++);
			if (var_name_length >= MAX_VAR_NAME_LENGTH && pText < deref->marker) // The variable name would be too long!
			{
				// This type of error is just a warning because this function isn't set up to cause a true
				// failure.  This is because the use of dynamically named variables is rare, and only for
				// people who should know what they're doing.  In any case, when the caller of this
				// function called it to resolve an output variable, it will see tha the result is
				// NULL and terminate the current subroutine.
				#define DYNAMIC_TOO_LONG "This dynamically built variable name is too long." \
					"  If this variable was not intended to be dynamic, remove the % symbols from it."
				LineError(DYNAMIC_TOO_LONG, FAIL, this_arg.text);
				return NULL;
			}
			// Now copy the contents of the dereferenced var.  For all cases, aBuf has already
			// been verified to be large enough, assuming the value hasn't changed between the
			// time we were called and the time the caller calculated the space needed.
			if (deref->var->Get() > (VarSizeType)(MAX_VAR_NAME_LENGTH - var_name_length)) // The variable name would be too long!
			{
				LineError(DYNAMIC_TOO_LONG, FAIL, this_arg.text);
				return NULL;
			}
			var_name_length += deref->var->Get(sVarName + var_name_length);
			// Finally, jump over the dereference text. Note that in the case of an expression, there might not
			// be any percent signs within the text of the dereference, e.g. x + y, not %x% + %y%.
			pText += deref->length;
		}
	}

	// Copy any chars that occur after the final deref into the buffer:
	for (; *pText && var_name_length < MAX_VAR_NAME_LENGTH; sVarName[var_name_length++] = *pText++);
	if (var_name_length >= MAX_VAR_NAME_LENGTH && *pText) // The variable name would be too long!
	{
		LineError(DYNAMIC_TOO_LONG, FAIL, this_arg.text);
		return NULL;
	}

	if (!var_name_length)
	{
		LineError("This dynamic variable is blank. If this variable was not intended to be dynamic,"
			" remove the % symbols from it.", FAIL, this_arg.text);
		return NULL;
	}

	// Terminate the buffer, even if nothing was written into it:
	sVarName[var_name_length] = '\0';

	static Var empty_var(sVarName, (void *)VAR_NORMAL, false); // Must use sVarName here.  See comment above for why.

	Var *found_var;
	if (!aCreateIfNecessary)
	{
		// Now we've dynamically build the variable name.  It's possible that the name is illegal,
		// so check that (the name is automatically checked by FindOrAddVar(), so we only need to
		// check it if we're not calling that):
		if (!Var::ValidateName(sVarName, g_script.mIsReadyToExecute))
			return NULL; // Above already displayed error for us.
		// The use of ALWAYS_PREFER_LOCAL below improves flexibility of assume-global functions
		// by allowing this command to resolve to a local first if such a local exists:
		if (found_var = g_script.FindVar(sVarName, var_name_length, NULL, ALWAYS_PREFER_LOCAL)) // Assign.
			return found_var;
		// At this point, this is either a non-existent variable or a reserved/built-in variable
		// that was never statically referenced in the script (only dynamically), e.g. A_IPAddress%A_Index%
		if (Script::GetVarType(sVarName) == (void *)VAR_NORMAL)
			// If not found: for performance reasons, don't create it because caller just wants an empty variable.
			return &empty_var;
		//else it's the clipboard or some other built-in variable, so continue onward so that the
		// variable gets created in the variable list, which is necessary to allow it to be properly
		// dereferenced, e.g. in a script consisting of only the following:
		// Loop, 4
		//     StringTrimRight, IP, A_IPAddress%A_Index%, 0
	}
	// Otherwise, aCreateIfNecessary is true or we want to create this variable unconditionally for the
	// reason described above.  ALWAYS_PREFER_LOCAL is used so that any existing local variable will
	// take precedence over a global of the same name when assume-global is in effect.  If neither type
	// of variable exists, a global variable will be created if assume-global is in effect.
	if (   !(found_var = g_script.FindOrAddVar(sVarName, var_name_length, ALWAYS_PREFER_LOCAL))   )
		return NULL;  // Above will already have displayed the error.
	if (this_arg.type == ARG_TYPE_OUTPUT_VAR && VAR_IS_READONLY(*found_var))
	{
		LineError(ERR_VAR_IS_READONLY, FAIL, sVarName);
		return NULL;  // Don't return the var, preventing the caller from assigning to it.
	}
	else
		return found_var;
}



Var *Script::FindOrAddVar(char *aVarName, size_t aVarNameLength, int aAlwaysUse, bool *apIsException)
// Caller has ensured that aVarName isn't NULL.
// Returns the Var whose name matches aVarName.  If it doesn't exist, it is created.
{
	if (!*aVarName)
		return NULL;
	int insert_pos;
	bool is_local; // Used to detect which type of var should be added in case the result of the below is NULL.
	Var *var;
	if (var = FindVar(aVarName, aVarNameLength, &insert_pos, aAlwaysUse, apIsException, &is_local))
		return var;
	// Otherwise, no match found, so create a new var.  This will return NULL if there was a problem,
	// in which case AddVar() will already have displayed the error:
	return AddVar(aVarName, aVarNameLength, insert_pos, is_local);
}



Var *Script::FindVar(char *aVarName, size_t aVarNameLength, int *apInsertPos, int aAlwaysUse
	, bool *apIsException, bool *apIsLocal)
// Caller has ensured that aVarName isn't NULL.  It must also ignore the contents of apInsertPos when
// a match (non-NULL value) is returned.
// Returns the Var whose name matches aVarName.  If it doesn't exist, NULL is returned.
// If caller provided a non-NULL apInsertPos, it will be given a the array index that a newly
// inserted item should have to keep the list in sorted order (which also allows the ListVars command
// to display the variables in alphabetical order).
{
	if (!*aVarName)
		return NULL;
	if (!aVarNameLength) // Caller didn't specify, so use the entire string.
		aVarNameLength = strlen(aVarName);

	// For the below, no error is reported because callers don't want that.  Instead, simply return
	// NULL to indicate that names that are illegal or too long are not found.  When the caller later
	// tries to add the variable, it will get an error then:
	if (aVarNameLength > MAX_VAR_NAME_LENGTH)
		return NULL;

	// The following copy is made because it allows the various searches below to use stricmp() instead of
	// strlicmp(), which close to doubles their performance.  The copy includes only the first aVarNameLength
	// characters from aVarName:
	char var_name[MAX_VAR_NAME_LENGTH + 1];
	strlcpy(var_name, aVarName, aVarNameLength + 1);  // +1 to convert length to size.

	Var *found_var = NULL; // Set default.
	bool is_local;
	if (aAlwaysUse == ALWAYS_USE_GLOBAL)
		is_local = false;
	else if (aAlwaysUse == ALWAYS_USE_LOCAL)
		// v1.0.44.10: The following was changed from it's former value of "true" so that places further below
		// (including passing is_local is call to AddVar()) don't have to ensure that g.CurrentFunc!=NULL.
		// This fixes a crash that occured when a caller specified ALWAYS_USE_LOCAL even though the current
		// thread isn't actually inside a *called* function (perhaps meaning things like a timed subroutine
		// that lies inside a "container function").
		// Some callers like SYSGET_CMD_MONITORAREA might try to find/add a local array if they see that their
		// base variable is classified as local (such classification occurs at loadtime, but only for non-dynamic
		// variable references).  But the current thread entered a "container function" by means other than a
		// function-call (such as SetTimer), not only is g.CurrentFunc NULL, but there's no easy way to discover
		// which function owns the currently executing line (a means could be added to the class "Var" or "Line"
		// but doesn't seem worth it yet due to performance and memory reduction).
		is_local = (g.CurrentFunc != NULL);
	else if (aAlwaysUse == ALWAYS_PREFER_LOCAL)
	{
		if (g.CurrentFunc) // Caller relies on us to do this final check.
			is_local = true;
		else
		{
			is_local = false;
			aAlwaysUse = ALWAYS_USE_GLOBAL;  // Override aAlwaysUse for maintainability, in case there are more references to it below.
		}
	}
	else // aAlwaysUse == ALWAYS_USE_DEFAULT
	{
		is_local = g.CurrentFunc && g.CurrentFunc->mDefaultVarType != VAR_ASSUME_GLOBAL; // i.e. ASSUME_LOCAL or ASSUME_NONE
		if (mFuncExceptionVar) // Caller has ensured that this non-NULL if and only if g.CurrentFunc is non-NULL.
		{
			int i;
			for (i = 0; i < mFuncExceptionVarCount; ++i)
			{
				if (!stricmp(var_name, mFuncExceptionVar[i]->mName)) // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
				{
					is_local = !is_local;  // Since it's an exception, it's always the opposite of what it would have been.
					found_var = mFuncExceptionVar[i];
					break;
				}
			}
			// The following section is necessary because a function's parameters are not put into the
			// exception list during load-time.  Thus, for an VAR_ASSUME_GLOBAL function, these are basically
			// treated as exceptions too.
			// If this function is one that assumes variables are global, the function's parameters are
			// implicitly declared local because parameters are always local:
			// Since the following is inside this block, it is checked only at loadtime.  It doesn't need
			// to be checked at runtime because most things that resolve input variables or variables whose
			// contents will be read (as compared to something that tries to create a dynamic variable, such
			// as ResolveVarOfArg() for an output variable) at runtime use the ALWAYS_PREFER_LOCAL flag to
			// indicate that a local of the same name as a global should take precedence.  This adds more
			// flexibility/benefit than its costs in terms of confusion because otherwise there would be
			// no way to dynamically reference the local variables of an assume-global function.
			if (g.CurrentFunc->mDefaultVarType == VAR_ASSUME_GLOBAL && !is_local) // g.CurrentFunc is also known to be non-NULL in this case.
			{
				for (i = 0; i < g.CurrentFunc->mParamCount; ++i)
					if (!stricmp(var_name, g.CurrentFunc->mParam[i].var->mName)) // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
					{
						is_local = true;
						found_var = g.CurrentFunc->mParam[i].var;
						break;
					}
			}
		} // if (there is an exception list)
	} // aAlwaysUse == ALWAYS_USE_DEFAULT

	// Above has ensured that g.CurrentFunc!=NULL whenever is_local==true.

	if (apIsLocal) // Its purpose is to inform caller of type it would have been in case we don't find a match.
		*apIsLocal = is_local; // And it stays this way even if globals will be searched because caller wants that.  In other words, a local var is created by default when there is not existing global or local.
	if (apInsertPos) // Set default.  Caller should ignore the value when match is found.
		*apInsertPos = -1;
	if (apIsException)
		*apIsException = (found_var != NULL);

	if (found_var) // Match found (as an exception or load-time "is parameter" exception).
		return found_var; // apInsertPos does not need to be set because caller doesn't need it when match is found.

	// Init for binary search loop:
	int left, right, mid, result;  // left/right must be ints to allow them to go negative and detect underflow.
	Var **var;  // An array of pointers-to-var.
	if (is_local)
	{
		var = g.CurrentFunc->mVar;
		right = g.CurrentFunc->mVarCount - 1;
	}
	else
	{
		var = mVar;
		right = mVarCount - 1;
	}

	// Binary search:
	for (left = 0; left <= right;) // "right" was already initialized above.
	{
		mid = (left + right) / 2;
		result = stricmp(var_name, var[mid]->mName); // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
		if (result > 0)
			left = mid + 1;
		else if (result < 0)
			right = mid - 1;
		else // Match found.
			return var[mid];
	}

	// Since above didn't return, no match was found in the main list, so search the lazy list if there
	// is one.  If there's no lazy list, the value of "left" established above will be used as the
	// insertion point further below:
	if (is_local)
	{
		var = g.CurrentFunc->mLazyVar;
		right = g.CurrentFunc->mLazyVarCount - 1;
	}
	else
	{
		var = mLazyVar;
		right = mLazyVarCount - 1;
	}

	if (var) // There is a lazy list to search (and even if the list is empty, left must be reset to 0 below).
	{
		// Binary search:
		for (left = 0; left <= right;)  // "right" was already initialized above.
		{
			mid = (left + right) / 2;
			result = stricmp(var_name, var[mid]->mName); // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
			if (result > 0)
				left = mid + 1;
			else if (result < 0)
				right = mid - 1;
			else // Match found.
				return var[mid];
		}
	}

	// Since above didn't return, no match was found and "left" always contains the position where aVarName
	// should be inserted to keep the list sorted.  The item is always inserted into the lazy list unless
	// there is no lazy list.
	// Set the output parameter, if present:
	if (apInsertPos) // Caller wants this value even if we'll be resorting to searching the global list below.
		*apInsertPos = left; // This is the index a newly inserted item should have to keep alphabetical order.

	// Since no match was found, if this is a local fall back to searching the list of globals at runtime
	// if the caller didn't insist on a particular type:
	if (is_local)
	{
		if (aAlwaysUse == ALWAYS_PREFER_LOCAL)
		{
			// In this case, callers want to fall back to globals when a local wasn't found.  However,
			// they want the insertion (if our caller will be doing one) to insert according to the
			// current assume-mode.  Therefore, if the mode is VAR_ASSUME_GLOBAL, pass the apIsLocal
			// and apInsertPos variables to FindVar() so that it will update them to be global.
			// Otherwise, do not pass them since they were already set correctly by us above.
			if (g.CurrentFunc->mDefaultVarType == VAR_ASSUME_GLOBAL)
				return FindVar(aVarName, aVarNameLength, apInsertPos, ALWAYS_USE_GLOBAL, NULL, apIsLocal);
			else
				return FindVar(aVarName, aVarNameLength, NULL, ALWAYS_USE_GLOBAL);
		}
		if (aAlwaysUse == ALWAYS_USE_DEFAULT && mIsReadyToExecute) // In this case, fall back to globals only at runtime.
			return FindVar(aVarName, aVarNameLength, NULL, ALWAYS_USE_GLOBAL);
	}
	// Otherwise, since above didn't return:
	return NULL; // No match.
}



Var *Script::AddVar(char *aVarName, size_t aVarNameLength, int aInsertPos, int aIsLocal)
// Returns the address of the new variable or NULL on failure.
// Caller must ensure that g.CurrentFunc!=NULL whenever aIsLocal==true.
// Caller must ensure that aVarName isn't NULL and that this isn't a duplicate variable name.
// In addition, it has provided aInsertPos, which is the insertion point so that the list stays sorted.
// Finally, aIsLocal has been provided to indicate which list, global or local, should receive this
// new variable.  aIsLocal is normally 0 or 1 (boolean), but it may be 2 to indicate "it's a local AND a
// function's parameter".
{
	if (!*aVarName) // Should never happen, so just silently indicate failure.
		return NULL;
	if (!aVarNameLength) // Caller didn't specify, so use the entire string.
		aVarNameLength = strlen(aVarName);

	if (aVarNameLength > MAX_VAR_NAME_LENGTH)
	{
		// Load-time callers should check for this.  But at runtime, it's possible for a dynamically
		// resolved variable name to be too long.  Note that aVarName should be the exact variable
		// name and does not need to be truncated to aVarNameLength whenever this error occurs
		// (i.e. at runtime):
		if (mIsReadyToExecute) // Runtime error.
			ScriptError("Variable name too long." ERR_ABORT, aVarName);
		else
			ScriptError("Variable name too long.", aVarName);
		return NULL;
	}

	// Make a temporary copy that includes only the first aVarNameLength characters from aVarName:
	char var_name[MAX_VAR_NAME_LENGTH + 1];
	strlcpy(var_name, aVarName, aVarNameLength + 1);  // See explanation above.  +1 to convert length to size.

	if (!Var::ValidateName(var_name, mIsReadyToExecute))
		// Above already displayed error for us.  This can happen at loadtime or runtime (e.g. StringSplit).
		return NULL;

	// Not necessary or desirable to add built-in variables to a function's list of locals.  Always keep
	// built-in vars in the global list for efficiency and to keep them out of ListVars.  Note that another
	// section at loadtime displays an error for any attempt to explicitly declare built-in variables as
	// either global or local.
	void *var_type = GetVarType(var_name);
	if (aIsLocal && (var_type != (void *)VAR_NORMAL || !stricmp(var_name, "ErrorLevel"))) // Attempt to create built-in variable as local.
	{
		if (aIsLocal == 1) // It's not a UDF's parameter, so fall back to the global built-in variable of this name rather than displaying an error.
			return FindOrAddVar(var_name, aVarNameLength, ALWAYS_USE_GLOBAL); // Force find-or-create of global.
		else // aIsLocal == 2, which means "this is a local variable and a function's parameter".
		{
			ScriptError("Illegal parameter name.", aVarName); // Short message since so rare.
			return NULL;
		}
	}

	// Allocate some dynamic memory to pass to the constructor:
	char *new_name = SimpleHeap::Malloc(var_name, aVarNameLength);
	if (!new_name)
		// It already displayed the error for us.  These mem errors are so unusual that we're not going
		// to bother varying the error message to include ERR_ABORT if this occurs during runtime.
		return NULL;

	Var *the_new_var = new Var(new_name, var_type, aIsLocal != 0); // , aAttrib);
	if (the_new_var == NULL)
	{
		ScriptError(ERR_OUTOFMEM);
		return NULL;
	}

	// If there's a lazy var list, aInsertPos provided by the caller is for it, so this new variable
	// always gets inserted into that list because there's always room for one more (because the
	// previously added variable would have purged it if it had reached capacity).
	Var **lazy_var = aIsLocal ? g.CurrentFunc->mLazyVar : mLazyVar;
	int &lazy_var_count = aIsLocal ? g.CurrentFunc->mLazyVarCount : mLazyVarCount; // Used further below too.
	if (lazy_var)
	{
		if (aInsertPos != lazy_var_count) // Need to make room at the indicated position for this variable.
			memmove(lazy_var + aInsertPos + 1, lazy_var + aInsertPos, (lazy_var_count - aInsertPos) * sizeof(Var *));
		//else both are zero or the item is being inserted at the end of the list, so it's easy.
		lazy_var[aInsertPos] = the_new_var;
		++lazy_var_count;
		// In a testing creating between 200,000 and 400,000 variables, using a size of 1000 vs. 500 improves
		// the speed by 17%, but when you substract out the binary search time (leaving only the insert time),
		// the increase is more like 34%.  But there is a diminishing return after that: Going to 2000 only
		// gains 20%, and to 4000 only gains an addition 10%.  Therefore, to conserve memory in functions that
		// have so many variables that the lazy list is used, a good trade-off seems to be 2000 (8 KB of memory)
		// per function that needs it.
		#define MAX_LAZY_VARS 2000 // Don't make this larger than 90000 without altering the incremental increase of alloc_count further below.
		if (lazy_var_count < MAX_LAZY_VARS) // The lazy list hasn't yet reached capacity, so no need to merge it into the main list.
			return the_new_var;
	}

	// Since above didn't return, either there is no lazy list or the lazy list is full and needs to be
	// merged into the main list.

	// Create references to whichever variable list (local or global) is being acted upon.  These
	// references simplify the code:
	Var **&var = aIsLocal ? g.CurrentFunc->mVar : mVar; // This needs to be a ref. too in case it needs to be realloc'd.
	int &var_count = aIsLocal ? g.CurrentFunc->mVarCount : mVarCount;
	int &var_count_max = aIsLocal ? g.CurrentFunc->mVarCountMax : mVarCountMax;
	int alloc_count;

	// Since the above would have returned if the lazy list is present but not yet full, if the left side
	// of the OR below is false, it also means that lazy_var is NULL.  Thus lazy_var==NULL is implicit for the
	// right side of the OR:
	if ((lazy_var && var_count + MAX_LAZY_VARS > var_count_max) || var_count == var_count_max)
	{
		// Increase by orders of magnitude each time because realloc() is probably an expensive operation
		// in terms of hurting performance.  So here, a little bit of memory is sacrificed to improve
		// the expected level of performance for scripts that use hundreds of thousands of variables.
		if (!var_count_max)
			alloc_count = aIsLocal ? 100 : 1000;  // 100 conserves memory since every function needs such a block, and most functions have much fewer than 100 local variables.
		else if (var_count_max < 1000)
			alloc_count = 1000;
		else if (var_count_max < 9999) // Making this 9999 vs. 10000 allows an exact/whole number of lazy_var blocks to fit into main indices between 10000 and 99999
			alloc_count = 9999;
		else if (var_count_max < 100000)
		{
			alloc_count = 100000;
			// This is also the threshold beyond which the lazy list is used to accelerate performance.
			// Create the permanently lazy list:
			Var **&lazy_var = aIsLocal ? g.CurrentFunc->mLazyVar : mLazyVar;
			if (   !(lazy_var = (Var **)malloc(MAX_LAZY_VARS * sizeof(Var *)))   )
			{
				ScriptError(ERR_OUTOFMEM);
				return NULL;
			}
		}
		else if (var_count_max < 1000000)
			alloc_count = 1000000;
		else
			alloc_count = var_count_max + 1000000;  // i.e. continue to increase by 4MB (1M*4) each time.

		Var **temp = (Var **)realloc(var, alloc_count * sizeof(Var *)); // If passed NULL, realloc() will do a malloc().
		if (!temp)
		{
			ScriptError(ERR_OUTOFMEM);
			return NULL;
		}
		var = temp;
		var_count_max = alloc_count;
	}

	if (!lazy_var)
	{
		if (aInsertPos != var_count) // Need to make room at the indicated position for this variable.
			memmove(var + aInsertPos + 1, var + aInsertPos, (var_count - aInsertPos) * sizeof(Var *));
		//else both are zero or the item is being inserted at the end of the list, so it's easy.
		var[aInsertPos] = the_new_var;
		++var_count;
		return the_new_var;
	}
	//else the variable was already inserted into the lazy list, so the above is not done.

	// Since above didn't return, the lazy list is not only present, but full because otherwise it
	// would have returned higher above.

	// Since the lazy list is now at its max capacity, merge it into the main list (if the
	// main list was at capacity, this section relies upon the fact that the above already
	// increased its capacity by an amount far larger than the number of items containined
	// in the lazy list).

	// LAZY LIST: Although it's not nearly as good as hashing (which might be implemented in the future,
	// though it would be no small undertaking since it affects so many design aspects, both load-time
	// and runtime for scripts), this method of accelerating inserts into a binary search array is
	// enormously beneficial because it improves the scalability of binary-search by two orders
	// of magnitude (from about 100,000 variables to at least 5M).  Credit for the idea goes to Lazlo.
	// DETAILS:
	// The fact that this merge operation is so much faster than total work required
	// to insert each one into the main list is the whole reason for having the lazy
	// list.  In other words, the large memmove() that would otherwise be required
	// to insert each new variable into the main list is completely avoided.  Large memmove()s
	// are far more costly than small ones because apparently they can't fit into the CPU
	// cache, so the operation would take hundreds or even thousands of times longer
	// depending on the speed difference between main memory and CPU cache.  But above and
	// beyond the CPU cache issue, the lazy sorting method results in vastly less memory
	// being moved than would have been required without it, so even if the CPU doesn't have
	// a cache, the lazy list method vastly increases performance for scripts that have more
	// than 100,000 variables, allowing at least 5 million variables to be created without a
	// dramatic reduction in performance.

	char *target_name;
	Var **insert_pos, **insert_pos_prev;
	int i, left, right, mid;

	// Append any items from the lazy list to the main list that are alphabetically greater than
	// the last item in the main list.  Above has already ensured that the main list is large enough
	// to accept all items in the lazy list.
	for (i = lazy_var_count - 1, target_name = var[var_count - 1]->mName
		; i > -1 && stricmp(target_name, lazy_var[i]->mName) < 0
		; --i);
	// Above is a self-contained loop.
	// Now do a separate loop to append (in the *correct* order) anything found above.
	for (int j = i + 1; j < lazy_var_count; ++j) // Might have zero iterations.
		var[var_count++] = lazy_var[j];
	lazy_var_count = i + 1; // The number of items that remain after moving out those that qualified.

	// This will have zero iterations if the above already moved them all:
	for (insert_pos = var + var_count, i = lazy_var_count - 1; i > -1; --i)
	{
		// Modified binary search that relies on the fact that caller has ensured a match will never
		// be found in the main list for each item in the lazy list:
		for (target_name = lazy_var[i]->mName, left = 0, right = (int)(insert_pos - var - 1); left <= right;)
		{
			mid = (left + right) / 2;
			if (stricmp(target_name, var[mid]->mName) > 0) // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
				left = mid + 1;
			else // it must be < 0 because caller has ensured it can't be equal (i.e. that there will be no match)
				right = mid - 1;
		}
		// Now "left" contains the insertion point is is known to be less than var_count due to a previous
		// set of loops above.  Make a gap there large enough to hold all items because that allows a
		// smaller total amount of memory to be moved by shifting the gap to the left in the main list,
		// gradually filling it as we go:
		insert_pos_prev = insert_pos;  // "prev" is the now the position of the beginning of the gap, but the gap is about to be shifted left by moving memory right.
		insert_pos = var + left; // This is where it *would* be inserted if we weren't doing the accelerated merge.
		memmove(insert_pos + i + 1, insert_pos, (insert_pos_prev - insert_pos) * sizeof(Var *));
		var[left + i] = lazy_var[i]; // Now insert this item at the far right side of the gap just created.
	}
	var_count += lazy_var_count;
	lazy_var_count = 0;  // Indicate that the lazy var list is now empty.

	return the_new_var;
}



void *Script::GetVarType(char *aVarName)
{
	// Convert to lowercase to help performance a little (it typically only helps loadtime performance because
	// this function is rarely called during script-runtime).
	char lowercase[MAX_VAR_NAME_LENGTH + 1];
	strlcpy(lowercase, aVarName, sizeof(lowercase)); // Caller should have ensured it fits, but call strlcpy() for maintainability.
	CharLower(lowercase);
	// Above: CharLower() is smaller in code size than strlwr(), but CharLower uses the OS locale and strlwr uses
	// the setlocal() locale (which is always the same if setlocal() is never called).  However, locale
	// differences shouldn't affect the cases checked below; some evidence of this is at MSDN:
	// "CharLower always maps uppercase I to lowercase I, even when the current language is Turkish or Azeri."

	if (lowercase[0] != 'a' || lowercase[1] != '_')  // This check helps average-case performance.
	{
		if (   !strcmp(lowercase, "true")
			|| !strcmp(lowercase, "false")) return (void *)BIV_True_False;
		if (!strcmp(lowercase, "clipboard")) return (void *)VAR_CLIPBOARD;
		if (!strcmp(lowercase, "clipboardall")) return (void *)VAR_CLIPBOARDALL;
		if (!strcmp(lowercase, "comspec")) return (void *)BIV_ComSpec; // Lacks an "A_" prefix for backward compatibility with pre-NoEnv scripts and also it's easier to type & remember.
		if (!strcmp(lowercase, "programfiles")) return (void *)BIV_ProgramFiles; // v1.0.43.08: Added to ease the transition to #NoEnv.
		// Otherwise:
		return (void *)VAR_NORMAL;
	}

	// Otherwise, lowercase begins with "a_", so it's probably one of the built-in variables.
	char *lower = lowercase + 2;

	// Keeping the most common ones near the top helps performance a little.
	if (!strcmp(lower, "index")) return (void *)BIV_LoopIndex;  // A short name since it's typed so often.

	if (   !strcmp(lower, "mmmm")    // Long name of month.
		|| !strcmp(lower, "mmm")     // 3-char abbrev. month name.
		|| !strcmp(lower, "dddd")    // Name of weekday, e.g. Sunday
		|| !strcmp(lower, "ddd")   ) // Abbrev., e.g. Sun
		return (void *)BIV_MMM_DDD;

	if (   !strcmp(lower, "yyyy")
		|| !strcmp(lower, "year") // Same as above.
		|| !strcmp(lower, "mm")   // 01 thru 12
		|| !strcmp(lower, "mon")  // Same
		|| !strcmp(lower, "dd")   // 01 thru 31
		|| !strcmp(lower, "mday") // Same
		|| !strcmp(lower, "wday")
		|| !strcmp(lower, "yday")
		|| !strcmp(lower, "yweek")
		|| !strcmp(lower, "hour")
		|| !strcmp(lower, "min")
		|| !strcmp(lower, "sec")
		|| !strcmp(lower, "msec")   )
		return (void *)BIV_DateTime;

	if (!strcmp(lower, "tickcount")) return (void *)BIV_TickCount;
	if (   !strcmp(lower, "now")
		|| !strcmp(lower, "nowutc")) return (void *)BIV_Now;

	if (!strcmp(lower, "workingdir")) return (void *)BIV_WorkingDir;
	if (!strcmp(lower, "scriptname")) return (void *)BIV_ScriptName;
	if (!strcmp(lower, "scriptdir")) return (void *)BIV_ScriptDir;
	if (!strcmp(lower, "scriptfullpath")) return (void *)BIV_ScriptFullPath;
	if (!strcmp(lower, "linenumber")) return (void *)BIV_LineNumber;
	if (!strcmp(lower, "linefile")) return (void *)BIV_LineFile;

// A_IsCompiled is left blank/undefined in uncompiled scripts.
#ifdef AUTOHOTKEYSC
	if (!strcmp(lower, "iscompiled")) return (void *)BIV_IsCompiled;
#endif

	if (   !strcmp(lower, "batchlines")
		|| !strcmp(lower, "numbatchlines")) return (void *)BIV_BatchLines;
	if (!strcmp(lower, "titlematchmode")) return (void *)BIV_TitleMatchMode;
	if (!strcmp(lower, "titlematchmodespeed")) return (void *)BIV_TitleMatchModeSpeed;
	if (!strcmp(lower, "detecthiddenwindows")) return (void *)BIV_DetectHiddenWindows;
	if (!strcmp(lower, "detecthiddentext")) return (void *)BIV_DetectHiddenText;
	if (!strcmp(lower, "autotrim")) return (void *)BIV_AutoTrim;
	if (!strcmp(lower, "stringcasesense")) return (void *)BIV_StringCaseSense;
	if (!strcmp(lower, "formatinteger")) return (void *)BIV_FormatInteger;
	if (!strcmp(lower, "formatfloat")) return (void *)BIV_FormatFloat;
	if (!strcmp(lower, "keydelay")) return (void *)BIV_KeyDelay;
	if (!strcmp(lower, "windelay")) return (void *)BIV_WinDelay;
	if (!strcmp(lower, "controldelay")) return (void *)BIV_ControlDelay;
	if (!strcmp(lower, "mousedelay")) return (void *)BIV_MouseDelay;
	if (!strcmp(lower, "defaultmousespeed")) return (void *)BIV_DefaultMouseSpeed;
	if (!strcmp(lower, "issuspended")) return (void *)BIV_IsSuspended;

	if (!strcmp(lower, "iconhidden")) return (void *)BIV_IconHidden;
	if (!strcmp(lower, "icontip")) return (void *)BIV_IconTip;
	if (!strcmp(lower, "iconfile")) return (void *)BIV_IconFile;
	if (!strcmp(lower, "iconnumber")) return (void *)BIV_IconNumber;

	if (!strcmp(lower, "exitreason")) return (void *)BIV_ExitReason;

	if (!strcmp(lower, "ostype")) return (void *)BIV_OSType;
	if (!strcmp(lower, "osversion")) return (void *)BIV_OSVersion;
	if (!strcmp(lower, "language")) return (void *)BIV_Language;
	if (   !strcmp(lower, "computername")
		|| !strcmp(lower, "username")) return (void *)BIV_UserName_ComputerName;

	if (!strcmp(lower, "windir")) return (void *)BIV_WinDir;
	if (!strcmp(lower, "temp")) return (void *)BIV_Temp; // Debatably should be A_TempDir, but brevity seemed more popular with users, perhaps for heavy uses of the temp folder.
	if (!strcmp(lower, "programfiles")) return (void *)BIV_ProgramFiles;
	if (!strcmp(lower, "mydocuments")) return (void *)BIV_MyDocuments;

	if (   !strcmp(lower, "appdata")
		|| !strcmp(lower, "appdatacommon")) return (void *)BIV_AppData;
	if (   !strcmp(lower, "desktop")
		|| !strcmp(lower, "desktopcommon")) return (void *)BIV_Desktop;
	if (   !strcmp(lower, "startmenu")
		|| !strcmp(lower, "startmenucommon")) return (void *)BIV_StartMenu;
	if (   !strcmp(lower, "programs")
		|| !strcmp(lower, "programscommon")) return (void *)BIV_Programs;
	if (   !strcmp(lower, "startup")
		|| !strcmp(lower, "startupcommon")) return (void *)BIV_Startup;

	if (!strcmp(lower, "isadmin")) return (void *)BIV_IsAdmin;
	if (!strcmp(lower, "cursor")) return (void *)BIV_Cursor;
	if (   !strcmp(lower, "caretx")
		|| !strcmp(lower, "carety")) return (void *)BIV_Caret;
	if (   !strcmp(lower, "screenwidth")
		|| !strcmp(lower, "screenheight")) return (void *)BIV_ScreenWidth_Height;

	if (!strncmp(lower, "ipaddress", 9))
	{
		lower += 9;
		return (void *)(*lower >= '1' && *lower <= '4'
			&& !lower[1]) // Make sure has only one more character rather than none or several (e.g. A_IPAddress1abc should not be match).
			? (void *)BIV_IPAddress
			: (void *)VAR_NORMAL; // Otherwise it can't be a match for any built-in variable.
	}

	if (!strncmp(lower, "loop", 4))
	{
		lower += 4;
		if (!strcmp(lower, "readline")) return (void *)BIV_LoopReadLine;
		if (!strcmp(lower, "field")) return (void *)BIV_LoopField;

		if (!strncmp(lower, "file", 4))
		{
			lower += 4;
			if (!strcmp(lower, "name")) return (void *)BIV_LoopFileName;
			if (!strcmp(lower, "shortname")) return (void *)BIV_LoopFileShortName;
			if (!strcmp(lower, "ext")) return (void *)BIV_LoopFileExt;
			if (!strcmp(lower, "dir")) return (void *)BIV_LoopFileDir;
			if (!strcmp(lower, "fullpath")) return (void *)BIV_LoopFileFullPath;
			if (!strcmp(lower, "longpath")) return (void *)BIV_LoopFileLongPath;
			if (!strcmp(lower, "shortpath")) return (void *)BIV_LoopFileShortPath;
			if (!strcmp(lower, "attrib")) return (void *)BIV_LoopFileAttrib;

			if (   !strcmp(lower, "timemodified")
				|| !strcmp(lower, "timecreated")
				|| !strcmp(lower, "timeaccessed")) return (void *)BIV_LoopFileTime;
			if (   !strcmp(lower, "size")
				|| !strcmp(lower, "sizekb")
				|| !strcmp(lower, "sizemb")) return (void *)BIV_LoopFileSize;
			// Otherwise, it can't be a match for any built-in variable:
			return (void *)VAR_NORMAL;
		}

		if (!strncmp(lower, "reg", 3))
		{
			lower += 3;
			if (!strcmp(lower, "type")) return (void *)BIV_LoopRegType;
			if (!strcmp(lower, "key")) return (void *)BIV_LoopRegKey;
			if (!strcmp(lower, "subkey")) return (void *)BIV_LoopRegSubKey;
			if (!strcmp(lower, "name")) return (void *)BIV_LoopRegName;
			if (!strcmp(lower, "timemodified")) return (void *)BIV_LoopRegTimeModified;
			// Otherwise, it can't be a match for any built-in variable:
			return (void *)VAR_NORMAL;
		}
	}

	if (!strcmp(lower, "thisfunc")) return (void *)BIV_ThisFunc;
	if (!strcmp(lower, "thislabel")) return (void *)BIV_ThisLabel;
	if (!strcmp(lower, "thismenuitem")) return (void *)BIV_ThisMenuItem;
	if (!strcmp(lower, "thismenuitempos")) return (void *)BIV_ThisMenuItemPos;
	if (!strcmp(lower, "thismenu")) return (void *)BIV_ThisMenu;
	if (!strcmp(lower, "thishotkey")) return (void *)BIV_ThisHotkey;
	if (!strcmp(lower, "priorhotkey")) return (void *)BIV_PriorHotkey;
	if (!strcmp(lower, "timesincethishotkey")) return (void *)BIV_TimeSinceThisHotkey;
	if (!strcmp(lower, "timesincepriorhotkey")) return (void *)BIV_TimeSincePriorHotkey;
	if (!strcmp(lower, "endchar")) return (void *)BIV_EndChar;
	if (!strcmp(lower, "lasterror")) return (void *)BIV_LastError;

	if (!strcmp(lower, "eventinfo")) return (void *)BIV_EventInfo; // It's called "EventInfo" vs. "GuiEventInfo" because it applies to non-Gui events such as OnClipboardChange.
	if (!strcmp(lower, "guicontrol")) return (void *)BIV_GuiControl;

	if (   !strcmp(lower, "guicontrolevent") // v1.0.36: A_GuiEvent was added as a synonym for A_GuiControlEvent because it seems unlikely that A_GuiEvent will ever be needed for anything:
		|| !strcmp(lower, "guievent")) return (void *)BIV_GuiEvent;

	if (   !strcmp(lower, "gui")
		|| !strcmp(lower, "guiwidth")
		|| !strcmp(lower, "guiheight")
		|| !strcmp(lower, "guix") // Naming: Brevity seems more a benefit than would A_GuiEventX's improved clarity.
		|| !strcmp(lower, "guiy")) return (void *)BIV_Gui; // These can be overloaded if a GuiMove label or similar is ever needed.

	if (!strcmp(lower, "timeidle")) return (void *)BIV_TimeIdle;
	if (!strcmp(lower, "timeidlephysical")) return (void *)BIV_TimeIdlePhysical;
	if (   !strcmp(lower, "space")
		|| !strcmp(lower, "tab")) return (void *)BIV_Space_Tab;
	if (!strcmp(lower, "ahkversion")) return (void *)BIV_AhkVersion;
	if (!strcmp(lower, "ahkpath")) return (void *)BIV_AhkPath;

	// Since above didn't return:
	return (void *)VAR_NORMAL;
}



WinGroup *Script::FindGroup(char *aGroupName, bool aCreateIfNotFound)
// Caller must ensure that aGroupName isn't NULL.  But if it's the empty string, NULL is returned.
// Returns the Group whose name matches aGroupName.  If it doesn't exist, it is created if aCreateIfNotFound==true.
// Thread-safety: This function is thread-safe (except when when called with aCreateIfNotFound==true) even when
// the main thread happens to be calling AddGroup() and changing the linked list while it's being traversed here
// by the hook thread.  However, any subsequent changes to this function or AddGroup() must be carefully reviewed.
{
	if (!*aGroupName)
		return NULL;
	for (WinGroup *group = mFirstGroup; group != NULL; group = group->mNextGroup)
		if (!stricmp(group->mName, aGroupName)) // lstrcmpi() is not used: 1) avoids breaking exisitng scripts; 2) provides consistent behavior across multiple locales; 3) performance.
			return group; // Match found.
	// Otherwise, no match found, so create a new group.
	if (!aCreateIfNotFound || AddGroup(aGroupName) != OK)
		return NULL;
	return mLastGroup;
}



ResultType Script::AddGroup(char *aGroupName)
// Returns OK or FAIL.
// The caller must already have verfied that this isn't a duplicate group.
// This function is not thread-safe because it adds an entry to the quasi-global list of window groups.
// In addition, if this function is being called by one thread while another thread is calling FindGroup(),
// the thread-safety notes in FindGroup() apply.
{
	size_t aGroupName_length = strlen(aGroupName);
	if (aGroupName_length > MAX_VAR_NAME_LENGTH)
		return ScriptError("Group name too long.", aGroupName);
	if (!Var::ValidateName(aGroupName, false, DISPLAY_NO_ERROR)) // Seems best to use same validation as var names.
		return ScriptError("Illegal group name.", aGroupName);

	char *new_name = SimpleHeap::Malloc(aGroupName, aGroupName_length);
	if (!new_name)
		return FAIL;  // It already displayed the error for us.

	// The precise method by which the follows steps are done should be thread-safe even if
	// some other thread calls FindGroup() in the middle of the operation.  But any changes
	// must be carefully reviewed:
	WinGroup *the_new_group = new WinGroup(new_name);
	if (the_new_group == NULL)
		return ScriptError(ERR_OUTOFMEM);
	if (mFirstGroup == NULL)
		mFirstGroup = the_new_group;
	else
		mLastGroup->mNextGroup = the_new_group;
	// This must be done after the above:
	mLastGroup = the_new_group;
	return OK;
}



Line *Script::PreparseBlocks(Line *aStartingLine, bool aFindBlockEnd, Line *aParentLine)
// aFindBlockEnd should be true, only when this function is called
// by itself.  The end of this function relies upon this definition.
// Will return NULL to the top-level caller if there's an error, or if
// mLastLine is NULL (i.e. the script is empty).
{
	// Not thread-safe, so this can only parse one script at a time.
	// Not a problem for the foreseeable future:
	static int nest_level; // Level zero is the outermost one: outside all blocks.
	static bool abort;
	if (!aParentLine)
	{
		// We were called from outside, not recursively, so init these.  This is
		// very important if this function is ever to be called from outside
		// more than once, even though it isn't currently:
		nest_level = 0;
		abort = false;
	}

	int i, open_parens;
	bool in_quotes;
	DerefType *deref, *deref2;
	char *param_start, *param_end, *param_last_char, *cp, c;
	bool found;

	// Don't check aStartingLine here at top: only do it at the bottom
	// for its differing return values.
	for (Line *line = aStartingLine; line;)
	{
		// Check if any of each arg's derefs are function calls.  If so, do some validation and
		// preprocessing to set things up for better runtime performance:
		for (i = 0; i < line->mArgc; ++i) // For each arg.
		{
			ArgStruct &this_arg = line->mArg[i]; // For performance and convenience.
			// Exclude the derefs of output and input vars from consideration, since they can't
			// be function calls:
			if (!this_arg.is_expression // For now, only expressions are capable of calling functions. If ever change this, might want to add a check here for this_arg.type != ARG_TYPE_NORMAL (for performance).
				|| !this_arg.deref) // No function-calls present.
				continue;
			for (deref = this_arg.deref; deref->marker; ++deref) // For each deref.
			{
				if (!deref->is_function)
					continue;
				if (   !(deref->func = FindFunc(deref->marker, deref->length))   )
				{
#ifndef AUTOHOTKEYSC
					bool error_was_shown;
					if (   !(deref->func = FindFuncInLibrary(deref->marker, deref->length, error_was_shown))   )
					{
						abort = true; // So that the caller doesn't also report an error.
						// When above already displayed the proximate cause of the error, it's usually
						// undesirable to show the cascade effects of that error in a second dialog:
						return error_was_shown ? NULL : line->PreparseError(ERR_NONEXISTENT_FUNCTION, deref->marker);
					}
#else
					abort = true;
					return line->PreparseError(ERR_NONEXISTENT_FUNCTION, deref->marker);
#endif
				}
				// An earlier stage has ensured that if the function exists, it's mJumpToLine is non-NULL.
				Func &func = *deref->func; // For performance and convenience.
				// Ealier stage has ensured that strchr() will always find an open-parenthesis:
				for (deref->param_count = 0, param_start = omit_leading_whitespace(strchr(deref->marker, '(') + 1);;)
				{
					// For each parameter of this function-call.
					if (*param_start == ')') // No more params.
						break;
					if (*param_start == ',')
					{
						abort = true; // So that the caller doesn't also report an error.
						return line->PreparseError(ERR_BLANK_PARAM, deref->marker);
					}
					// Although problems such as blank/empty parameters and missing close-paren were already
					// checked by DefineFunc(), that was done only for the function's formal definition, not
					// the calls to it.  And although parentheses were balanced in all expressions at an earlier
					// stage, it's done again here in case function calls are ever allowed to be occur in
					// a non-expression (or dynamic functions calls such as FnArray%i%() are ever supported):
					if (!*param_start)
					{
						abort = true; // So that the caller doesn't also report an error.
						return line->PreparseError(ERR_MISSING_CLOSE_PAREN, deref->marker);
					}

					// Find the end of this function-param by taking into account nested parentheses, omitting
					// from consideration any parentheses inside of quoted/literal strings.  When this loop is done,
					// param_end this param's final comma or this function-call's close-paren when this param
					// is the last one.
					for (in_quotes = false, open_parens = 0, param_end = param_start;; ++param_end)
					{
						// If nested function calls are encountered within the function call being examined
						// now, they are skipped over because they will be processed here only when the outer
						// loop gets to them.
						c = *param_end; // switch() is not used so that "break" can be used to exit the loop.
						if (c == ',')
						{
							if (!(in_quotes || open_parens)) // This comma belongs to our function, so it marks the end of this param.
								break;
							//else it's not a real comma since it's inside the parentheses of a subexpression or
							// sub-function, or inside a quoted/literal string.  Ignore it.
						}
						else if (c == ')')
						{
							if (!in_quotes)
							{
								if (!open_parens) // This is our function's close-paren, and thus the end of this param.
									break;
								else
									--open_parens;
							}
							//else it's not a real paren since it's inside a quoted/literal string.  Ignore it.
						}
						else if (c == '(')
						{
							if (!in_quotes) // Literal parentheses inside a quoted string should not be counted for this purpose.
								++open_parens;
						}
						else if (c == '"')
							// The simple method below is sufficient for our purpose even if a quoted string contains
							// pairs of double-quotes to represent a single literal quote, e.g. "quoted ""word""".
							// In other words, it relies on the fact that there must be an even number of quotes
							// inside any mandatory-numeric arg that is an expression such as x=="red,blue"
							in_quotes = !in_quotes;
						else if (!c) // This function lacks a closing paren.
						{
							// Might happen if this is a syntax error not catchable by the earlier stage of syntax
							// checking (paren balancing, quote balancing, etc.)
							abort = true; // So that the caller doesn't also report an error.
							return line->PreparseError(ERR_MISSING_CLOSE_PAREN, deref->marker);
						}
						//else it's some other, non-special character, so ignore it.
					} // for() that finds the end of this param of this function.

					// Above would have returned unless *param_end is either a comma or close-paren (namely the
					// one that terminates this parameter of this function).

					if (deref->param_count >= func.mParamCount) // Check this every iteration to avoid going beyond MAX_FUNCTION_PARAMS.
					{
						abort = true; // So that the caller doesn't also report an error.
						return line->PreparseError("Too many parameters passed to function.", deref->marker);
					}
					// Below relies on the above check having been done first to avoid reading beyond the
					// end of the mParam array.
					// If this parameter is formally declared as ByRef, report a load-time error if
					// the actual-parameter is obviously not a variable (can't catch everything, such
					// as invalid double derefs, e.g. Array%VarContainingSpaces%):
					if (!func.mIsBuiltIn && func.mParam[deref->param_count].is_byref)
					{
						// First check if there are any EXPR_TELLTALES characters in this param, since the
						// presence of an expression for this parameter means it can't resolve to a variable
						// as required by ByRef:
						for (cp = param_start, param_last_char = omit_trailing_whitespace(param_start, param_end - 1)
							; cp <= param_last_char; ++cp)
						{
							if (*cp == ':' && cp[1] == '=')
								// v1.0.46.05: This section fixes the inability to pass ByRef certain non-trivial
								// assignments like X := " ". Although this doesn't give 100% detection, something
								// more elaborate seems unjustified (in both code size and performance) given that
								// this is only a syntax check.
								break;
							if (strchr(EXPR_FORBIDDEN_BYREF, *cp)) // This character isn't allowed in something passed ByRef unless it's an assignment (which is checked below).
							{
								if (Line::StartsWithAssignmentOp(cp) || strstr(cp, " ? ")) // v1.0.46.09: Also allow a ternary unconditionally, because it can be an arbitrarily complex expression followed by two branches that yield variables.
								{
									// Skip over :=, +=, -=, *=, /=, ++, -- ... because they can be passed ByRef.
									// In fact, don't even continue the loop because any assignment can be followed
									// by an arbitrarily complex sub-expression that shouldn't disqualify ByRef.
									break;
								}
								abort = true; // So that the caller doesn't also report an error.
								return line->PreparseError(ERR_BYREF, param_start);   // param_start seems more informative than func.mParam[deref->param_count].var->mName
							}
						}
						// Below relies on the above having been done because the above should prevent
						// any is_function derefs from being possible since their parentheses would have been caught
						// as an error:
						// For each deref after the function name itself, ensure that there is at least
						// one deref in between this param's param_start and param_end.  This finds many
						// common syntax errors such as passing a literal number or string to a ByRef
						// parameter.  Note that there can be more than one for something like Array%i%_%j%
						// or a ternary like true ? x : y.
						for (found = false, deref2 = deref + 1; deref2->marker; ++deref2)
							if (deref2->marker >= param_start && deref2->marker < param_end)
							{
								found = true;
								break;
							}
						if (!found)
						{
							abort = true; // So that the caller doesn't also report an error.
							return line->PreparseError(ERR_BYREF, param_start); // param_start seems more informative than func.mParam[deref->param_count].var->mName
						}
					}

					++deref->param_count;

					// Set up for the next iteration:
					param_start = param_end; // Must already be a comma or close-paren due to checking higher above.
					if (*param_start == ',')
					{
						param_start = omit_leading_whitespace(param_start + 1);
						if (*param_start == ')')
						{
							abort = true; // So that the caller doesn't also report an error.
							return line->PreparseError(ERR_BLANK_PARAM, param_start); // Report param_start vs. aBuf to give an idea of where the blank parameter is in a possibly long list of params.
						}
					}
					//else it might be ')', in which case the next iteration will handle it.
					// Above has ensured that param_start now points to the next parameter, or ')' if none.
				} // for each parameter of this function call.
				if (deref->param_count < func.mMinParams)
				{
					abort = true; // So that the caller doesn't also report an error.
					return line->PreparseError("Too few parameters passed to function.", deref->marker);
				}
			} // for each deref of this arg
		} // for each arg of this line

		// All lines in our recursion layer are assigned to the block that the caller specified:
		if (line->mParentLine == NULL) // i.e. don't do it if it's already "owned" by an IF or ELSE.
			line->mParentLine = aParentLine; // Can be NULL.

		if (ACT_IS_IF_OR_ELSE_OR_LOOP(line->mActionType) || line->mActionType == ACT_REPEAT)
		{
			// Make the line immediately following each ELSE, IF or LOOP be enclosed by that stmt.
			// This is done to make it illegal for a Goto or Gosub to jump into a deeper layer,
			// such as in this example:
			// #y::
			// ifwinexist, pad
			// {
			//    goto, label1
			//    ifwinexist, pad
			//    label1:
			//    ; With or without the enclosing block, the goto would still go to an illegal place
			//    ; in the below, resulting in an "unexpected else" error:
			//    {
			//	     msgbox, ifaction
			//    } ; not necessary to make this line enclosed by the if because labels can't point to it?
			// else
			//    msgbox, elseaction
			// }
			// return

			// In this case, the loader should have already ensured that line->mNextLine is not NULL:
			line->mNextLine->mParentLine = line;
			// Go onto the IF's or ELSE's action in case it too is an IF, rather than skipping over it:
			line = line->mNextLine;
			continue;
		}

		switch (line->mActionType)
		{
		case ACT_BLOCK_BEGIN:
			// Some insane limit too large to ever likely be exceeded, yet small enough not
			// to be a risk of stack overflow when recursing in ExecUntil().  Mostly, this is
			// here to reduce the chance of a program crash if a binary file, a corrupted file,
			// or something unexpected has been loaded as a script when it shouldn't have been.
			// Update: Increased the limit from 100 to 1000 so that large "else if" ladders
			// can be constructed.  Going much larger than 1000 seems unwise since ExecUntil()
			// will have to recurse for each nest-level, possibly resulting in stack overflow
			// if things get too deep:
			if (nest_level > 1000)
			{
				abort = true; // So that the caller doesn't also report an error.
				return line->PreparseError("Nesting too deep."); // Short msg since so rare.
			}
			// Since the current convention is to store the line *after* the
			// BLOCK_END as the BLOCK_BEGIN's related line, that line can
			// be legitimately NULL if this block's BLOCK_END is the last
			// line in the script.  So it's up to the called function
			// to report an error if it never finds a BLOCK_END for us.
			// UPDATE: The design requires that we do it here instead:
			++nest_level;
			if (NULL == (line->mRelatedLine = PreparseBlocks(line->mNextLine, 1, line)))
				if (abort) // the above call already reported the error.
					return NULL;
				else
				{
					abort = true; // So that the caller doesn't also report an error.
					return line->PreparseError(ERR_MISSING_CLOSE_BRACE);
				}
			--nest_level;
			// The convention is to have the BLOCK_BEGIN's related_line
			// point to the line *after* the BLOCK_END.
			line->mRelatedLine = line->mRelatedLine->mNextLine;  // Might be NULL now.
			// Otherwise, since any blocks contained inside this one would already
			// have been handled by the recursion in the above call, continue searching
			// from the end of this block:
			line = line->mRelatedLine; // If NULL, the loop-condition will catch it.
			break;
		case ACT_BLOCK_END:
			// Return NULL (failure) if the end was found but we weren't looking for one
			// (i.e. it's an orphan).  Otherwise return the line after the block_end line,
			// which will become the caller's mRelatedLine.  UPDATE: Return the
			// END_BLOCK line itself so that the caller can differentiate between
			// a NULL due to end-of-script and a NULL caused by an error:
			return aFindBlockEnd ? line  // Doesn't seem necessary to set abort to true.
				: line->PreparseError(ERR_MISSING_OPEN_BRACE);
		default: // Continue line-by-line.
			line = line->mNextLine;
		} // switch()
	} // for each line

	// End of script has been reached.  <line> is now NULL so don't attempt to dereference it.
	// If we were still looking for an EndBlock to match up with a begin, that's an error.
	// Don't report the error here because we don't know which begin-block is waiting
	// for an end (the caller knows and must report the error).  UPDATE: Must report
	// the error here (see comments further above for explanation).   UPDATE #2: Changed
	// it again: Now we let the caller handle it again:
	if (aFindBlockEnd)
		//return mLastLine->PreparseError("The script ends while a block is still open (missing }).");
		return NULL;
	// If no error, return something non-NULL to indicate success to the top-level caller.
	// We know we're returning to the top-level caller because aFindBlockEnd is only true
	// when we're recursed, and in that case the above would have returned.  Thus,
	// we're not recursed upon reaching this line:
	return mLastLine;
}



Line *Script::PreparseIfElse(Line *aStartingLine, ExecUntilMode aMode, AttributeType aLoopTypeFile
	, AttributeType aLoopTypeReg, AttributeType aLoopTypeRead, AttributeType aLoopTypeParse)
// Zero is the default for aMode, otherwise:
// Will return NULL to the top-level caller if there's an error, or if
// mLastLine is NULL (i.e. the script is empty).
// Note: This function should be called with aMode == ONLY_ONE_LINE
// only when aStartingLine's ActionType is something recursable such
// as IF and BEGIN_BLOCK.  Otherwise, it won't return after only one line.
{
	// Don't check aStartingLine here at top: only do it at the bottom
	// for it's differing return values.
	Line *line_temp;
	// Although rare, a statement can be enclosed in more than one type of special loop,
	// e.g. both a file-loop and a reg-loop:
	AttributeType loop_type_file, loop_type_reg, loop_type_read, loop_type_parse;
	for (Line *line = aStartingLine; line != NULL;)
	{
		if (ACT_IS_IF(line->mActionType) || line->mActionType == ACT_LOOP || line->mActionType == ACT_REPEAT)
		{
			// ActionType is an IF or a LOOP.
			line_temp = line->mNextLine;  // line_temp is now this IF's or LOOP's action-line.
			// Update: Below is commented out because it's now impossible (since all scripts end in ACT_EXIT):
			//if (line_temp == NULL) // This is an orphan IF/LOOP (has no action-line) at the end of the script.
			//	return line->PreparseError("Q"); // Placeholder. Formerly "This if-statement or loop has no action."

			// Other things rely on this check having been done, such as "if (line->mRelatedLine != NULL)":
			if (line_temp->mActionType == ACT_ELSE || line_temp->mActionType == ACT_BLOCK_END)
				return line->PreparseError("Inappropriate line beneath IF or LOOP.");

			// We're checking for ATTR_LOOP_FILEPATTERN here to detect whether qualified commands enclosed
			// in a true file loop are allowed to omit their filename parameter:
			loop_type_file = ATTR_NONE;
			if (aLoopTypeFile == ATTR_LOOP_FILEPATTERN || line->mAttribute == ATTR_LOOP_FILEPATTERN)
				// i.e. if either one is a file-loop, that's enough to establish
				// the fact that we're in a file loop.
				loop_type_file = ATTR_LOOP_FILEPATTERN;
			else if (aLoopTypeFile == ATTR_LOOP_UNKNOWN || line->mAttribute == ATTR_LOOP_UNKNOWN)
				// ATTR_LOOP_UNKNOWN takes precedence over ATTR_LOOP_NORMAL because
				// we can't be sure if we're in a file loop, but it's correct to
				// assume that we are (otherwise, unwarranted syntax errors may be reported
				// later on in here).
				loop_type_file = ATTR_LOOP_UNKNOWN;
			else if (aLoopTypeFile == ATTR_LOOP_NORMAL || line->mAttribute == ATTR_LOOP_NORMAL)
				loop_type_file = ATTR_LOOP_NORMAL;

			// The section is the same as above except for registry vs. file loops:
			loop_type_reg = ATTR_NONE;
			if (aLoopTypeReg == ATTR_LOOP_REG || line->mAttribute == ATTR_LOOP_REG)
				loop_type_reg = ATTR_LOOP_REG;
			else if (aLoopTypeReg == ATTR_LOOP_UNKNOWN || line->mAttribute == ATTR_LOOP_UNKNOWN)
				loop_type_reg = ATTR_LOOP_UNKNOWN;
			else if (aLoopTypeReg == ATTR_LOOP_NORMAL || line->mAttribute == ATTR_LOOP_NORMAL)
				loop_type_reg = ATTR_LOOP_NORMAL;

			// Same as above except for READ-FILE loops:
			loop_type_read = ATTR_NONE;
			if (aLoopTypeRead == ATTR_LOOP_READ_FILE || line->mAttribute == ATTR_LOOP_READ_FILE)
				loop_type_read = ATTR_LOOP_READ_FILE;
			else if (aLoopTypeRead == ATTR_LOOP_UNKNOWN || line->mAttribute == ATTR_LOOP_UNKNOWN)
				loop_type_read = ATTR_LOOP_UNKNOWN;
			else if (aLoopTypeRead == ATTR_LOOP_NORMAL || line->mAttribute == ATTR_LOOP_NORMAL)
				loop_type_read = ATTR_LOOP_NORMAL;

			// Same as above except for PARSING loops:
			loop_type_parse = ATTR_NONE;
			if (aLoopTypeParse == ATTR_LOOP_PARSE || line->mAttribute == ATTR_LOOP_PARSE)
				loop_type_parse = ATTR_LOOP_PARSE;
			else if (aLoopTypeParse == ATTR_LOOP_UNKNOWN || line->mAttribute == ATTR_LOOP_UNKNOWN)
				loop_type_parse = ATTR_LOOP_UNKNOWN;
			else if (aLoopTypeParse == ATTR_LOOP_NORMAL || line->mAttribute == ATTR_LOOP_NORMAL)
				loop_type_parse = ATTR_LOOP_NORMAL;

			// Check if the IF's action-line is something we want to recurse.  UPDATE: Always
			// recurse because other line types, such as Goto and Gosub, need to be preparsed
			// by this function even if they are the single-line actions of an IF or an ELSE:
			// Recurse this line rather than the next because we want
			// the called function to recurse again if this line is a ACT_BLOCK_BEGIN
			// or is itself an IF:
			line_temp = PreparseIfElse(line_temp, ONLY_ONE_LINE, loop_type_file, loop_type_reg, loop_type_read
				, loop_type_parse);
			// If not end-of-script or error, line_temp is now either:
			// 1) If this if's/loop's action was a BEGIN_BLOCK: The line after the end of the block.
			// 2) If this if's/loop's action was another IF or LOOP:
			//    a) the line after that if's else's action; or (if it doesn't have one):
			//    b) the line after that if's/loop's action
			// 3) If this if's/loop's action was some single-line action: the line after that action.
			// In all of the above cases, line_temp is now the line where we
			// would expect to find an ELSE for this IF, if it has one.

			// Now the above has ensured that line_temp is this line's else, if it has one.
			// Note: line_temp will be NULL if the end of the script has been reached.
			// UPDATE: That can't happen now because all scripts end in ACT_EXIT:
			if (line_temp == NULL) // Error or end-of-script was reached.
				return NULL;

			// Seems best to keep this check for mainability because changes to other checks can impact
			// whether this check will ever be "true":
			if (line->mRelatedLine != NULL)
				return line->PreparseError("Q"); // Placeholder since it shouldn't happen.  Formerly "This if-statement or LOOP unexpectedly already had an ELSE or end-point."
			// Set it to the else's action, rather than the else itself, since the else itself
			// is never needed during execution.  UPDATE: No, instead set it to the ELSE itself
			// (if it has one) since we jump here at runtime when the IF is finished (whether
			// it's condition was true or false), thus skipping over any nested IF's that
			// aren't in blocks beneath it.  If there's no ELSE, the below value serves as
			// the jumppoint we go to when the if-statement is finished.  Example:
			// if x
			//   if y
			//     if z
			//       action1
			//     else
			//       action2
			// action3
			// x's jumppoint should be action3 so that all the nested if's
			// under the first one can be skipped after the "if x" line is recursively
			// evaluated.  Because of this behavior, all IFs will have a related line
			// with the possibly exception of the very last if-statement in the script
			// (which is possible only if the script doesn't end in a Return or Exit).
			line->mRelatedLine = line_temp;  // Even if <line> is a LOOP and line_temp and else?

			// Even if aMode == ONLY_ONE_LINE, an IF and its ELSE count as a single
			// statement (one line) due to its very nature (at least for this purpose),
			// so always continue on to evaluate the IF's ELSE, if present:
			if (line_temp->mActionType == ACT_ELSE)
			{
				if (line->mActionType == ACT_LOOP || line->mActionType == ACT_REPEAT)
				{
					 // this can't be our else, so let the caller handle it.
					if (aMode != ONLY_ONE_LINE)
						// This ELSE was encountered while sequentially scanning the contents
						// of a block or at the otuermost nesting layer.  More thought is required
						// to verify this is correct.  UPDATE: This check is very old and I haven't
						// found a case that can produce it yet, but until proven otherwise its safer
						// to assume it's possible.
						return line_temp->PreparseError(ERR_ELSE_WITH_NO_IF);
					// Let the caller handle this else, since it can't be ours:
					return line_temp;
				}
				// Now use line vs. line_temp to hold the new values, so that line_temp
				// stays as a marker to the ELSE line itself:
				line = line_temp->mNextLine;  // Set it to the else's action line.
				// Update: The following is now impossible because all scripts end in ACT_EXIT.
				// Thus, it's commented out:
				//if (line == NULL) // An else with no action.
				//	return line_temp->PreparseError("Q"); // Placeholder since impossible. Formerly "This ELSE has no action."
				if (line->mActionType == ACT_ELSE || line->mActionType == ACT_BLOCK_END)
					return line_temp->PreparseError("Inappropriate line beneath ELSE.");
				// Assign to line rather than line_temp:
				line = PreparseIfElse(line, ONLY_ONE_LINE, aLoopTypeFile, aLoopTypeReg, aLoopTypeRead
					, aLoopTypeParse);
				if (line == NULL)
					return NULL; // Error or end-of-script.
				// Set this ELSE's jumppoint.  This is similar to the jumppoint set for
				// an ELSEless IF, so see related comments above:
				line_temp->mRelatedLine = line;
			}
			else // line doesn't have an else, so just continue processing from line_temp's position
				line = line_temp;

			// Both cases above have ensured that line is now the first line beyond the
			// scope of the if-statement and that of any ELSE it may have.

			if (aMode == ONLY_ONE_LINE) // Return the next unprocessed line to the caller.
				return line;
			// Otherwise, continue processing at line's new location:
			continue;
		} // ActionType is "IF".

		// Since above didn't continue, do the switch:
		char *line_raw_arg1 = const_cast<char*>(LINE_RAW_ARG1); // Resolve only once to help reduce code size.
		char *line_raw_arg2 = const_cast<char*>(LINE_RAW_ARG2); //

		switch (line->mActionType)
		{
		case ACT_BLOCK_BEGIN:
			line = PreparseIfElse(line->mNextLine, UNTIL_BLOCK_END, aLoopTypeFile, aLoopTypeReg, aLoopTypeRead
				, aLoopTypeParse);
			// "line" is now either NULL due to an error, or the location of the END_BLOCK itself.
			if (line == NULL)
				return NULL; // Error.
			break;
		case ACT_BLOCK_END:
			if (aMode == ONLY_ONE_LINE)
				 // Syntax error.  The caller would never expect this single-line to be an
				 // end-block.  UPDATE: I think this is impossible because callers only use
				 // aMode == ONLY_ONE_LINE when aStartingLine's ActionType is already
				 // known to be an IF or a BLOCK_BEGIN:
				return line->PreparseError("Q"); // Placeholder (see above). Formerly "Unexpected end-of-block (single)."
			if (UNTIL_BLOCK_END)
				// Return line rather than line->mNextLine because, if we're at the end of
				// the script, it's up to the caller to differentiate between that condition
				// and the condition where NULL is an error indicator.
				return line;
			// Otherwise, we found an end-block we weren't looking for.  This should be
			// impossible since the block pre-parsing already balanced all the blocks?
			return line->PreparseError("Q"); // Placeholder (see above). Formerly "Unexpected end-of-block (multi)."
		case ACT_BREAK:
		case ACT_CONTINUE:
			if (!aLoopTypeFile && !aLoopTypeReg && !aLoopTypeRead && !aLoopTypeParse)
				return line->PreparseError("Break/Continue must be enclosed by a Loop.");
			break;

		case ACT_GOTO:  // These two must be done here (i.e. *after* all the script lines have been added),
		case ACT_GOSUB: // so that labels both above and below each Gosub/Goto can be resolved.
			if (line->ArgHasDeref(1))
				// Since the jump-point contains a deref, it must be resolved at runtime:
				line->mRelatedLine = NULL;
			else
				if (!line->GetJumpTarget(false))
					return NULL; // Error was already displayed by called function.
			break;

		// These next 4 must also be done here (i.e. *after* all the script lines have been added),
		// so that labels both above and below this line can be resolved:
		case ACT_ONEXIT:
			if (*line_raw_arg1 && !line->ArgHasDeref(1))
				if (   !(line->mAttribute = FindLabel(line_raw_arg1))   )
					return line->PreparseError(ERR_NO_LABEL);
			break;

		case ACT_HOTKEY:
			if (   *line_raw_arg2 && !line->ArgHasDeref(2)
				&& !line->ArgHasDeref(1) && strnicmp(line_raw_arg1, "IfWin", 5)   ) // v1.0.42: Omit IfWinXX from validation.
				if (   !(line->mAttribute = FindLabel(line_raw_arg2))   )
					if (!Hotkey::ConvertAltTab(line_raw_arg2, true))
						return line->PreparseError(ERR_NO_LABEL);
			break;

		case ACT_SETTIMER:
			if (!line->ArgHasDeref(1))
				if (   !(line->mAttribute = FindLabel(line_raw_arg1))   )
					return line->PreparseError(ERR_NO_LABEL);
			if (*line_raw_arg2 && !line->ArgHasDeref(2))
				if (!Line::ConvertOnOff(line_raw_arg2) && !IsPureNumeric(line_raw_arg2, true) // v1.0.46.16: Allow negatives to support the new run-only-once mode.
					&& !line->mArg[1].is_expression) // v1.0.46.10: Don't consider expressions THAT CONTAIN NO VARIABLES OR FUNCTION-CALLS like "% 2*500" to be a syntax error.
					return line->PreparseError(ERR_PARAM2_INVALID);
			break;

		case ACT_GROUPADD: // This must be done here because it relies on all other lines already having been added.
			if (*LINE_RAW_ARG4 && !line->ArgHasDeref(4))
			{
				// If the label name was contained in a variable, that label is now resolved and cannot
				// be changed.  This is in contrast to something like "Gosub, %MyLabel%" where a change in
				// the value of MyLabel will change the behavior of the Gosub at runtime:
				Label *label = FindLabel(const_cast<char*>(LINE_RAW_ARG4));
				if (!label)
					return line->PreparseError(ERR_NO_LABEL);
				line->mRelatedLine = (Line *)label; // The script loader has ensured that this can't be NULL.
				// Can't do this because the current line won't be the launching point for the
				// Gosub.  Instead, the launching point will be the GroupActivate rather than the
				// GroupAdd, so it will be checked by the GroupActivate or not at all (since it's
				// not that important in the case of a Gosub -- it's mostly for Goto's):
				//return IsJumpValid(label->mJumpToLine);
			}
			break;

		case ACT_ELSE:
			// Should never happen because the part that handles the if's, above, should find
			// all the elses and handle them.  UPDATE: This happens if there's
			// an extra ELSE in this scope level that has no IF:
			return line->PreparseError(ERR_ELSE_WITH_NO_IF);
		} // switch()

		line = line->mNextLine; // If NULL due to physical end-of-script, the for-loop's condition will catch it.
		if (aMode == ONLY_ONE_LINE) // Return the next unprocessed line to the caller.
			// In this case, line shouldn't be (and probably can't be?) NULL because the line after
			// a single-line action shouldn't be the physical end of the script.  That's because
			// the loader has ensured that all scripts now end in ACT_EXIT.  And that final
			// ACT_EXIT should never be parsed here in ONLY_ONE_LINE mode because the only time
			// that mode is used is for the action of an IF, an ELSE, or possibly a LOOP.
			// In all of those cases, the final ACT_EXIT line in the script (which is explicitly
			// insertted by the loader) cannot be the line that was just processed by the
			// switch().  Therefore, the above assignment should not have set line to NULL
			// (which is good because NULL would probably be construed as "failure" by our
			// caller in this case):
			return line;
		// else just continue the for-loop at the new value of line.
	} // for()

	// End of script has been reached.  line is now NULL so don't dereference it.

	// If we were still looking for an EndBlock to match up with a begin, that's an error.
	// This indicates that the at least one BLOCK_BEGIN is missing a BLOCK_END.
	// However, since the blocks were already balanced by the block pre-parsing function,
	// this should be impossible unless the design of this function is flawed.
	if (aMode == UNTIL_BLOCK_END)
#ifdef _DEBUG
		return mLastLine->PreparseError("DEBUG: The script ended while a block was still open."); // This is a bug because the preparser already verified all blocks are balanced.
#else
		return NULL; // Shouldn't happen, so just return failure.
#endif

	// If we were told to process a single line, we were recursed and it should have returned above,
	// so it's an error here (can happen if we were called with aStartingLine == NULL?):
	if (aMode == ONLY_ONE_LINE)
		return mLastLine->PreparseError("Q"); // Placeholder since probably impossible.  Formerly "The script ended while an action was still expected."

	// Otherwise, return something non-NULL to indicate success to the top-level caller:
	return mLastLine;
}


//-------------------------------------------------------------------------------------

// Init static vars:
Line *Line::sLog[] = {NULL};  // Initialize all the array elements.
DWORD Line::sLogTick[]; // No initialization needed.
int Line::sLogNext = 0;  // Start at the first element.

#ifdef AUTOHOTKEYSC  // Reduces code size to omit things that are unused, and helps catch bugs at compile-time.
	char *Line::sSourceFile[1]; // No init needed.
#else
	char **Line::sSourceFile = NULL; // Init to NULL for use with realloc() and for maintainability.
	int Line::sMaxSourceFiles = 0;
#endif
	int Line::sSourceFileCount = 0; // Zero source files initially.  The main script will be the first.

char *Line::sDerefBuf = NULL;  // Buffer to hold the values of any args that need to be dereferenced.
size_t Line::sDerefBufSize = 0;
int Line::sLargeDerefBufs = 0; // Keeps track of how many large bufs exist on the call-stack, for the purpose of determining when to stop the buffer-freeing timer.
char *Line::sArgDeref[MAX_ARGS]; // No init needed.
Var *Line::sArgVar[MAX_ARGS]; // Same.


void Line::FreeDerefBufIfLarge()
{
	if (sDerefBufSize > LARGE_DEREF_BUF_SIZE)
	{
		// Freeing the buffer should be safe even if the script's current quasi-thread is in the middle
		// of executing a command, since commands are all designed to make only temporary use of the
		// deref buffer (they make copies of anything they need prior to calling MsgSleep() or anything
		// else that might pump messages and thus result in a call to us here).
		free(sDerefBuf); // The above size-check has ensured this is non-NULL.
		SET_S_DEREF_BUF(NULL, 0);
		--sLargeDerefBufs;
		if (!sLargeDerefBufs)
			KILL_DEREF_TIMER
	}
	//else leave the timer running because some other deref buffer in a recursed ExpandArgs() layer
	// is still waiting to be freed (even if it isn't, it should be harmless to keep the timer running
	// just in case, since each call to ExpandArgs() will reset/postpone the timer due to the script
	// having demonstrated that it isn't idle).
}



ResultType Line::ExecUntil(ExecUntilMode aMode, char **apReturnValue, Line **apJumpToLine)
// Start executing at "this" line, stop when aMode indicates.
// RECURSIVE: Handles all lines that involve flow-control.
// aMode can be UNTIL_RETURN, UNTIL_BLOCK_END, ONLY_ONE_LINE.
// Returns FAIL, OK, EARLY_RETURN, or EARLY_EXIT.
// apJumpToLine is a pointer to Line-ptr (handle), which is an output parameter.  If NULL,
// the caller is indicating it doesn't need this value, so it won't (and can't) be set by
// the called recursion layer.
{
	Line *unused_jump_to_line;
	Line *&caller_jump_to_line = apJumpToLine ? *apJumpToLine : unused_jump_to_line; // Simplifies code in other places.
	// Important to init, since most of the time it will keep this value.
	// Tells caller that no jump is required (default):
	caller_jump_to_line = NULL;

	// The benchmark improvement of having the following variables declared outside the loop rather than inside
	// is about 0.25%.  Since that is probably not even statistically significant, the only reason for declaring
	// them here is in case compilers other than MSVC++ 7.1 benefit more -- and because it's an old silly habit.
	__int64 loop_iteration;
	WIN32_FIND_DATA *loop_file;
	RegItemStruct *loop_reg_item;
	LoopReadFileStruct *loop_read_file;
	char *loop_field;

	Line *jump_to_line; // Don't use *apJumpToLine because it might not exist.
	Label *jump_to_label;  // For use with Gosub & Goto & GroupActivate.
	ResultType if_condition, result;
	LONG_OPERATION_INIT

	for (Line *line = this; line != NULL;)
	{
		// If a previous command (line) had the clipboard open, perhaps because it directly accessed
		// the clipboard via Var::Contents(), we close it here for performance reasons (see notes
		// in Clipboard::Open() for details):
		CLOSE_CLIPBOARD_IF_OPEN;

		// The below must be done at least when the keybd or mouse hook is active, but is currently
		// always done since it's a very low overhead call, and has the side-benefit of making
		// the app maximally responsive when the script is busy during high BatchLines.
		// This low-overhead call achieves at least two purposes optimally:
		// 1) Keyboard and mouse lag is minimized when the hook(s) are installed, since this single
		//    Peek() is apparently enough to route all pending input to the hooks (though it's inexplicable
		//    why calling MsgSleep(-1) does not achieve this goal, since it too does a Peek().
		//    Nevertheless, that is the testing result that was obtained: the mouse cursor lagged
		//    in tight script loops even when MsgSleep(-1) or (0) was called every 10ms or so.
		// 2) The app is maximally responsive while executing with a high or infinite BatchLines.
		// 3) Hotkeys are maximally responsive.  For example, if a user has game hotkeys, using
		//    a GetTickCount() method (which very slightly improves performance by cutting back on
		//    the number of Peek() calls) would introduce up to 10ms of delay before the hotkey
		//    finally takes effect.  10ms can be significant in games, where ping (latency) itself
		//    can sometimes be only 10 or 20ms. UPDATE: It looks like PeekMessage() yields CPU time
		//    automatically, similar to a Sleep(0), when our queue has no messages.  Since this would
		//    make scripts slow to a crawl, only do the Peek() every 5ms or so (though the timer
		//    granularity is 10ms on mosts OSes, so that's the true interval).
		// 4) Timed subroutines are run as consistently as possible (to help with this, a check
		//    similar to the below is also done for single commmands that take a long time, such
		//    as URLDownloadToFile, FileSetAttrib, etc.
		LONG_OPERATION_UPDATE

		// If interruptions are currently forbidden, it's our responsibility to check if the number
		// of lines that have been run since this quasi-thread started now indicate that
		// interruptibility should be reenabled.  But if UninterruptedLineCountMax is negative, don't
		// bother checking because this quasi-thread will stay non-interruptible until it finishes.
		// v1.0.38.04: If g.ThreadIsCritical==true, no need to check or accumulate g.UninterruptedLineCount
		// because the script is now in charge of this thread's interruptibility.
		if (!g.AllowThreadToBeInterrupted && !g.ThreadIsCritical && g_script.mUninterruptedLineCountMax > -1) // Ordered for short-circuit performance.
		{
			// Note that there is a timer that handles the UninterruptibleTime setting, so we don't
			// have handle that setting here.  But that timer is killed by the DISABLE_UNINTERRUPTIBLE
			// macro we call below.  This is because we don't want the timer to "fire" after we've
			// already met the conditions which allow interruptibility to be restored, because if
			// it did, it might interfere with the fact that some other code might already be using
			// g.AllowThreadToBeInterrupted again for its own purpose:
			if (g.UninterruptedLineCount > g_script.mUninterruptedLineCountMax)
				MAKE_THREAD_INTERRUPTIBLE
			else
				// Incrementing this unconditionally makes it a cruder measure than g.LinesPerCycle,
				// but it seems okay to be less accurate for this purpose:
				++g.UninterruptedLineCount;
		}

		// The below handles the message-loop checking regardless of whether
		// aMode is ONLY_ONE_LINE (i.e. recursed) or not (i.e. we're using
		// the for-loop to execute the script linearly):
		if ((g.LinesPerCycle > -1 && g_script.mLinesExecutedThisCycle >= g.LinesPerCycle)
			|| (g.IntervalBeforeRest > -1 && tick_now - g_script.mLastScriptRest >= (DWORD)g.IntervalBeforeRest))
			// Sleep in between batches of lines, like AutoIt, to reduce the chance that
			// a maxed CPU will interfere with time-critical apps such as games,
			// video capture, or video playback.  Note: MsgSleep() will reset
			// mLinesExecutedThisCycle for us:
			MsgSleep(10);  // Don't use INTERVAL_UNSPECIFIED, which wouldn't sleep at all if there's a msg waiting.

		// At this point, a pause may have been triggered either by the above MsgSleep()
		// or due to the action of a command (e.g. Pause, or perhaps tray menu "pause" was selected during Sleep):
		while (g.IsPaused) // Benches slightly faster than while() for some reason. Also, an initial "if (g.IsPaused)" prior to the loop doesn't make it any faster.
			MsgSleep(INTERVAL_UNSPECIFIED);  // Must check often to periodically run timed subroutines.

		// Do these only after the above has had its opportunity to spend a significant amount
		// of time doing what it needed to do.  i.e. do these immediately before the line will actually
		// be run so that the time it takes to run will be reflected in the ListLines log.
        g_script.mCurrLine = line;  // Simplifies error reporting when we get deep into function calls.

		// Maintain a circular queue of the lines most recently executed:
		sLog[sLogNext] = line; // The code actually runs faster this way than if this were combined with the above.
		// Get a fresh tick in case tick_now is out of date.  Strangely, it takes benchmarks 3% faster
		// on my system with this line than without it, but that's probably just a quirk of the build
		// or the CPU's caching.  It was already shown previously that the released version of 1.0.09
		// was almost 2% faster than an early version of this version (yet even now, that prior version
		// benchmarks slower than this one, which I can't explain).
		sLogTick[sLogNext++] = GetTickCount();  // Incrementing here vs. separately benches a little faster.
		if (sLogNext >= LINE_LOG_SIZE)
			sLogNext = 0;

		// Do this only after the opportunity to Sleep (above) has passed, because during
		// that sleep, a new subroutine might be launched which would likely overwrite the
		// deref buffer used for arg expansion, below:
		// Expand any dereferences contained in this line's args.
		// Note: Only one line at a time be expanded via the above function.  So be sure
		// to store any parts of a line that are needed prior to moving on to the next
		// line (e.g. control stmts such as IF and LOOP).  Also, don't expand
		// ACT_ASSIGN because a more efficient way of dereferencing may be possible
		// in that case:
		if (line->mActionType != ACT_ASSIGN)
		{
			result = line->ExpandArgs();
			// As of v1.0.31, ExpandArgs() will also return EARLY_EXIT if a function call inside one of this
			// line's expressions did an EXIT.
			if (result != OK)
				return result; // In the case of FAIL: Abort the current subroutine, but don't terminate the app.
		}

		if (ACT_IS_IF(line->mActionType))
		{
			++g_script.mLinesExecutedThisCycle;  // If and its else count as one line for this purpose.
			if_condition = line->EvaluateCondition();
			if (if_condition == FAIL)
				return FAIL;
			if (if_condition == CONDITION_TRUE)
			{
				// line->mNextLine has already been verified non-NULL by the pre-parser, so
				// this dereference is safe:
				result = line->mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
				if (jump_to_line == line)
					// Since this IF's ExecUntil() encountered a Goto whose target is the IF
					// itself, continue with the for-loop without moving to a different
					// line.  Also: stay in this recursion layer even if aMode == ONLY_ONE_LINE
					// because we don't want the caller handling it because then it's cleanup
					// to jump to its end-point (beyond its own and any unowned elses) won't work.
					// Example:
					// if x  <-- If this layer were to do it, its own else would be unexpectedly encountered.
					//    label1:
					//    if y  <-- We want this statement's layer to handle the goto.
					//       goto, label1
					//    else
					//       ...
					// else
					//   ...
					continue;
				if (aMode == ONLY_ONE_LINE)
				{
					// When jump_to_line!=NULL, the above call to ExecUntil() told us to jump somewhere.
					// But since we're in ONLY_ONE_LINE mode, our caller must handle it because only it knows how
					// to extricate itself from whatever it's doing:
					caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump (if applicable). jump_to_line==NULL is ok.
					return result;
				}
				if (result == FAIL || result == EARLY_RETURN || result == EARLY_EXIT
					|| result == LOOP_BREAK || result == LOOP_CONTINUE)
					// EARLY_RETURN can occur if this if's action was a block, and that block
					// contained a RETURN, or if this if's only action is RETURN.  It can't
					// occur if we just executed a Gosub, because that Gosub would have been
					// done from a deeper recursion layer (and executing a Gosub in
					// ONLY_ONE_LINE mode can never return EARLY_RETURN).
					return result;
				// Now this if-statement, including any nested if's and their else's,
				// has been fully evaluated by the recusion above.  We must jump to
				// the end of this if-statement to get to the right place for
				// execution to resume.  UPDATE: Or jump to the goto target if the
				// call to ExecUntil told us to do that instead:
				if (jump_to_line != NULL && jump_to_line->mParentLine != line->mParentLine)
				{
					caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump.
					return OK;
				}
				if (jump_to_line != NULL) // jump to where the caller told us to go, rather than the end of IF.
					line = jump_to_line;
				else // Do the normal clean-up for an IF statement:
				{
					// Set line to be either the IF's else or the end of the if-stmt:
					if (   !(line = line->mRelatedLine)   )
						// The preparser has ensured that the only time this can happen is when
						// the end of the script has been reached (i.e. this if-statement
						// has no else and it's the last statement in the script):
						return OK;
					if (line->mActionType == ACT_ELSE)
						line = line->mRelatedLine;
						// Now line is the ELSE's "I'm finished" jump-point, which is where
						// we want to be.  If line is now NULL, it will be caught when this
						// loop iteration is ended by the "continue" stmt below.  UPDATE:
						// it can't be NULL since all scripts now end in ACT_EXIT.
					// else the IF had NO else, so we're already at the IF's "I'm finished" jump-point.
				}
			}
			else // if_condition == CONDITION_FALSE
			{
				if (   !(line = line->mRelatedLine)   )  // Set to IF's related line.
					// The preparser has ensured that this can only happen if the end of the script
					// has been reached.  UPDATE: Probably can't happen anymore since all scripts
					// are now provided with a terminating ACT_EXIT:
					return OK;
				if (line->mActionType != ACT_ELSE && aMode == ONLY_ONE_LINE)
					// Since this IF statement has no ELSE, and since it was executed
					// in ONLY_ONE_LINE mode, the IF-ELSE statement, which counts as
					// one line for the purpose of ONLY_ONE_LINE mode, has finished:
					return OK;
				if (line->mActionType == ACT_ELSE) // This IF has an else.
				{
					// Preparser has ensured that every ELSE has a non-NULL next line:
					result = line->mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
					if (aMode == ONLY_ONE_LINE)
					{
						// When jump_to_line!=NULL, the above call to ExecUntil() told us to jump somewhere.
						// But since we're in ONLY_ONE_LINE mode, our caller must handle it because only it knows how
						// to extricate itself from whatever it's doing:
						caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump (if applicable). jump_to_line==NULL is ok.
						return result;
					}
					if (result == FAIL || result == EARLY_RETURN || result == EARLY_EXIT
						|| result == LOOP_BREAK || result == LOOP_CONTINUE)
						return result;
					if (jump_to_line != NULL && jump_to_line->mParentLine != line->mParentLine)
					{
						caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump.
						return OK;
					}
					if (jump_to_line != NULL)
						// jump to where the called function told us to go, rather than the end of our ELSE.
						line = jump_to_line;
					else // Do the normal clean-up for an ELSE statement.
						line = line->mRelatedLine;
						// Now line is the ELSE's "I'm finished" jump-point, which is where
						// we want to be.  If line is now NULL, it will be caught when this
						// loop iteration is ended by the "continue" stmt below.  UPDATE:
						// it can't be NULL since all scripts now end in ACT_EXIT.
					// else the IF had NO else, so we're already at the IF's "I'm finished" jump-point.
				}
				// else the IF had NO else, so we're already at the IF's "I'm finished" jump-point.
			} // if_condition == CONDITION_FALSE
			continue; // Let the for-loop process the new location specified by <line>.
		} // if (ACT_IS_IF)

		// If above didn't continue, it's not an IF, so handle the other
		// flow-control types:
		switch (line->mActionType)
		{
		case ACT_GOSUB:
			// A single gosub can cause an infinite loop if misused (i.e. recusive gosubs),
			// so be sure to do this to prevent the program from hanging:
			++g_script.mLinesExecutedThisCycle;
			if (   !(jump_to_label = (Label *)line->mRelatedLine)   )
				// The label is a dereference, otherwise it would have been resolved at load-time.
				// So send true because we don't want to update its mRelatedLine.  This is because
				// we want to resolve the label every time through the loop in case the variable
				// that contains the label changes, e.g. Gosub, %MyLabel%
				if (   !(jump_to_label = line->GetJumpTarget(true))   )
					return FAIL; // Error was already displayed by called function.
			// I'm pretty sure it's not valid for this call to ExecUntil() to tell us to jump
			// somewhere, because the called function, or a layer even deeper, should handle
			// the goto prior to returning to us?  So the last parameter is omitted:
			result = jump_to_label->Execute();
			// Must do these return conditions in this specific order:
			if (result == FAIL || result == EARLY_EXIT)
				return result;
			if (aMode == ONLY_ONE_LINE)
				// This Gosub doesn't want its caller to know that the gosub's
				// subroutine returned early:
				return (result == EARLY_RETURN) ? OK : result;
			// If the above didn't return, the subroutine finished successfully and
			// we should now continue on with the line after the Gosub:
			line = line->mNextLine;
			continue;  // Resume looping starting at the above line.  "continue" is actually slight faster than "break" in these cases.

		case ACT_GOTO:
			// A single goto can cause an infinite loop if misused, so be sure to do this to
			// prevent the program from hanging:
			++g_script.mLinesExecutedThisCycle;
			if (   !(jump_to_label = (Label *)line->mRelatedLine)   )
				// The label is a dereference, otherwise it would have been resolved at load-time.
				// So send true because we don't want to update its mRelatedLine.  This is because
				// we want to resolve the label every time through the loop in case the variable
				// that contains the label changes, e.g. Gosub, %MyLabel%
				if (   !(jump_to_label = line->GetJumpTarget(true))   )
					return FAIL; // Error was already displayed by called function.
			// Now that the Goto is certain to occur:
			g.CurrentLabel = jump_to_label; // v1.0.46.16: Support A_ThisLabel.
			// One or both of these lines can be NULL.  But the preparser should have
			// ensured that all we need to do is a simple compare to determine
			// whether this Goto should be handled by this layer or its caller
			// (i.e. if this Goto's target is not in our nesting level, it MUST be the
			// caller's responsibility to either handle it or pass it on to its
			// caller).
			if (aMode == ONLY_ONE_LINE || line->mParentLine != jump_to_label->mJumpToLine->mParentLine)
			{
				caller_jump_to_line = jump_to_label->mJumpToLine; // Tell the caller to handle this jump.
				return OK;
			}
			// Otherwise, we will handle this Goto since it's in our nesting layer:
			line = jump_to_label->mJumpToLine;
			continue;  // Resume looping starting at the above line.  "continue" is actually slight faster than "break" in these cases.

		case ACT_GROUPACTIVATE: // Similar to ACT_GOSUB, which is why this section is here rather than in Perform().
		{
			++g_script.mLinesExecutedThisCycle; // Always increment for GroupActivate.
			WinGroup *group;
			if (   !(group = (WinGroup *)mAttribute)   )
				group = g_script.FindGroup(ARG1);
			result = OK; // Set default.
			if (group)
			{
				// Note: This will take care of DoWinDelay if needed:
				group->Activate(*ARG2 && !stricmp(ARG2, "R"), NULL, &jump_to_label);
				if (jump_to_label)
				{
					if (!line->IsJumpValid(*jump_to_label))
						// This check probably isn't necessary since IsJumpValid() is mostly
						// for Goto's.  But just in case the gosub's target label is some
						// crazy place:
						return FAIL;
					// This section is just like the Gosub code above, so maintain them together.
					result = jump_to_label->Execute();
					if (result == FAIL || result == EARLY_EXIT)
						return result;
				}
			}
			//else no such group, so just proceed.
			if (aMode == ONLY_ONE_LINE)  // v1.0.45: These two lines were moved here from above to provide proper handling for GroupActivate that lacks a jump/gosub and that lies directly beneath an IF or ELSE.
				return (result == EARLY_RETURN) ? OK : result;
			line = line->mNextLine;
			continue;  // Resume looping starting at the above line.  "continue" is actually slight faster than "break" in these cases.
		}

		case ACT_RETURN:
			// Although a return is really just a kind of block-end, keep it separate
			// because when a return is encountered inside a block, it has a double function:
			// to first break out of all enclosing blocks and then return from the gosub.
			// NOTE: The return's ARG1 expression has been evaluated by ExpandArgs() above,
			// which is desirable *even* if apReturnValue is NULL (i.e. the caller will be
			// ignoring the return value) in case the return's expression calls a function
			// which has side-effects.  For example, "return LogThisEvent()".
			if (apReturnValue) // Caller wants the return value.
				*apReturnValue = ARG1; // This sets it to blank if this return lacks an arg.
			//else the return value, if any, is discarded.
			// Don't count returns against the total since they should be nearly instantaneous. UPDATE: even if
			// the return called a function (e.g. return fn()), that function's lines would have been added
			// to the total, so there doesn't seem much problem with not doing it here.
			//++g_script.mLinesExecutedThisCycle;
			if (aMode != UNTIL_RETURN)
				// Tells the caller to return early if it's not the Gosub that directly
				// brought us into this subroutine.  i.e. it allows us to escape from
				// any number of nested blocks in order to get back out of their
				// recursive layers and back to the place this RETURN has meaning
				// to someone (at the right recursion layer):
				return EARLY_RETURN;
			return OK;

		case ACT_BREAK:
			return LOOP_BREAK;

		case ACT_CONTINUE:
			return LOOP_CONTINUE;

		case ACT_LOOP:
		case ACT_REPEAT:
		{
			HKEY root_key_type; // For registry loops, this holds the type of root key, independent of whether it is local or remote.
			AttributeType attr = line->mAttribute;
			if (attr == ATTR_LOOP_REG)
				root_key_type = RegConvertRootKey(ARG1);
			else if (ATTR_LOOP_IS_UNKNOWN_OR_NONE(attr))
			{
				// Since it couldn't be determined at load-time (probably due to derefs),
				// determine whether it's a file-loop, registry-loop or a normal/counter loop.
				// But don't change the value of line->mAttribute because that's our
				// indicator of whether this needs to be evaluated every time for
				// this particular loop (since the nature of the loop can change if the
				// contents of the variables dereferenced for this line change during runtime):
				switch (line->mArgc)
				{
				case 0:
					attr = ATTR_LOOP_NORMAL;
					break;
				case 1:
					// Unlike at loadtime, allow it to be negative at runtime in case it was a variable
					// reference that resolved to a negative number, to indicate that 0 iterations
					// should be performed.  UPDATE: Also allow floating point numbers at runtime
					// but not at load-time (since it doesn't make sense to have a literal floating
					// point number as the iteration count, but a variable containing a pure float
					// should be allowed):
					if (IsPureNumeric(ARG1, true, true, true))
						attr = ATTR_LOOP_NORMAL;
					else
					{
						root_key_type = RegConvertRootKey(ARG1);
						attr = root_key_type ? ATTR_LOOP_REG : ATTR_LOOP_FILEPATTERN;
					}
					break;
				default: // 2 or more args.
					if (!stricmp(ARG1, "Read"))
						attr = ATTR_LOOP_READ_FILE;
					// Note that a "Parse" loop is not allowed to have it's first param be a variable reference
					// that resolves to the word "Parse" at runtime.  This is because the input variable would not
					// have been resolved in this case (since the type of loop was unknown at load-time),
					// and it would be complicated to have to add code for that, especially since there's
					// virtually no conceivable use for allowing it be a variable reference.
					else
					{
						root_key_type = RegConvertRootKey(ARG1);
						attr = root_key_type ? ATTR_LOOP_REG : ATTR_LOOP_FILEPATTERN;
					}
				}
			}

			// HANDLE ANY ERROR CONDITIONS THAT CAN ABORT THE LOOP:
			FileLoopModeType file_loop_mode;
			bool recurse_subfolders;
			if (attr == ATTR_LOOP_FILEPATTERN)
			{
				file_loop_mode = (line->mArgc <= 1) ? FILE_LOOP_FILES_ONLY : ConvertLoopMode(ARG2);
				if (file_loop_mode == FILE_LOOP_INVALID)
					return line->LineError(ERR_PARAM2_INVALID ERR_ABORT, FAIL, ARG2);
				recurse_subfolders = (*ARG3 == '1' && !*(ARG3 + 1));
			}
			else if (attr == ATTR_LOOP_REG)
			{
				file_loop_mode = (line->mArgc <= 2) ? FILE_LOOP_FILES_ONLY : ConvertLoopMode(ARG3);
				if (file_loop_mode == FILE_LOOP_INVALID)
					return line->LineError(ERR_PARAM3_INVALID ERR_ABORT, FAIL, ARG3);
				recurse_subfolders = (*ARG4 == '1' && !*(ARG4 + 1));
			}

			// ONLY AFTER THE ABOVE IS IT CERTAIN THE LOOP WILL LAUNCH (i.e. there was no error or early return).
			// So only now is it safe to make changes to global things like g.mLoopIteration.
			bool continue_main_loop = false; // Init these output parameters prior to starting each type of loop.
			jump_to_line = NULL;             //

			// IN CASE THERE'S AN OUTER LOOP ENCLOSING THIS ONE, BACK UP THE A_LOOPXXX VARIABLES:
			loop_iteration = g.mLoopIteration;
			loop_file = g.mLoopFile;
			loop_reg_item = g.mLoopRegItem;
			loop_read_file = g.mLoopReadFile;
			loop_field = g.mLoopField;

			// INIT "A_INDEX" (one-based not zero-based). This is done here rather than in each PerformLoop()
			// function because it reduces code size and also because registry loops and file-pattern loops
			// can be intrinsically recursive (this is also related to the loop-recursion bugfix documented
			// for v1.0.20: fixes A_Index so that it doesn't wrongly reset to 0 inside recursive file-loops
			// and registry loops).
			g.mLoopIteration = 1;

			// PERFORM THE LOOP:
			/*
			//casts to a type other than an integral or enumeration type cannot appear in a constant-expression
			switch ((size_t)attr)
			{
			case ATTR_LOOP_NORMAL: // Listed first for performance.
				bool is_infinite; // "is_infinite" is more maintainable and future-proof than using LLONG_MAX to simulate an infinite loop. Plus it gives peace-of-mind and the LLONG_MAX method doesn't measurably improve benchmarks (nor does BOOL vs. bool).
				__int64 iteration_limit;
				if (line->mArgc > 0) // At least one parameter is present.
				{
					// Note that a 0 means infinite in AutoIt2 for the REPEAT command; so the following handles
					// that too.
					iteration_limit = ATOI64(ARG1); // If it's negative, zero iterations will be performed automatically.
					is_infinite = (line->mActionType == ACT_REPEAT && !iteration_limit);
				}
				else // It's either ACT_REPEAT or an ACT_LOOP without parameters.
				{
					iteration_limit = 0; // Avoids debug-mode's "used without having been defined" (though it's merely passed as a parameter, not ever used in this case).
					is_infinite = true;  // Override the default set earlier.
				}
				result = line->PerformLoop(apReturnValue, continue_main_loop, jump_to_line
					, iteration_limit, is_infinite);
				break;
			case ATTR_LOOP_PARSE:
				// The phrase "csv" is unique enough since user can always rearrange the letters
				// to do a literal parse using C, S, and V as delimiters:
				if (stricmp(ARG3, "CSV"))
					result = line->PerformLoopParse(apReturnValue, continue_main_loop, jump_to_line);
				else
					result = line->PerformLoopParseCSV(apReturnValue, continue_main_loop, jump_to_line);
				break;
			case ATTR_LOOP_READ_FILE:
				FILE *read_file;
				if (*ARG2 && (read_file = fopen(ARG2, "r"))) // v1.0.47: Added check for "" to avoid debug-assertion failure while in debug mode (maybe it's bad to to open file "" in release mode too).
				{
					result = line->PerformLoopReadFile(apReturnValue, continue_main_loop, jump_to_line, read_file, ARG3);
					fclose(read_file);
				}
				else
					// The open of a the input file failed.  So just set result to OK since setting the
					// ErrorLevel isn't supported with loops (since that seems like it would be an overuse
					// of ErrorLevel, perhaps changing its value too often when the user would want
					// it saved -- in any case, changing that now might break existing scripts).
					result = OK;
				break;
			case ATTR_LOOP_FILEPATTERN:
				result = line->PerformLoopFilePattern(apReturnValue, continue_main_loop, jump_to_line, file_loop_mode
					, recurse_subfolders, ARG1);
				break;
			case ATTR_LOOP_REG:
				// This isn't the most efficient way to do things (e.g. the repeated calls to
				// RegConvertRootKey()), but it the simplest way for now.  Optimization can
				// be done at a later time:
				bool is_remote_registry;
				HKEY root_key;
				if (root_key = RegConvertRootKey(ARG1, &is_remote_registry)) // This will open the key if it's remote.
				{
					// root_key_type needs to be passed in order to support GetLoopRegKey():
					result = line->PerformLoopReg(apReturnValue, continue_main_loop, jump_to_line, file_loop_mode
						, recurse_subfolders, root_key_type, root_key, ARG2);
					if (is_remote_registry)
						RegCloseKey(root_key);
				}
				else
					// The open of a remote key failed (we know it's remote otherwise it should have
					// failed earlier rather than here).  So just set result to OK since no ErrorLevel
					// setting is supported with loops (since that seems like it would be an overuse
					// of ErrorLevel, perhaps changing its value too often when the user would want
					// it saved.  But in any case, changing that now might break existing scripts).
					result = OK;
				break;
			}
			*/

            if (((size_t)attr) == (size_t)ATTR_LOOP_NORMAL) // Listed first for performance.
			{
				bool is_infinite; // "is_infinite" is more maintainable and future-proof than using LLONG_MAX to simulate an infinite loop. Plus it gives peace-of-mind and the LLONG_MAX method doesn't measurably improve benchmarks (nor does BOOL vs. bool).
				__int64 iteration_limit;
				if (line->mArgc > 0) // At least one parameter is present.
				{
					// Note that a 0 means infinite in AutoIt2 for the REPEAT command; so the following handles
					// that too.
					iteration_limit = ATOI64(ARG1); // If it's negative, zero iterations will be performed automatically.
					is_infinite = (line->mActionType == ACT_REPEAT && !iteration_limit);
				}
				else // It's either ACT_REPEAT or an ACT_LOOP without parameters.
				{
					iteration_limit = 0; // Avoids debug-mode's "used without having been defined" (though it's merely passed as a parameter, not ever used in this case).
					is_infinite = true;  // Override the default set earlier.
				}
				result = line->PerformLoop(apReturnValue, continue_main_loop, jump_to_line
					, iteration_limit, is_infinite);
			}
			else if (((size_t)attr) == (size_t)ATTR_LOOP_PARSE)
			{
				// The phrase "csv" is unique enough since user can always rearrange the letters
				// to do a literal parse using C, S, and V as delimiters:
				if (stricmp(ARG3, "CSV"))
					result = line->PerformLoopParse(apReturnValue, continue_main_loop, jump_to_line);
				else
					result = line->PerformLoopParseCSV(apReturnValue, continue_main_loop, jump_to_line);
			}
			else if (((size_t)attr) == (size_t)ATTR_LOOP_READ_FILE)
			{
				FILE *read_file;
				if (*ARG2 && (read_file = fopen(ARG2, "r"))) // v1.0.47: Added check for "" to avoid debug-assertion failure while in debug mode (maybe it's bad to to open file "" in release mode too).
				{
					result = line->PerformLoopReadFile(apReturnValue, continue_main_loop, jump_to_line, read_file, ARG3);
					fclose(read_file);
				}
				else
					// The open of a the input file failed.  So just set result to OK since setting the
					// ErrorLevel isn't supported with loops (since that seems like it would be an overuse
					// of ErrorLevel, perhaps changing its value too often when the user would want
					// it saved -- in any case, changing that now might break existing scripts).
					result = OK;
			}
			else if (((size_t)attr) == (size_t)ATTR_LOOP_FILEPATTERN)
			{
				result = line->PerformLoopFilePattern(apReturnValue, continue_main_loop, jump_to_line, file_loop_mode
					, recurse_subfolders, ARG1);
			}
			else if (((size_t)attr) == (size_t)ATTR_LOOP_REG)
			{
				// This isn't the most efficient way to do things (e.g. the repeated calls to
				// RegConvertRootKey()), but it the simplest way for now.  Optimization can
				// be done at a later time:
				bool is_remote_registry;
				HKEY root_key;
				if (root_key = RegConvertRootKey(ARG1, &is_remote_registry)) // This will open the key if it's remote.
				{
					// root_key_type needs to be passed in order to support GetLoopRegKey():
					result = line->PerformLoopReg(apReturnValue, continue_main_loop, jump_to_line, file_loop_mode
						, recurse_subfolders, root_key_type, root_key, ARG2);
					if (is_remote_registry)
						RegCloseKey(root_key);
				}
				else
					// The open of a remote key failed (we know it's remote otherwise it should have
					// failed earlier rather than here).  So just set result to OK since no ErrorLevel
					// setting is supported with loops (since that seems like it would be an overuse
					// of ErrorLevel, perhaps changing its value too often when the user would want
					// it saved.  But in any case, changing that now might break existing scripts).
					result = OK;
			}

			// RESTORE THE PREVIOUS A_LOOPXXX VARIABLES.  If there isn't an outer loop, this will set them
			// all to NULL/0, which is the most proper and also in keeping with historical behavior.
			// This backup/restore approach was adopted in v1.0.44.14 to simplify things and improve maintainability.
			// This change improved performance by only 1%, which isn't statistically significant.  More importantly,
			// it indirectly fixed the following bug:
			// When a "return" is executed inside a loop's body (and possibly an Exit or Fail too, but those are
			// covered more for simplicity and maintainability), the situations below require superglobals like
			// A_Index and A_LoopField to be restored for the outermost caller of ExecUntil():
			// 1) Var%A_Index% := func_that_returns_directly_from_inside_a_loop_body().
			//    The above happened because the return in the function's loop failed to restore A_Index for its
			//    caller because it had been designed to restore inter-line, not for intra-line activities like
			//    calling functions.
			// 2) A command that has expressions in two separate parameters and one of those parameters calls
			//    a function that returns directly from inside one of its loop bodies.
			//
			// This change was made feasible by making the A_LoopXXX attributes thread-specific, which prevents
			// interrupting threads from affecting the values our thread sees here.  So that change protects
			// against thread interruptions, and this backup/restore change here keeps the Loop variables in
			// sync with the current nesting level (braces, gosub, etc.)
			//
			// The memory for structs like g.mLoopFile resides in the stack of an instance of PerformLoop(),
			// which is our caller or our caller's caller, etc.  In other words, it shouldn't be possible for
			// variables like g.mLoopFile to be non-NULL if there isn't a PerformLoop() beneath us in the stack.
			g.mLoopIteration = loop_iteration;
			g.mLoopFile = loop_file;
			g.mLoopRegItem = loop_reg_item;
			g.mLoopReadFile = loop_read_file;
			g.mLoopField = loop_field;
			// Above is done unconditionally (regardless of the value of "result") for simplicity and maintainability.

			if (result == FAIL || result == EARLY_RETURN || result == EARLY_EXIT)
				return result;
			// else result can be LOOP_BREAK or OK, but not LOOP_CONTINUE.
			if (continue_main_loop) // It signaled us to do this:
				continue;

			if (aMode == ONLY_ONE_LINE)
			{
				// When jump_to_line!=NULL, the above call to ExecUntil() told us to jump somewhere.
				// But since we're in ONLY_ONE_LINE mode, our caller must handle it because only it knows how
				// to extricate itself from whatever it's doing:
				caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump (if any).  jump_to_line==NULL is ok.
				// Return OK even if our result was LOOP_CONTINUE because we already handled the continue:
				return OK;
			}
			if (jump_to_line)
			{
				if (jump_to_line->mParentLine != line->mParentLine)
				{
					// Our caller must handle the jump if it doesn't share the same parent as the
					// current line (i.e. it's not at the same nesting level) because that means
					// the jump target is at a more shallow nesting level than where we are now:
					caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump (if applicable).
					return OK;
				}
				// Since above didn't return, we're supposed to handle this jump.  So jump and then
				// continue execution from there:
				line = jump_to_line;
				continue; // end this case of the switch().
			}
			// Since the above didn't return or break, either the loop has completed the specified
			// number of iterations or it was broken via the break command.  In either case, we jump
			// to the line after our loop's structure and continue there:
			line = line->mRelatedLine;
			continue;  // Resume looping starting at the above line.  "continue" is actually slight faster than "break" in these cases.
		} // case ACT_LOOP.

		case ACT_EXIT:
			// If this script has no hotkeys and hasn't activated one of the hooks, EXIT will cause the
			// the program itself to terminate.  Otherwise, it causes us to return from all blocks
			// and Gosubs (i.e. all the way out of the current subroutine, which was usually triggered
			// by a hotkey):
			if (IS_PERSISTENT)
				return EARLY_EXIT;  // It's "early" because only the very end of the script is the "normal" exit.
				// EARLY_EXIT needs to be distinct from FAIL for ExitApp() and AutoExecSection().
			else
				// This has been tested and it does yield to the OS the error code indicated in ARG1,
				// if present (otherwise it returns 0, naturally) as expected:
				return g_script.ExitApp(EXIT_EXIT, NULL, ATOI(ARG1));

		case ACT_EXITAPP: // Unconditional exit.
			return g_script.ExitApp(EXIT_EXIT, NULL, ATOI(ARG1));

		case ACT_BLOCK_BEGIN:
			if (line->mAttribute) // This is the ACT_BLOCK_BEGIN that starts a function's body.
			{
				// Any time this happens at runtime it means a function has been defined inside the
				// auto-execute section, a block, or other place the flow of execution can reach
				// on its own.  This is not considered a call to the function.  Instead, the entire
				// body is just skipped over using this high performance method.  However, the function's
				// opening brace will show up in ListLines, but that seems preferable to the performance
				// overhead of explicitly removing it here.
				line = line->mRelatedLine; // Resume execution at the line following this functions end-block.
				continue;  // Resume looping starting at the above line.  "continue" is actually slight faster than "break" in these cases.
			}
			// Don't count block-begin/end against the total since they should be nearly instantaneous:
			//++g_script.mLinesExecutedThisCycle;
			// In this case, line->mNextLine is already verified non-NULL by the pre-parser:
			result = line->mNextLine->ExecUntil(UNTIL_BLOCK_END, apReturnValue, &jump_to_line);
			if (jump_to_line == line)
				// Since this Block-begin's ExecUntil() encountered a Goto whose target is the
				// block-begin itself, continue with the for-loop without moving to a different
				// line.  Also: stay in this recursion layer even if aMode == ONLY_ONE_LINE
				// because we don't want the caller handling it because then it's cleanup
				// to jump to its end-point (beyond its own and any unowned elses) won't work.
				// Example:
				// if x  <-- If this layer were to do it, its own else would be unexpectedly encountered.
				// label1:
				// { <-- We want this statement's layer to handle the goto.
				//    if y
				//       goto, label1
				//    else
				//       ...
				// }
				// else
				//   ...
				continue;
			if (aMode == ONLY_ONE_LINE)
			{
				// When jump_to_line!=NULL, the above call to ExecUntil() told us to jump somewhere.
				// But since we're in ONLY_ONE_LINE mode, our caller must handle it because only it knows how
				// to extricate itself from whatever it's doing:
				caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump (if applicable).  jump_to_line==NULL is ok.
				return result;
			}
			if (result == FAIL || result == EARLY_RETURN || result == EARLY_EXIT
				|| result == LOOP_BREAK || result == LOOP_CONTINUE)
				return result;
			// Currently, all blocks are normally executed in ONLY_ONE_LINE mode because
			// they are the direct actions of an IF, an ELSE, or a LOOP.  So the
			// above will already have returned except when the user has created a
			// generic, standalone block with no assciated control statement.
			// Check to see if we need to jump somewhere:
			if (jump_to_line != NULL && line->mParentLine != jump_to_line->mParentLine)
			{
				caller_jump_to_line = jump_to_line; // Tell the caller to handle this jump (if applicable).
				return OK;
			}
			if (jump_to_line != NULL) // jump to where the caller told us to go, rather than the end of our block.
				line = jump_to_line;
			else // Just go to the end of our block and continue from there.
				line = line->mRelatedLine;
				// Now line is the line after the end of this block.  Can be NULL (end of script).
				// UPDATE: It can't be NULL (not that it matters in this case) since the loader
				// has ensured that all scripts now end in an ACT_EXIT.
			continue;  // Resume looping starting at the above line.  "continue" is actually slight faster than "break" in these cases.

		case ACT_BLOCK_END:
			// Don't count block-begin/end against the total since they should be nearly instantaneous:
			//++g_script.mLinesExecutedThisCycle;
			if (aMode != UNTIL_BLOCK_END)
				// Rajat found a way for this to happen that basically amounts to this:
				// If within a loop you gosub a label that is also inside of the block, and
				// that label sometimes doesn't return (i.e. due to a missing "return" somewhere
				// in its flow of control), the loop(s)'s block-end symbols will be encountered
				// by the subroutine, and these symbols don't have meaning to it.  In other words,
				// the subroutine has put us into a waiting-for-return state rather than a
				// waiting-for-block-end state, so when block-end's are encountered, that is
				// considered a runtime error:
				return line->LineError("A \"return\" must be encountered prior to this \"}\"." ERR_ABORT);  // Former error msg was "Unexpected end-of-block (Gosub without Return?)."
			return OK; // It's the caller's responsibility to resume execution at the next line, if appropriate.

		// ACT_ELSE can happen when one of the cases in this switch failed to properly handle
		// aMode == ONLY_ONE_LINE.  But even if ever happens, it will just drop into the default
		// case, which will result in a FAIL (silent exit of thread) as an indicator of the problem.
		// So it's commented out:
		//case ACT_ELSE:
		//	// Shouldn't happen if the pre-parser and this function are designed properly?
		//	return line->LineError("Unexpected ELSE." ERR_ABORT);

		default:
			++g_script.mLinesExecutedThisCycle;
			result = line->Perform();
			if (!result || aMode == ONLY_ONE_LINE)
				// Thus, Perform() should be designed to only return FAIL if it's an error that would make
				// it unsafe to proceed in the subroutine we're executing now:
				return result; // Can be either OK or FAIL.
			line = line->mNextLine;
		} // switch()
	} // for each line

	// Above loop ended because the end of the script was reached.
	// At this point, it should be impossible for aMode to be
	// UNTIL_BLOCK_END because that would mean that the blocks
	// aren't all balanced (or there is a design flaw in this
	// function), but they are balanced because the preparser
	// verified that.  It should also be impossible for the
	// aMode to be ONLY_ONE_LINE because the function is only
	// called in that mode to execute the first action-line
	// beneath an IF or an ELSE, and the preparser has already
	// verified that every such IF and ELSE has a non-NULL
	// line after it.  Finally, aMode can be UNTIL_RETURN, but
	// that is normal mode of operation at the top level,
	// so probably shouldn't be considered an error.  For example,
	// if the script has no hotkeys, it is executed from its
	// first line all the way to the end.  For it not to have
	// a RETURN or EXIT is not an error.  UPDATE: The loader
	// now ensures that all scripts end in ACT_EXIT, so
	// this line should never be reached:
	return OK;
}



ResultType Line::EvaluateCondition() // __forceinline on this reduces benchmarks, probably because it reduces caching effectiveness by having code in the case that doesn't execute much in the benchmarks.
// Returns FAIL, CONDITION_TRUE, or CONDITION_FALSE.
{
#ifdef _DEBUG
	if (!ACT_IS_IF(mActionType))
		return LineError("DEBUG: EvaluateCondition() was called with a line that isn't a condition."
			ERR_ABORT);
#endif

	SymbolType var_is_pure_numeric, value_is_pure_numeric, value2_is_pure_numeric;
	int if_condition;
	char *cp;

	switch (mActionType)
	{
	case ACT_IFEXPR:
		// Use ATOF to support hex, float, and integer formats.  Also, explicitly compare to 0.0
		// to avoid truncation of double, which would result in a value such as 0.1 being seen
		// as false rather than true.  Fixed in v1.0.25.12 so that only the following are false:
		// 0
		// 0.0
		// 0x0
		// (variants of the above)
		// blank string
		// ... in other words, "if var" should be true if it contains a non-numeric string.
		cp = ARG1;  // It should help performance to resolve the ARG1 macro only once.
		if (!*cp)
			if_condition = false;
		else if (!IsPureNumeric(cp, true, false, true)) // i.e. a var containing all whitespace would be considered "true", since it's a non-blank string that isn't equal to 0.0.
			if_condition = true;
		else // It's purely numeric, not blank, and not all whitespace.
			if_condition = (ATOF(cp) != 0.0);
		break;

	// For ACT_IFWINEXIST and ACT_IFWINNOTEXIST, although we validate that at least one
	// of their window params is non-blank during load, it's okay at runtime for them
	// all to resolve to be blank (due to derefs), without an error being reported.
	// It's probably more flexible that way, and in any event WinExist() is equipped to
	// handle all-blank params:
	case ACT_IFWINEXIST:
		// NULL-check this way avoids compiler warnings:
		if_condition = (WinExist(g, FOUR_ARGS, false, true) != NULL);
		break;
	case ACT_IFWINNOTEXIST:
		if_condition = !WinExist(g, FOUR_ARGS, false, true); // Seems best to update last-used even here.
		break;
	case ACT_IFWINACTIVE:
		if_condition = (WinActive(g, FOUR_ARGS, true) != NULL);
		break;
	case ACT_IFWINNOTACTIVE:
		if_condition = !WinActive(g, FOUR_ARGS, true);
		break;

	case ACT_IFEXIST:
		if_condition = DoesFilePatternExist(ARG1);
		break;
	case ACT_IFNOTEXIST:
		if_condition = !DoesFilePatternExist(ARG1);
		break;

	case ACT_IFINSTRING:
	case ACT_IFNOTINSTRING:
		// The most common mode is listed first for performance:
		if_condition = g_strstr(ARG1, ARG2) != NULL; // To reduce code size, resolve large macro only once for both these commands.
		if (mActionType == ACT_IFNOTINSTRING)
			if_condition = !if_condition;
		break;

	case ACT_IFEQUAL:
	case ACT_IFNOTEQUAL:
		// For now, these seem to be the best rules to follow:
		// 1) If either one is non-empty and non-numeric, they're compared as strings.
		// 2) Otherwise, they're compared as numbers (with empty vars treated as zero).
		// In light of the above, two empty values compared to each other is the same as
		// "0 compared to 0".  e.g. if the clipboard is blank, the line "if clipboard ="
		// would be true.  However, the following are side-effects (are there any more?):
		// if var1 =    ; statement is true if var1 contains a literal zero (possibly harmful)
		// if var1 = 0  ; statement is true if var1 is blank (mostly harmless?)
		// if var1 !=   ; statement is false if var1 contains a literal zero (possibly harmful)
		// if var1 != 0 ; statement is false if var1 is blank (mostly harmless?)
		// In light of the above, the BOTH_ARE_NUMERIC macro has been altered to return
		// false if one of the items is a literal zero and the other is blank, so that
		// the two items will be compared as strings.  UPDATE: Altered it again because it
		// seems best to consider blanks to always be non-numeric (i.e. if either var is blank,
		// they will be compared as strings rather than as numbers):

		#undef DETERMINE_NUMERIC_TYPES
		#define DETERMINE_NUMERIC_TYPES \
			value_is_pure_numeric = IsPureNumeric(ARG2, true, false, true);\
			var_is_pure_numeric = IsPureNumeric(ARG1, true, false, true);
		#define DETERMINE_NUMERIC_TYPES2 \
			DETERMINE_NUMERIC_TYPES \
			value2_is_pure_numeric = IsPureNumeric(ARG3, true, false, true);
		#define IF_EITHER_IS_NON_NUMERIC if (!value_is_pure_numeric || !var_is_pure_numeric)
		#define IF_EITHER_IS_NON_NUMERIC2 if (!value_is_pure_numeric || !value2_is_pure_numeric || !var_is_pure_numeric)
		#undef IF_EITHER_IS_FLOAT
		#define IF_EITHER_IS_FLOAT if (value_is_pure_numeric == PURE_FLOAT || var_is_pure_numeric == PURE_FLOAT)

		if (mArgc > 1 && ARGVARRAW1 && ARGVARRAW1->IsBinaryClip() && ARGVARRAW2 && ARGVARRAW2->IsBinaryClip())
			if_condition = (ARGVARRAW1->Length() == ARGVARRAW2->Length()) // Accessing ARGVARRAW in all these places is safe due to the check mArgc > 1.
				&& !memcmp(ARGVARRAW1->Contents(), ARGVARRAW2->Contents(), ARGVARRAW1->Length());
		else
		{
			DETERMINE_NUMERIC_TYPES
			IF_EITHER_IS_NON_NUMERIC
				if_condition = !g_strcmp(ARG1, ARG2);
			else IF_EITHER_IS_FLOAT  // It might perform better to only do float conversions & math when necessary.
				if_condition = ATOF(ARG1) == ATOF(ARG2);
			else
				if_condition = ATOI64(ARG1) == ATOI64(ARG2);
		}
		if (mActionType == ACT_IFNOTEQUAL)
			if_condition = !if_condition;
		break;

	case ACT_IFLESS:
		DETERMINE_NUMERIC_TYPES
		IF_EITHER_IS_NON_NUMERIC
			if_condition = g_strcmp(ARG1, ARG2) < 0;
		else IF_EITHER_IS_FLOAT  // It might perform better to only do float conversions & math when necessary.
			if_condition = ATOF(ARG1) < ATOF(ARG2);
		else
			if_condition = ATOI64(ARG1) < ATOI64(ARG2);
		break;
	case ACT_IFLESSOREQUAL:
		DETERMINE_NUMERIC_TYPES
		IF_EITHER_IS_NON_NUMERIC
			if_condition = g_strcmp(ARG1, ARG2) < 1;
		else IF_EITHER_IS_FLOAT  // It might perform better to only do float conversions & math when necessary.
			if_condition = ATOF(ARG1) <= ATOF(ARG2);
		else
			if_condition = ATOI64(ARG1) <= ATOI64(ARG2);
		break;
	case ACT_IFGREATER:
		DETERMINE_NUMERIC_TYPES
		IF_EITHER_IS_NON_NUMERIC
			if_condition = g_strcmp(ARG1, ARG2) > 0;
		else IF_EITHER_IS_FLOAT  // It might perform better to only do float conversions & math when necessary.
			if_condition = ATOF(ARG1) > ATOF(ARG2);
		else
			if_condition = ATOI64(ARG1) > ATOI64(ARG2);
		break;
	case ACT_IFGREATEROREQUAL:
		DETERMINE_NUMERIC_TYPES
		IF_EITHER_IS_NON_NUMERIC
			if_condition = g_strcmp(ARG1, ARG2) > -1;
		else IF_EITHER_IS_FLOAT  // It might perform better to only do float conversions & math when necessary.
			if_condition = ATOF(ARG1) >= ATOF(ARG2);
		else
			if_condition = ATOI64(ARG1) >= ATOI64(ARG2);
		break;

	case ACT_IFBETWEEN:
	case ACT_IFNOTBETWEEN:
		DETERMINE_NUMERIC_TYPES2
		IF_EITHER_IS_NON_NUMERIC2
		{
			if (g.StringCaseSense == SCS_INSENSITIVE) // The most common mode is listed first for performance.
				if_condition = !(stricmp(ARG1, ARG2) < 0 || stricmp(ARG1, ARG3) > 0);
			else if (g.StringCaseSense == SCS_INSENSITIVE_LOCALE)
				if_condition = lstrcmpi(ARG1, ARG2) > -1 && lstrcmpi(ARG1, ARG3) < 1;
			else  // case sensitive
				if_condition = !(strcmp(ARG1, ARG2) < 0 || strcmp(ARG1, ARG3) > 0);
		}
		else IF_EITHER_IS_FLOAT
		{
			double arg1_as_float = ATOF(ARG1);
			if_condition = arg1_as_float >= ATOF(ARG2) && arg1_as_float <= ATOF(ARG3);
		}
		else
		{
			__int64 arg1_as_int64 = ATOI64(ARG1);
			if_condition = arg1_as_int64 >= ATOI64(ARG2) && arg1_as_int64 <= ATOI64(ARG3);
		}
		if (mActionType == ACT_IFNOTBETWEEN)
			if_condition = !if_condition;
		break;

	case ACT_IFIN:
	case ACT_IFNOTIN:
		if_condition = IsStringInList(ARG1, ARG2, true);
		if (mActionType == ACT_IFNOTIN)
			if_condition = !if_condition;
		break;

	case ACT_IFCONTAINS:
	case ACT_IFNOTCONTAINS:
		if_condition = IsStringInList(ARG1, ARG2, false);
		if (mActionType == ACT_IFNOTCONTAINS)
			if_condition = !if_condition;
		break;

	case ACT_IFIS:
	case ACT_IFISNOT:
	{
		char *cp;
		VariableTypeType variable_type = ConvertVariableTypeName(ARG2);
		if (variable_type == VAR_TYPE_INVALID)
		{
			// Type is probably a dereferenced variable that resolves to an invalid type name.
			// It seems best to make the condition false in these cases, rather than pop up
			// a runtime error dialog:
			if_condition = false;
			break;
		}
		switch(variable_type)
		{
		case VAR_TYPE_NUMBER:
			if_condition = IsPureNumeric(ARG1, true, false, true);  // Floats are defined as being numeric.
			break;
		case VAR_TYPE_INTEGER:
			if_condition = IsPureNumeric(ARG1, true, false, false);
			break;
		case VAR_TYPE_FLOAT:
			if_condition = IsPureNumeric(ARG1, true, false, true) == PURE_FLOAT;
			break;
		case VAR_TYPE_TIME:
		{
			SYSTEMTIME st;
			// Also insist on numeric, because even though YYYYMMDDToFileTime() will properly convert a
			// non-conformant string such as "2004.4", for future compatibility, we don't want to
			// report that such strings are valid times:
			if_condition = IsPureNumeric(ARG1, false, false, false) && YYYYMMDDToSystemTime(ARG1, st, true);
			break;
		}
		case VAR_TYPE_DIGIT:
			if_condition = true;
			for (cp = ARG1; *cp; ++cp)
				if (!isdigit((UCHAR)*cp))
				{
					if_condition = false;
					break;
				}
			break;
		case VAR_TYPE_XDIGIT:
			cp = ARG1;
			if (!strnicmp(cp, "0x", 2)) // v1.0.44.09: Allow 0x prefix, which seems to do more good than harm (unlikely to break existing scripts).
				cp += 2;
			if_condition = true;
			for (; *cp; ++cp)
				if (!isxdigit((UCHAR)*cp))
				{
					if_condition = false;
					break;
				}
			break;
		case VAR_TYPE_ALNUM:
			// Like AutoIt3, the empty string is considered to be alphabetic, which is only slightly debatable.
			if_condition = true;
			for (cp = ARG1; *cp; ++cp)
				if (!IsCharAlphaNumeric(*cp)) // Use this to better support chars from non-English languages.
				{
					if_condition = false;
					break;
				}
			break;
		case VAR_TYPE_ALPHA:
			// Like AutoIt3, the empty string is considered to be alphabetic, which is only slightly debatable.
			if_condition = true;
			for (cp = ARG1; *cp; ++cp)
				if (!IsCharAlpha(*cp)) // Use this to better support chars from non-English languages.
				{
					if_condition = false;
					break;
				}
			break;
		case VAR_TYPE_UPPER:
			if_condition = true;
			for (cp = ARG1; *cp; ++cp)
				if (!IsCharUpper(*cp)) // Use this to better support chars from non-English languages.
				{
					if_condition = false;
					break;
				}
			break;
		case VAR_TYPE_LOWER:
			if_condition = true;
			for (cp = ARG1; *cp; ++cp)
				if (!IsCharLower(*cp)) // Use this to better support chars from non-English languages.
				{
					if_condition = false;
					break;
				}
			break;
		case VAR_TYPE_SPACE:
			if_condition = true;
			for (cp = ARG1; *cp; ++cp)
				if (!isspace(*cp))
				{
					if_condition = false;
					break;
				}
			break;
		}
		if (mActionType == ACT_IFISNOT)
			if_condition = !if_condition;
		break;
	}

	case ACT_IFMSGBOX:
	{
		int mb_result = ConvertMsgBoxResult(ARG1);
		if (!mb_result)
			return LineError(ERR_PARAM1_INVALID ERR_ABORT, FAIL, ARG1);
		if_condition = (g.MsgBoxResult == mb_result);
		break;
	}
	default: // Should never happen, but return an error if it does.
#ifdef _DEBUG
		return LineError("DEBUG: EvaluateCondition(): Unhandled windowing action type." ERR_ABORT);
#else
		return FAIL;
#endif
	}
	return if_condition ? CONDITION_TRUE : CONDITION_FALSE;
}



ResultType Line::PerformLoop(char **apReturnValue, bool &aContinueMainLoop, Line *&aJumpToLine
	, __int64 aIterationLimit, bool aIsInfinite) // bool performs better than BOOL in current benchmarks for this.
// This performs much better (by at least 7%) as a function than as inline code, probably because
// it's only called to set up the loop, not each time through the loop.
{
	ResultType result;
	Line *jump_to_line;
	for (; aIsInfinite || g.mLoopIteration <= aIterationLimit; ++g.mLoopIteration)
	{
		// Execute once the body of the loop (either just one statement or a block of statements).
		// Preparser has ensured that every LOOP has a non-NULL next line.
		result = mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
		{
			// Although ExecUntil() will treat the LOOP_BREAK result identically to OK, we
			// need to return LOOP_BREAK in case our caller is another instance of this
			// same function (i.e. due to recursing into subfolders):
			return result;
		}
		if (jump_to_line)
		{
			if (jump_to_line == this)
				// Since this LOOP's ExecUntil() encountered a Goto whose target is the LOOP
				// itself, continue with the for-loop without moving to a different
				// line.  Also: stay in this recursion layer even if aMode == ONLY_ONE_LINE
				// because we don't want the caller handling it because then it's cleanup
				// to jump to its end-point (beyond its own and any unowned elses) won't work.
				// Example:
				// if x  <-- If this layer were to do it, its own else would be unexpectedly encountered.
				//    label1:
				//    loop  <-- We want this statement's layer to handle the goto.
				//       goto, label1
				// else
				//   ...
				// Also, signal all our callers to return until they get back to the original
				// ExecUntil() instance that started the loop:
				aContinueMainLoop = true;
			else // jump_to_line must be a line that's at the same level or higher as our Exec_Until's LOOP statement itself.
				aJumpToLine = jump_to_line; // Signal the caller to handle this jump.
			break;
		}
		// Otherwise, the result of executing the body of the loop, above, was either OK
		// (the current iteration completed normally) or LOOP_CONTINUE (the current loop
		// iteration was cut short).  In both cases, just continue on through the loop.
	} // for()

	// The script's loop is now over.
	return OK;
}



ResultType Line::PerformLoopFilePattern(char **apReturnValue, bool &aContinueMainLoop, Line *&aJumpToLine
	, FileLoopModeType aFileLoopMode, bool aRecurseSubfolders, char *aFilePattern)
// Note: Even if aFilePattern is just a directory (i.e. with not wildcard pattern), it seems best
// not to append "\\*.*" to it because the pattern might be a script variable that the user wants
// to conditionally resolve to various things at runtime.  In other words, it's valid to have
// only a single directory be the target of the loop.
{
	// Make a local copy of the path given in aFilePattern because as the lines of
	// the loop are executed, the deref buffer (which is what aFilePattern might
	// point to if we were called from ExecUntil()) may be overwritten --
	// and we will need the path string for every loop iteration.  We also need
	// to determine naked_filename_or_pattern:
	char file_path[MAX_PATH], naked_filename_or_pattern[MAX_PATH]; // Giving +3 extra for "*.*" seems fairly pointless because any files that actually need that extra room would fail to be retrieved by FindFirst/Next due to their inability to support paths much over 256.
	size_t file_path_length;
	strlcpy(file_path, aFilePattern, sizeof(file_path));
	char *last_backslash = strrchr(file_path, '\\');
	if (last_backslash)
	{
		strcpy(naked_filename_or_pattern, last_backslash + 1); // Naked filename.  No danger of overflow due size of src vs. dest.
		*(last_backslash + 1) = '\0';  // Convert file_path to be the file's path, but use +1 to retain the final backslash on the string.
		file_path_length = strlen(file_path);
	}
	else
	{
		strcpy(naked_filename_or_pattern, file_path); // No danger of overflow due size of src vs. dest.
		*file_path = '\0'; // There is no path, so make it empty to use current working directory.
		file_path_length = 0;
	}

	// g.mLoopFile is the current file of the file-loop that encloses this file-loop, if any.
	// The below is our own current_file, which will take precedence over g.mLoopFile if this
	// loop is a file-loop:
	BOOL file_found;
	WIN32_FIND_DATA new_current_file;
	HANDLE file_search = FindFirstFile(aFilePattern, &new_current_file);
	for ( file_found = (file_search != INVALID_HANDLE_VALUE) // Convert FindFirst's return value into a boolean so that it's compatible with with FindNext's.
		; file_found && FileIsFilteredOut(new_current_file, aFileLoopMode, file_path, file_path_length)
		; file_found = FindNextFile(file_search, &new_current_file));
	// file_found and new_current_file have now been set for use below.
	// Above is responsible for having properly set file_found and file_search.

	ResultType result;
	Line *jump_to_line;
	for (; file_found; ++g.mLoopIteration)
	{
		g.mLoopFile = &new_current_file; // inner file-loop's file takes precedence over any outer file-loop's.
		// Other types of loops leave g.mLoopFile unchanged so that a file-loop can enclose some other type of
		// inner loop, and that inner loop will still have access to the outer loop's current file.

		// Execute once the body of the loop (either just one statement or a block of statements).
		// Preparser has ensured that every LOOP has a non-NULL next line.
		result = mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
		{
			FindClose(file_search);
			// Although ExecUntil() will treat the LOOP_BREAK result identically to OK, we
			// need to return LOOP_BREAK in case our caller is another instance of this
			// same function (i.e. due to recursing into subfolders):
			return result;
		}
		if (jump_to_line) // See comments in PerformLoop() about this section.
		{
			if (jump_to_line == this)
				aContinueMainLoop = true;
			else
				aJumpToLine = jump_to_line; // Signal our caller to handle this jump.
			break;
		}
		// Otherwise, the result of executing the body of the loop, above, was either OK
		// (the current iteration completed normally) or LOOP_CONTINUE (the current loop
		// iteration was cut short).  In both cases, just continue on through the loop.
		// But first do end-of-iteration steps:
		while ((file_found = FindNextFile(file_search, &new_current_file))
			&& FileIsFilteredOut(new_current_file, aFileLoopMode, file_path, file_path_length)); // Relies on short-circuit boolean order.
			// Above is a self-contained loop that keeps fetching files until there's no more files, or a file
			// is found that isn't filtered out.  It also sets file_found and new_current_file for use by the
			// outer loop.
	} // for()

	// The script's loop is now over.
	if (file_search != INVALID_HANDLE_VALUE)
		FindClose(file_search);

	// If aRecurseSubfolders is true, we now need to perform the loop's body for every subfolder to
	// search for more files and folders inside that match aFilePattern.  We can't do this in the
	// first loop, above, because it may have a restricted file-pattern such as *.txt and we want to
	// find and recurse into ALL folders:
	if (!aRecurseSubfolders) // No need to continue into the "recurse" section.
		return OK;

	// Since above didn't return, this is a file-loop and recursion into sub-folders has been requested.
	// Append *.* to file_path so that we can retrieve all files and folders in the aFilePattern
	// main folder.  We're only interested in the folders, but we have to use *.* to ensure
	// that the search will find all folder names:
	if (file_path_length > sizeof(file_path) - 4) // v1.0.45.03: No room to append "*.*", so for simplicity, skip this folder (don't recurse into it).
		return OK; // This situation might be impossible except for 32000-capable paths because the OS seems to reserve room inside every directory for at least the maximum length of a short filename.
	char *append_pos = file_path + file_path_length;
	strcpy(append_pos, "*.*"); // Above has already verified that no overflow is possible.

	file_search = FindFirstFile(file_path, &new_current_file);
	if (file_search == INVALID_HANDLE_VALUE)
		return OK; // Nothing more to do.
	// Otherwise, recurse into any subdirectories found inside this parent directory.

	size_t path_and_pattern_length = file_path_length + strlen(naked_filename_or_pattern); // Calculated only once for performance.
	do
	{
		if (!(new_current_file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) // We only want directories (except "." and "..").
			|| new_current_file.cFileName[0] == '.' && (!new_current_file.cFileName[1]      // Relies on short-circuit boolean order.
				|| new_current_file.cFileName[1] == '.' && !new_current_file.cFileName[2])  //
			// v1.0.45.03: Skip over folders whose full-path-names are too long to be supported by the ANSI
			// versions of FindFirst/FindNext.  Without this fix, the section below formerly called PerformLoop()
			// with a truncated full-path-name, which caused the last_backslash-finding logic to find the wrong
			// backslash, which in turn caused infinite recursion and a stack overflow (i.e. caused by the
			// full-path-name getting truncated in the same spot every time, endlessly).
			|| path_and_pattern_length + strlen(new_current_file.cFileName) > sizeof(file_path) - 2) // -2 to reflect: 1) the backslash to be added between cFileName and naked_filename_or_pattern; 2) the zero terminator.
			continue;
		// Build the new search pattern, which consists of the original file_path + the subfolder name
		// we just discovered + the original pattern:
		sprintf(append_pos, "%s\\%s", new_current_file.cFileName, naked_filename_or_pattern); // Indirectly set file_path to the new search pattern.  This won't overflow due to the check above.
		// Pass NULL for the 2nd param because it will determine its own current-file when it does
		// its first loop iteration.  This is because this directory is being recursed into, not
		// processed itself as a file-loop item (since this was already done in the first loop,
		// above, if its name matches the original search pattern):
		result = PerformLoopFilePattern(apReturnValue, aContinueMainLoop, aJumpToLine, aFileLoopMode, aRecurseSubfolders, file_path);
		// result should never be LOOP_CONTINUE because the above call to PerformLoop() should have
		// handled that case.  However, it can be LOOP_BREAK if it encoutered the break command.
		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
		{
			FindClose(file_search);
			return result;  // Return even LOOP_BREAK, since our caller can be either ExecUntil() or ourself.
		}
		if (aContinueMainLoop // The call to PerformLoop() above signaled us to break & return.
			|| aJumpToLine)
			// Above: There's no need to check "aJumpToLine == this" because PerformLoop() would already have
			// handled it.  But if it set aJumpToLine to be non-NULL, it means we have to return and let our caller
			// handle the jump.
			break;
	} while (FindNextFile(file_search, &new_current_file));
	FindClose(file_search);

	return OK;
}



ResultType Line::PerformLoopReg(char **apReturnValue, bool &aContinueMainLoop, Line *&aJumpToLine, FileLoopModeType aFileLoopMode
	, bool aRecurseSubfolders, HKEY aRootKeyType, HKEY aRootKey, char *aRegSubkey)
// aRootKeyType is the type of root key, independent of whether it's local or remote.
// This is used because there's no easy way to determine which root key a remote HKEY
// refers to.
{
	RegItemStruct reg_item(aRootKeyType, aRootKey, aRegSubkey);
	HKEY hRegKey;

	// Open the specified subkey.  Be sure to only open with the minimum permission level so that
	// the keys & values can be deleted or written to (though I'm not sure this would be an issue
	// in most cases):
	if (RegOpenKeyEx(reg_item.root_key, reg_item.subkey, 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &hRegKey) != ERROR_SUCCESS)
		return OK;

	// Get the count of how many values and subkeys are contained in this parent key:
	DWORD count_subkeys;
	DWORD count_values;
	if (RegQueryInfoKey(hRegKey, NULL, NULL, NULL, &count_subkeys, NULL, NULL
		, &count_values, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
	{
		RegCloseKey(hRegKey);
		return OK;
	}

	ResultType result;
	Line *jump_to_line;
	DWORD i;

	// See comments in PerformLoop() for details about this section.
	// Note that &reg_item is passed to ExecUntil() rather than... (comment was never finished).
	#define MAKE_SCRIPT_LOOP_PROCESS_THIS_ITEM \
	{\
		g.mLoopRegItem = &reg_item;\
		result = mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);\
		++g.mLoopIteration;\
		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)\
		{\
			RegCloseKey(hRegKey);\
			return result;\
		}\
		if (jump_to_line)\
		{\
			if (jump_to_line == this)\
				aContinueMainLoop = true;\
			else\
				aJumpToLine = jump_to_line;\
			break;\
		}\
	}

	DWORD name_size;

	// First enumerate the values, which are analogous to files in the file system.
	// Later, the subkeys ("subfolders") will be done:
	if (count_values > 0 && aFileLoopMode != FILE_LOOP_FOLDERS_ONLY) // The caller doesn't want "files" (values) excluded.
	{
		reg_item.InitForValues();
		// Going in reverse order allows values to be deleted without disrupting the enumeration,
		// at least in some cases:
		for (i = count_values - 1;; --i)
		{
			// Don't use CONTINUE in loops such as this due to the loop-ending condition being explicitly
			// checked at the bottom.
			name_size = sizeof(reg_item.name);  // Must reset this every time through the loop.
			*reg_item.name = '\0';
			if (RegEnumValue(hRegKey, i, reg_item.name, &name_size, NULL, &reg_item.type, NULL, NULL) == ERROR_SUCCESS)
				MAKE_SCRIPT_LOOP_PROCESS_THIS_ITEM
			// else continue the loop in case some of the lower indexes can still be retrieved successfully.
			if (i == 0)  // Check this here due to it being an unsigned value that we don't want to go negative.
				break;
		}
	}

	// If the loop is neither processing subfolders nor recursing into them, don't waste the performance
	// doing the next loop:
	if (!count_subkeys || (aFileLoopMode == FILE_LOOP_FILES_ONLY && !aRecurseSubfolders))
	{
		RegCloseKey(hRegKey);
		return OK;
	}

	// Enumerate the subkeys, which are analogous to subfolders in the files system:
	// Going in reverse order allows keys to be deleted without disrupting the enumeration,
	// at least in some cases:
	reg_item.InitForSubkeys();
	char subkey_full_path[MAX_REG_ITEM_LENGTH + 1]; // But doesn't include the root key name, which is not only by design but testing shows that if it did, the length could go over 260.
	for (i = count_subkeys - 1;; --i) // Will have zero iterations if there are no subkeys.
	{
		// Don't use CONTINUE in loops such as this due to the loop-ending condition being explicitly
		// checked at the bottom.
		name_size = sizeof(reg_item.name); // Must be reset for every iteration.
		if (RegEnumKeyEx(hRegKey, i, reg_item.name, &name_size, NULL, NULL, NULL, &reg_item.ftLastWriteTime) == ERROR_SUCCESS)
		{
			if (aFileLoopMode != FILE_LOOP_FILES_ONLY) // have the script's loop process this subkey.
				MAKE_SCRIPT_LOOP_PROCESS_THIS_ITEM
			if (aRecurseSubfolders) // Now recurse into the subkey, regardless of whether it was processed above.
			{
				// Build the new subkey name using the an area of memory on the stack that we won't need
				// after the recusive call returns to us.  Omit the leading backslash if subkey is blank,
				// which supports recursively searching the contents of keys contained within a root key
				// (fixed for v1.0.17):
				snprintf(subkey_full_path, sizeof(subkey_full_path), "%s%s%s", reg_item.subkey
					, *reg_item.subkey ? "\\" : "", reg_item.name);
				// This section is very similar to the one in PerformLoop(), so see it for comments:
				result = PerformLoopReg(apReturnValue, aContinueMainLoop, aJumpToLine, aFileLoopMode
					, aRecurseSubfolders, aRootKeyType, aRootKey, subkey_full_path);
				if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
				{
					RegCloseKey(hRegKey);
					return result;
				}
				if (aContinueMainLoop || aJumpToLine)
					break;
			}
		}
		// else continue the loop in case some of the lower indexes can still be retrieved successfully.
		if (i == 0)  // Check this here due to it being an unsigned value that we don't want to go negative.
			break;
	}
	RegCloseKey(hRegKey);
	return OK;
}



ResultType Line::PerformLoopParse(char **apReturnValue, bool &aContinueMainLoop, Line *&aJumpToLine)
{
	if (!*ARG2) // Since the input variable's contents are blank, the loop will execute zero times.
		return OK;

	// The following will be used to hold the parsed items.  It needs to have its own storage because
	// even though ARG2 might always be a writable memory area, we can't rely upon it being
	// persistent because it might reside in the deref buffer, in which case the other commands
	// in the loop's body would probably overwrite it.  Even if the ARG2's contents aren't in
	// the deref buffer, we still can't modify it (i.e. to temporarily terminate it and thus
	// bypass the need for malloc() below) because that might modify the variable contents, and
	// that variable may be referenced elsewhere in the body of the loop (which would result
	// in unexpected side-effects).  So, rather than have a limit of 64K or something (which
	// would limit this feature's usefulness for parsing a large list of filenames, for example),
	// it seems best to dynamically allocate a temporary buffer large enough to hold the
	// contents of ARG2 (the input variable).  Update: Since these loops tend to be enclosed
	// by file-read loops, and thus may be called thousands of times in a short period,
	// it should help average performance to use the stack for small vars rather than
	// constantly doing malloc() and free(), which are much higher overhead and probably
	// cause memory fragmentation (especially with thousands of calls):
	size_t space_needed = ArgLength(2) + 1;  // +1 for the zero terminator.
	char *stack_buf, *buf;
	#define FREE_PARSE_MEMORY if (buf != stack_buf) free(buf)  // Also used by the CSV version of this function.
	#define LOOP_PARSE_BUF_SIZE 40000                          //
	if (space_needed <= LOOP_PARSE_BUF_SIZE)
	{
		stack_buf = (char *)_alloca(LOOP_PARSE_BUF_SIZE); // Helps performance.  See comments above.
		buf = stack_buf;
	}
	else
	{
		if (   !(buf = (char *)malloc(space_needed))   )
			// Probably best to consider this a critical error, since on the rare times it does happen, the user
			// would probably want to know about it immediately.
			return LineError(ERR_OUTOFMEM, FAIL, ARG2);
		stack_buf = NULL; // For comparison purposes later below.
	}
	strcpy(buf, ARG2); // Make the copy.

	// Make a copy of ARG3 and ARG4 in case either one's contents are in the deref buffer, which would
	// probably be overwritten by the commands in the script loop's body:
	char delimiters[512], omit_list[512];
	strlcpy(delimiters, ARG3, sizeof(delimiters));
	strlcpy(omit_list, ARG4, sizeof(omit_list));

	ResultType result;
	Line *jump_to_line;
	char *field, *field_end, saved_char;
	size_t field_length;

	for (field = buf;;)
	{
		if (*delimiters)
		{
			if (   !(field_end = StrChrAny(field, delimiters))   ) // No more delimiters found.
				field_end = field + strlen(field);  // Set it to the position of the zero terminator instead.
		}
		else // Since no delimiters, every char in the input string is treated as a separate field.
		{
			// But exclude this char if it's in the omit_list:
			if (*omit_list && strchr(omit_list, *field))
			{
				++field; // Move on to the next char.
				if (!*field) // The end of the string has been reached.
					break;
				continue;
			}
			field_end = field + 1;
		}

		saved_char = *field_end;  // In case it's a non-delimited list of single chars.
		*field_end = '\0';  // Temporarily terminate so that GetLoopField() will see the correct substring.

		if (*omit_list && *field && *delimiters)  // If no delimiters, the omit_list has already been handled above.
		{
			// Process the omit list.
			field = omit_leading_any(field, omit_list, field_end - field);
			if (*field) // i.e. the above didn't remove all the chars due to them all being in the omit-list.
			{
				field_length = omit_trailing_any(field, omit_list, field_end - 1);
				field[field_length] = '\0';  // Terminate here, but don't update field_end, since saved_char needs it.
			}
		}

		// See comments in PerformLoop() for details about this section.
		g.mLoopField = field;
		result = mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
		++g.mLoopIteration;

		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
		{
			FREE_PARSE_MEMORY;
			return result;
		}
		if (jump_to_line) // See comments in PerformLoop() about this section.
		{
			if (jump_to_line == this)
				aContinueMainLoop = true;
			else
				aJumpToLine = jump_to_line; // Signal our caller to handle this jump.
			break;
		}
		if (!saved_char) // The last item in the list has just been processed, so the loop is done.
			break;
		*field_end = saved_char;  // Undo the temporary termination, in case the list of delimiters is blank.
		field = *delimiters ? field_end + 1 : field_end;  // Move on to the next field.
	}
	FREE_PARSE_MEMORY;
	return OK;
}



ResultType Line::PerformLoopParseCSV(char **apReturnValue, bool &aContinueMainLoop, Line *&aJumpToLine)
// This function is similar to PerformLoopParse() so the two should be maintained together.
// See PerformLoopParse() for comments about the below (comments have been mostly stripped
// from this function).
{
	if (!*ARG2) // Since the input variable's contents are blank, the loop will execute zero times.
		return OK;

	// See comments in PerformLoopParse() for details.
	size_t space_needed = ArgLength(2) + 1;  // +1 for the zero terminator.
	char *stack_buf, *buf;
	if (space_needed <= LOOP_PARSE_BUF_SIZE)
	{
		stack_buf = (char *)_alloca(LOOP_PARSE_BUF_SIZE); // Helps performance.  See comments above.
		buf = stack_buf;
	}
	else
	{
		if (   !(buf = (char *)malloc(space_needed))   )
			return LineError(ERR_OUTOFMEM, FAIL, ARG2);
		stack_buf = NULL; // For comparison purposes later below.
	}
	strcpy(buf, ARG2); // Make the copy.

	char omit_list[512];
	strlcpy(omit_list, ARG4, sizeof(omit_list));

	ResultType result;
	Line *jump_to_line;
	char *field, *field_end, saved_char;
	size_t field_length;
	bool field_is_enclosed_in_quotes;

	for (field = buf;;)
	{
		if (*field == '"')
		{
			// For each field, check if the optional leading double-quote is present.  If it is,
			// skip over it since we always know it's the one that marks the beginning of
			// the that field.  This assumes that a field containing escaped double-quote is
			// always contained in double quotes, which is how Excel does it.  For example:
			// """string with escaped quotes""" resolves to a literal quoted string:
			field_is_enclosed_in_quotes = true;
			++field;
		}
		else
			field_is_enclosed_in_quotes = false;

		for (field_end = field;;)
		{
			if (   !(field_end = strchr(field_end, field_is_enclosed_in_quotes ? '"' : ','))   )
			{
				// This is the last field in the string, so set field_end to the position of
				// the zero terminator instead:
				field_end = field + strlen(field);
				break;
			}
			if (field_is_enclosed_in_quotes)
			{
				// The quote discovered above marks the end of the string if it isn't followed
				// by another quote.  But if it is a pair of quotes, replace it with a single
				// literal double-quote and then keep searching for the real ending quote:
				if (field_end[1] == '"')  // A pair of quotes was encountered.
				{
					memmove(field_end, field_end + 1, strlen(field_end + 1) + 1); // +1 to include terminator.
					++field_end; // Skip over the literal double quote that we just produced.
					continue; // Keep looking for the "real" ending quote.
				}
				// Otherwise, this quote marks the end of the field, so just fall through and break.
			}
			// else field is not enclosed in quotes, so the comma discovered above must be a delimiter.
			break;
		}

		saved_char = *field_end; // This can be the terminator, a comma, or a double-quote.
		*field_end = '\0';  // Terminate here so that GetLoopField() will see the correct substring.

		if (*omit_list && *field)
		{
			// Process the omit list.
			field = omit_leading_any(field, omit_list, field_end - field);
			if (*field) // i.e. the above didn't remove all the chars due to them all being in the omit-list.
			{
				field_length = omit_trailing_any(field, omit_list, field_end - 1);
				field[field_length] = '\0';  // Terminate here, but don't update field_end, since we need its pos.
			}
		}

		// See comments in PerformLoop() for details about this section.
		g.mLoopField = field;
		result = mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
		++g.mLoopIteration;

		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
		{
			FREE_PARSE_MEMORY;
			return result;
		}
		if (jump_to_line) // See comments in PerformLoop() about this section.
		{
			if (jump_to_line == this)
				aContinueMainLoop = true;
			else
				aJumpToLine = jump_to_line; // Signal our caller to handle this jump.
			break;
		}

		if (!saved_char) // The last item in the list has just been processed, so the loop is done.
			break;
		if (saved_char == ',') // Set "field" to be the position of the next field.
			field = field_end + 1;
		else // saved_char must be a double-quote char.
		{
			field = field_end + 1;
			if (!*field) // No more fields occur after this one.
				break;
			// Find the next comma, which must be a real delimiter since we're in between fields:
			if (   !(field = strchr(field, ','))   ) // No more fields.
				break;
			// Set it to be the first character of the next field, which might be a double-quote
			// or another comma (if the field is empty).
			++field;
		}
	}
	FREE_PARSE_MEMORY;
	return OK;
}



ResultType Line::PerformLoopReadFile(char **apReturnValue, bool &aContinueMainLoop, Line *&aJumpToLine, FILE *aReadFile, char *aWriteFileName)
{
	LoopReadFileStruct loop_info(aReadFile, aWriteFileName);
	size_t line_length;
	ResultType result;
	Line *jump_to_line;

	for (; fgets(loop_info.mCurrentLine, sizeof(loop_info.mCurrentLine), loop_info.mReadFile);)
	{
		line_length = strlen(loop_info.mCurrentLine);
		if (line_length && loop_info.mCurrentLine[line_length - 1] == '\n') // Remove newlines like FileReadLine does.
			loop_info.mCurrentLine[--line_length] = '\0';
		// See comments in PerformLoop() for details about this section.
		g.mLoopReadFile = &loop_info;
		result = mNextLine->ExecUntil(ONLY_ONE_LINE, apReturnValue, &jump_to_line);
		++g.mLoopIteration;
		if (result == LOOP_BREAK || result == EARLY_RETURN || result == EARLY_EXIT || result == FAIL)
		{
			if (loop_info.mWriteFile)
				fclose(loop_info.mWriteFile);
			return result;
		}
		if (jump_to_line) // See comments in PerformLoop() about this section.
		{
			if (jump_to_line == this)
				aContinueMainLoop = true;
			else
				aJumpToLine = jump_to_line; // Signal our caller to handle this jump.
			break;
		}
	}

	if (loop_info.mWriteFile)
		fclose(loop_info.mWriteFile);

	// Don't return result because we want to always return OK unless it was one of the values
	// already explicitly checked and returned above.  In other words, there might be values other
	// than OK that aren't explicitly checked for, above.
	return OK;
}



__forceinline ResultType Line::Perform() // __forceinline() currently boosts performance a bit, though it's probably more due to the butterly effect and cache hits/misses.
// Performs only this line's action.
// Returns OK or FAIL.
// The function should not be called to perform any flow-control actions such as
// Goto, Gosub, Return, Block-Begin, Block-End, If, Else, etc.
{
	char buf_temp[MAX_REG_ITEM_LENGTH + 1], *contents; // For registry and other things.
	WinGroup *group; // For the group commands.
	Var *output_var = OUTPUT_VAR; // Okay if NULL. Users of it should only consider it valid if their first arg is actually an output_variable.
	ToggleValueType toggle;  // For commands that use on/off/neutral.
	// Use signed values for these in case they're really given an explicit negative value:
	int start_char_num, chars_to_extract; // For String commands.
	size_t source_length; // For String commands.
	SymbolType var_is_pure_numeric, value_is_pure_numeric; // For math operations.
	vk_type vk; // For GetKeyState.
	Label *target_label;  // For ACT_SETTIMER and ACT_HOTKEY
	int instance_number;  // For sound commands.
	DWORD component_type; // For sound commands.
	__int64 device_id;  // For sound commands.  __int64 helps avoid compiler warning for some conversions.
	bool is_remote_registry; // For Registry commands.
	HKEY root_key; // For Registry commands.
	ResultType result;  // General purpose.

	// Even though the loading-parser already checked, check again, for now,
	// at least until testing raises confidence.  UPDATE: Don't this because
	// sometimes (e.g. ACT_ASSIGN/ADD/SUB/MULT/DIV) the number of parameters
	// required at load-time is different from that at runtime, because params
	// are taken out or added to the param list:
	//if (nArgs < g_act[mActionType].MinParams) ...

	switch (mActionType)
	{
	case ACT_ASSIGN:
		// Note: This line's args have not yet been dereferenced in this case (i.e. ExpandArgs() hasn't been
		// called).  The below function will handle that if it is needed.
		return PerformAssign();  // It will report any errors for us.

	case ACT_ASSIGNEXPR:
		// Currently, ACT_ASSIGNEXPR can occur even when mArg[1].is_expression==false, such as things like var:=5
		// and var:=Array%i%.  Search on "is_expression = " to find such cases in the script-loading/parsing
		// routines.
		if (mArgc > 1)
		{
			if (mArg[1].is_expression) // v1.0.45: ExpandExpression() already took care of it for us (for performance reasons).
				return OK;
			// sArgVar is used to enhance performance, which would otherwise be poor for dynamic variables
			// such as Var:=Array%i% (which is an expression and handled by ACT_ASSIGNEXPR rather than
			// ACT_ASSIGN) because Array%i% would have to be resolved twice (once here and once
			// previously by ExpandArgs()) just to find out if it's IsBinaryClip()).
			if (ARGVARRAW2) // RAW is safe due to the above check of mArgc > 1.
			{
				if (ARGVARRAW2->IsBinaryClip()) // This can be reached via things like: x := binary_clip
					// Performance should be good in this case since IsBinaryClip() implies a single isolated deref,
					// which would never have been copied into the deref buffer.
					return output_var->AssignBinaryClip(*ARGVARRAW2); // ARG2 must be VAR_NORMAL due to IsBinaryClip() check above (it can't even be VAR_CLIPBOARDALL).
				// v1.0.46.01: The following can be reached because loadtime no longer translates such statements
				// into ACT_ASSIGN vs. ACT_ASSIGNEXPR.  Even without that change, it can also be reached by
				// something like:
				//    DynClipboardAll = ClipboardAll
				//    ClipSaved := %DynClipboardAll%
				if (ARGVARRAW2->Type() == VAR_CLIPBOARDALL)
					return output_var->AssignClipboardAll();
			}
		}
		// Note that simple assignments such as Var:="xyz" or Var:=Var2 are resolved to be
		// non-expressions at load-time.  In these cases, ARG2 would have been expanded
		// normally rather than evaluated as an expression.
		return output_var->Assign(ARG2); // ARG2 now contains the evaluated result of the expression.

	case ACT_EXPRESSION:
		// Nothing needs to be done because the expression in ARG1 (which is the only arg) has already
		// been evaluated and its functions and subfunctions called.  Examples:
		//    fn(123, "string", var, fn2(y))
		//    x&=3
		//    var ? func() : x:=y
		return OK;

	// Like AutoIt2, if either output_var or ARG1 aren't purely numeric, they
	// will be considered to be zero for all of the below math functions:
	case ACT_ADD:
		#undef DETERMINE_NUMERIC_TYPES
		#define DETERMINE_NUMERIC_TYPES \
			value_is_pure_numeric = IsPureNumeric(ARG2, true, false, true, true);\
			var_is_pure_numeric = IsPureNumeric(output_var->Contents(), true, false, true, true);

		// Some performance can be gained by relying on the fact that short-circuit boolean
		// can skip the "var_is_pure_numeric" check whenever value_is_pure_numeric == PURE_FLOAT.
		// This is because var_is_pure_numeric is never directly needed here (unlike EvaluateCondition()).
		// However, benchmarks show that this makes such a small difference that it's not worth the
		// loss of maintainability and the slightly larger code size due to macro expansion:
		//#undef IF_EITHER_IS_FLOAT
		//#define IF_EITHER_IS_FLOAT if (value_is_pure_numeric == PURE_FLOAT \
		//	|| IsPureNumeric(output_var->Contents(), true, false, true, true) == PURE_FLOAT)

		DETERMINE_NUMERIC_TYPES

		if (*ARG3 && strchr("SMHD", toupper(*ARG3))) // the command is being used to add a value to a date-time.
		{
			if (!value_is_pure_numeric) // It's considered to be zero, so the output_var is left unchanged:
				return OK;
			else
			{
				// Use double to support a floating point value for days, hours, minutes, etc:
				double nUnits = ATOF(ARG2);  // ATOF() returns a double, at least on MSVC++ 7.x
				FILETIME ft, ftNowUTC;
				if (*output_var->Contents())
				{
					if (!YYYYMMDDToFileTime(output_var->Contents(), ft))
						return output_var->Assign(""); // Set to blank to indicate the problem.
				}
				else // The output variable is currently blank, so substitute the current time for it.
				{
					GetSystemTimeAsFileTime(&ftNowUTC);
					FileTimeToLocalFileTime(&ftNowUTC, &ft);  // Convert UTC to local time.
				}
				// Convert to 10ths of a microsecond (the units of the FILETIME struct):
				switch (toupper(*ARG3))
				{
				case 'S': // Seconds
					nUnits *= (double)10000000;
					break;
				case 'M': // Minutes
					nUnits *= ((double)10000000 * 60);
					break;
				case 'H': // Hours
					nUnits *= ((double)10000000 * 60 * 60);
					break;
				case 'D': // Days
					nUnits *= ((double)10000000 * 60 * 60 * 24);
					break;
				}
				// Convert ft struct to a 64-bit variable (maybe there's some way to avoid these conversions):
				ULARGE_INTEGER ul;
				ul.LowPart = ft.dwLowDateTime;
				ul.HighPart = ft.dwHighDateTime;
				// Add the specified amount of time to the result value:
				ul.QuadPart += (__int64)nUnits;  // Seems ok to cast/truncate in light of the *=10000000 above.
				// Convert back into ft struct:
				ft.dwLowDateTime = ul.LowPart;
				ft.dwHighDateTime = ul.HighPart;
				return output_var->Assign(FileTimeToYYYYMMDD(buf_temp, ft, false));
			}
		}
		else // ARG3 is absent or invalid, so do normal math (not date-time).
		{
			IF_EITHER_IS_FLOAT
				return output_var->Assign(ATOF(output_var->Contents()) + ATOF(ARG2));  // Overload: Assigns a double.
			else // Non-numeric variables or values are considered to be zero for the purpose of the calculation.
				return output_var->Assign(ATOI64(output_var->Contents()) + ATOI64(ARG2));  // Overload: Assigns an int.
		}
		return OK;  // Never executed.

	case ACT_SUB:
		if (*ARG3 && strchr("SMHD", toupper(*ARG3))) // the command is being used to subtract date-time values.
		{
			bool failed;
			// If either ARG2 or output_var->Contents() is blank, it will default to the current time:
			__int64 time_until = YYYYMMDDSecondsUntil(ARG2, output_var->Contents(), failed);
			if (failed) // Usually caused by an invalid component in the date-time string.
				return output_var->Assign("");
			switch (toupper(*ARG3))
			{
			// Do nothing in the case of 'S' (seconds).  Otherwise:
			case 'M': time_until /= 60; break; // Minutes
			case 'H': time_until /= 60 * 60; break; // Hours
			case 'D': time_until /= 60 * 60 * 24; break; // Days
			}
			// Only now that any division has been performed (to reduce the magnitude of
			// time_until) do we cast down into an int, which is the standard size
			// used for non-float results (the result is always non-float for subtraction
			// of two date-times):
			return output_var->Assign(time_until); // Assign as signed 64-bit.
		}
		else // ARG3 is absent or invalid, so do normal math (not date-time).
		{
			DETERMINE_NUMERIC_TYPES
			IF_EITHER_IS_FLOAT
				return output_var->Assign(ATOF(output_var->Contents()) - ATOF(ARG2));  // Overload: Assigns a double.
			else // Non-numeric variables or values are considered to be zero for the purpose of the calculation.
				return output_var->Assign(ATOI64(output_var->Contents()) - ATOI64(ARG2));  // Overload: Assigns an INT.
		}
		// All paths above return.

	case ACT_MULT:
		DETERMINE_NUMERIC_TYPES
		IF_EITHER_IS_FLOAT
			return output_var->Assign(ATOF(output_var->Contents()) * ATOF(ARG2));  // Overload: Assigns a double.
		else // Non-numeric variables or values are considered to be zero for the purpose of the calculation.
			return output_var->Assign(ATOI64(output_var->Contents()) * ATOI64(ARG2));  // Overload: Assigns an INT.

	case ACT_DIV:
		DETERMINE_NUMERIC_TYPES
		IF_EITHER_IS_FLOAT
		{
			double ARG2_as_float = ATOF(ARG2);  // Since ATOF() returns double, at least on MSVC++ 7.x
			if (!ARG2_as_float)              // v1.0.46: Make behavior more consistent with expressions by
				return output_var->Assign(); // avoiding a runtime error dialog; just make the output variable blank.
			return output_var->Assign(ATOF(output_var->Contents()) / ARG2_as_float);  // Overload: Assigns a double.
		}
		else // Non-numeric variables or values are considered to be zero for the purpose of the calculation.
		{
			__int64 ARG2_as_int = ATOI64(ARG2);
			if (!ARG2_as_int)                // v1.0.46: Make behavior more consistent with expressions by
				return output_var->Assign(); // avoiding a runtime error dialog; just make the output variable blank.
			return output_var->Assign(ATOI64(output_var->Contents()) / ARG2_as_int);  // Overload: Assigns an INT.
		}

	case ACT_STRINGLEFT:
		chars_to_extract = ATOI(ARG3); // Use 32-bit signed to detect negatives and fit it VarSizeType.
		if (chars_to_extract < 0)
			// For these we don't report an error, since it might be intentional for
			// it to be called this way, in which case it will do nothing other than
			// set the output var to be blank.
			chars_to_extract = 0;
		else
		{
			source_length = ArgLength(2); // Should be quick because Arg2 is an InputVar (except when it's a built-in var perhaps).
			if (chars_to_extract > (int)source_length)
				chars_to_extract = (int)source_length; // Assign() requires a length that's <= the actual length of the string.
		}
		// It will display any error that occurs.
		return output_var->Assign(ARG2, chars_to_extract);

	case ACT_STRINGRIGHT:
		chars_to_extract = ATOI(ARG3); // Use 32-bit signed to detect negatives and fit it VarSizeType.
		if (chars_to_extract < 0)
			chars_to_extract = 0;
		source_length = ArgLength(2);
		if ((UINT)chars_to_extract > source_length)
			chars_to_extract = (int)source_length;
		// It will display any error that occurs:
		return output_var->Assign(ARG2 + source_length - chars_to_extract, chars_to_extract);

	case ACT_STRINGMID:
		// v1.0.43.10: Allow chars-to-extract to be blank, which means "get all characters".
		// However, for backward compatibility, examine the raw arg, not ARG4.  That way, any existing
		// scripts that use a variable reference or expression that resolves to an empty string will
		// have the parameter treated as zero (as in previous versions) rather than "all characters".
		if (mArgc < 4 || !*mArg[3].text)
			chars_to_extract = INT_MAX;
		else
		{
			chars_to_extract = ATOI(ARG4); // Use 32-bit signed to detect negatives and fit it VarSizeType.
			if (chars_to_extract < 1)
				return output_var->Assign();  // Set it to be blank in this case.
		}
		start_char_num = ATOI(ARG3);
		if (toupper(*ARG5) == 'L')  // Chars to the left of start_char_num will be extracted.
		{
			// TRANSLATE "L" MODE INTO THE EQUIVALENT NORMAL MODE:
			if (start_char_num < 1) // Starting at a character number that is invalid for L mode.
				return output_var->Assign();  // Blank seems most appropriate for the L option in this case.
			start_char_num -= (chars_to_extract - 1);
			if (start_char_num < 1)
				// Reduce chars_to_extract to reflect the fact that there aren't enough chars
				// to the left of start_char_num, so we'll extract only them:
				chars_to_extract -= (1 - start_char_num);
		}
		// ABOVE HAS CONVERTED "L" MODE INTO NORMAL MODE, so "L" no longer needs to be considered below.
		// UPDATE: The below is also needed for the L option to work correctly.  Older:
		// It's somewhat debatable, but it seems best not to report an error in this and
		// other cases.  The result here is probably enough to speak for itself, for script
		// debugging purposes:
		if (start_char_num < 1)
			start_char_num = 1; // 1 is the position of the first char, unlike StringGetPos.
		source_length = ArgLength(2); // This call seems unavoidable in both "L" mode and normal mode.
		if (source_length < (UINT)start_char_num) // Source is empty or start_char_num lies to the right of the entire string.
			return output_var->Assign(); // No chars exist there, so set it to be blank.
		source_length -= (start_char_num - 1); // Fix for v1.0.44.14: Adjust source_length to be the length starting at start_char_num.  Otherwise, the length passed to Assign() could be too long, and it now expects an accurate length.
		if ((UINT)chars_to_extract > source_length)
			chars_to_extract = (int)source_length;
		return output_var->Assign(ARG2 + start_char_num - 1, chars_to_extract);

	case ACT_STRINGTRIMLEFT:
		chars_to_extract = ATOI(ARG3); // Use 32-bit signed to detect negatives and fit it VarSizeType.
		if (chars_to_extract < 0)
			chars_to_extract = 0;
		source_length = ArgLength(2);
		if ((UINT)chars_to_extract > source_length) // This could be intentional, so don't display an error.
			chars_to_extract = (int)source_length;
		return output_var->Assign(ARG2 + chars_to_extract, (VarSizeType)(source_length - chars_to_extract));

	case ACT_STRINGTRIMRIGHT:
		chars_to_extract = ATOI(ARG3); // Use 32-bit signed to detect negatives and fit it VarSizeType.
		if (chars_to_extract < 0)
			chars_to_extract = 0;
		source_length = ArgLength(2);
		if ((UINT)chars_to_extract > source_length) // This could be intentional, so don't display an error.
			chars_to_extract = (int)source_length;
		return output_var->Assign(ARG2, (VarSizeType)(source_length - chars_to_extract)); // It already displayed any error.

	case ACT_STRINGLOWER:
	case ACT_STRINGUPPER:
		contents = output_var->Contents(); // Set default.
		if (contents != ARG2 || output_var->Type() != VAR_NORMAL) // It's compared this way in case ByRef/aliases are involved.  This will detect even them.
		{
			// Clipboard is involved and/or source != dest.  Do it the more comprehensive way.
			// Set up the var, enlarging it if necessary.  If the output_var is of type VAR_CLIPBOARD,
			// this call will set up the clipboard for writing.
			// Fix for v1.0.45.02: The v1.0.45 change where the value is assigned directly without sizing the
			// variable first doesn't work in cases when the variable is the clipboard.  This is because the
			// clipboard's buffer is changeable (for the case conversion later below) only when using the following
			// approach, not a simple "assign then modify its Contents()".
			if (output_var->Assign(NULL, (VarSizeType)ArgLength(2)) != OK)
				return FAIL;
			contents = output_var->Contents(); // Do this only after the above might have changed the contents mem address.
			// Copy the input variable's text directly into the output variable:
			strcpy(contents, ARG2);
		}
		//else input and output are the same, normal variable; so nothing needs to be copied over.  Just leave
		// contents at the default set earlier, then convert its case.
		if (*ARG3 && toupper(*ARG3) == 'T' && !*(ARG3 + 1)) // Convert to title case.
			StrToTitleCase(contents);
		else if (mActionType == ACT_STRINGLOWER)
			CharLower(contents);
		else
			CharUpper(contents);
		return output_var->Close();  // In case it's the clipboard.

	case ACT_STRINGLEN:
		return output_var->Assign((__int64)(ARGVARRAW2 && ARGVARRAW2->IsBinaryClip() // Load-time validation has ensured mArgc > 1.
			? ARGVARRAW2->Length() + 1 // +1 to include the entire 4-byte terminator, which seems best in this case.
			: ArgLength(2)));
		// The above must be kept in sync with the StringLen() function elsewhere.

	case ACT_STRINGGETPOS:
	{
		char *arg4 = ARG4;
		int pos = -1; // Set default.
		int occurrence_number;
		if (*arg4 && strchr("LR", toupper(*arg4)))
			occurrence_number = *(arg4 + 1) ? ATOI(arg4 + 1) : 1;
		else
			occurrence_number = 1;
		// Intentionally allow occurrence_number to resolve to a negative, for scripting flexibililty:
		if (occurrence_number > 0)
		{
			if (!*ARG3) // It might be intentional, in obscure cases, to search for the empty string.
				pos = 0;
				// Above: empty string is always found immediately (first char from left) regardless
				// of whether the search will be conducted from the right.  This is because it's too
				// rare to worry about giving it any more explicit handling based on search direction.
			else
			{
				char *found, *haystack = ARG2, *needle = ARG3;
				int offset = ATOI(ARG5); // v1.0.30.03
				if (offset < 0)
					offset = 0;
				size_t haystack_length = offset ? ArgLength(2) : 1; // Avoids calling ArgLength() if no offset, in which case length isn't needed here.
				if (offset < (int)haystack_length)
				{
					if (*arg4 == '1' || toupper(*arg4) == 'R') // Conduct the search starting at the right side, moving leftward.
					{
						char prev_char, *terminate_here;
						if (offset)
						{
							terminate_here = haystack + haystack_length - offset;
							prev_char = *terminate_here;
							*terminate_here = '\0';  // Temporarily terminate for the duration of the search.
						}
						// Want it to behave like in this example: If searching for the 2nd occurrence of
						// FF in the string FFFF, it should find the first two F's, not the middle two:
						found = strrstr(haystack, needle, (StringCaseSenseType)g.StringCaseSense, occurrence_number);
						if (offset)
							*terminate_here = prev_char;
					}
					else
					{
						// Want it to behave like in this example: If searching for the 2nd occurrence of
						// FF in the string FFFF, it should find position 3 (the 2nd pair), not position 2:
						size_t needle_length = ArgLength(3);
						int i;
						for (i = 1, found = haystack + offset; ; ++i, found += needle_length)
							if (!(found = g_strstr(found, needle)) || i == occurrence_number)
								break;
					}
					if (found)
						pos = (int)(found - haystack);
					// else leave pos set to its default value, -1.
				}
				//else offset >= haystack_length, so no match is possible in either left or right mode.
			}
		}
		g_ErrorLevel->Assign( pos < 0 ? ERRORLEVEL_ERROR : ERRORLEVEL_NONE);
		return output_var->Assign(pos); // Assign() already displayed any error that may have occurred.
	}

	case ACT_STRINGREPLACE:
		return StringReplace();

	case ACT_TRANSFORM:
		return Transform(ARG2, ARG3, ARG4);

	case ACT_STRINGSPLIT:
		return StringSplit(ARG1, ARG2, ARG3, ARG4);

	case ACT_SPLITPATH:
		return SplitPath(ARG1);

	case ACT_SORT:
		return PerformSort(ARG1, ARG2);

	case ACT_PIXELSEARCH:
		// ATOI() works on ARG7 (the color) because any valid BGR or RGB color has 0x00 in the high order byte:
		return PixelSearch(ATOI(ARG3), ATOI(ARG4), ATOI(ARG5), ATOI(ARG6), ATOI(ARG7), ATOI(ARG8), ARG9, false);
	case ACT_IMAGESEARCH:
		return ImageSearch(ATOI(ARG3), ATOI(ARG4), ATOI(ARG5), ATOI(ARG6), ARG7);
	case ACT_PIXELGETCOLOR:
		return PixelGetColor(ATOI(ARG2), ATOI(ARG3), ARG4);

	case ACT_SEND:
	case ACT_SENDRAW:
		SendKeys(ARG1, mActionType == ACT_SENDRAW, g.SendMode);
		return OK;
	case ACT_SENDINPUT: // Raw mode is supported via {Raw} in ARG1.
		SendKeys(ARG1, false, g.SendMode == SM_INPUT_FALLBACK_TO_PLAY ? SM_INPUT_FALLBACK_TO_PLAY : SM_INPUT);
		return OK;
	case ACT_SENDPLAY: // Raw mode is supported via {Raw} in ARG1.
		SendKeys(ARG1, false, SM_PLAY);
		return OK;
	case ACT_SENDEVENT:
		SendKeys(ARG1, false, SM_EVENT);
		return OK;

	case ACT_CLICK:
		return PerformClick(ARG1);
	case ACT_MOUSECLICKDRAG:
		return PerformMouse(mActionType, SEVEN_ARGS);
	case ACT_MOUSECLICK:
		return PerformMouse(mActionType, THREE_ARGS, "", "", ARG5, ARG7, ARG4, ARG6);
	case ACT_MOUSEMOVE:
		return PerformMouse(mActionType, "", ARG1, ARG2, "", "", ARG3, ARG4);

	case ACT_MOUSEGETPOS:
		return MouseGetPos(ATOU(ARG5));

	case ACT_WINACTIVATE:
	case ACT_WINACTIVATEBOTTOM:
		if (WinActivate(g, FOUR_ARGS, mActionType == ACT_WINACTIVATEBOTTOM))
			// It seems best to do these sleeps here rather than in the windowing
			// functions themselves because that way, the program can use the
			// windowing functions without being subject to the script's delay
			// setting (i.e. there are probably cases when we don't need
			// to wait, such as bringing a message box to the foreground,
			// since no other actions will be dependent on it actually
			// having happened:
			DoWinDelay;
		return OK;

	case ACT_WINMINIMIZE:
	case ACT_WINMAXIMIZE:
	case ACT_WINRESTORE:
	case ACT_WINHIDE:
	case ACT_WINSHOW:
	case ACT_WINCLOSE:
	case ACT_WINKILL:
	{
		// Set initial guess for is_ahk_group (further refined later).  For ahk_group, WinText,
		// ExcludeTitle, and ExcludeText must be blank so that they are reserved for future use
		// (i.e. they're currently not supported since the group's own criteria take precedence):
		bool is_ahk_group = !(strnicmp(ARG1, "ahk_group", 9) || *ARG2 || *ARG4);
		// The following is not quite accurate since is_ahk_group is only a guess at this stage, but
		// given the extreme rarity of the guess being wrong, this shortcut seems justified to reduce
		// the code size/complexity.  A wait_time of zero seems best for group closing because it's
		// currently implemented to do the wait after every window in the group.  In addition,
		// this makes "WinClose ahk_group GroupName" behave identically to "GroupClose GroupName",
		// which seems best, for consistency:
		int wait_time = is_ahk_group ? 0 : DEFAULT_WINCLOSE_WAIT;
		if (mActionType == ACT_WINCLOSE || mActionType == ACT_WINKILL) // ARG3 is contains the wait time.
		{
			if (*ARG3 && !(wait_time = (int)(1000 * ATOF(ARG3)))   )
				wait_time = 500; // Legacy (prior to supporting floating point): 0 is defined as 500ms, which seems more useful than a true zero.
			if (*ARG5)
				is_ahk_group = false;  // Override the default.
		}
		else
			if (*ARG3)
				is_ahk_group = false;  // Override the default.
		// Act upon all members of this group (WinText/ExcludeTitle/ExcludeText are ignored in this mode).
		if (is_ahk_group && (group = g_script.FindGroup(omit_leading_whitespace(ARG1 + 9)))) // Assign.
			return group->ActUponAll(mActionType, wait_time); // It will do DoWinDelay if appropriate.
		//else try to act upon it as though "ahk_group something" is a literal window title.

		// Since above didn't return, it's not "ahk_group", so do the normal single-window behavior.
		if (mActionType == ACT_WINCLOSE || mActionType == ACT_WINKILL)
		{
			if (WinClose(g, ARG1, ARG2, wait_time, ARG4, ARG5, mActionType == ACT_WINKILL)) // It closed something.
				DoWinDelay;
			return OK;
		}
		else
			return PerformShowWindow(mActionType, FOUR_ARGS);
	}

	case ACT_ENVGET:
		return EnvGet(ARG2);

	case ACT_ENVSET:
		// MSDN: "If [the 2nd] parameter is NULL, the variable is deleted from the current process�s environment."
		// My: Though it seems okay, for now, just to set it to be blank if the user omitted the 2nd param or
		// left it blank (AutoIt3 does this too).  Also, no checking is currently done to ensure that ARG2
		// isn't longer than 32K, since future OSes may support longer env. vars.  SetEnvironmentVariable()
		// might return 0(fail) in that case anyway.  Also, ARG1 may be a dereferenced variable that resolves
		// to the name of an Env. Variable.  In any case, this name need not correspond to any existing
		// variable name within the script (i.e. script variables and env. variables aren't tied to each other
		// in any way).  This seems to be the most flexible approach, but are there any shortcomings?
		// The only one I can think of is that if the script tries to fetch the value of an env. var (perhaps
		// one that some other spawned program changed), and that var's name corresponds to the name of a
		// script var, the script var's value (if non-blank) will be fetched instead.
		// Note: It seems, at least under WinXP, that env variable names can contain spaces.  So it's best
		// not to validate ARG1 the same way we validate script variables (i.e. just let\
		// SetEnvironmentVariable()'s return value determine whether there's an error).  However, I just
		// realized that it's impossible to "retrieve" the value of an env var that has spaces (until now,
		// since the EnvGet command is available).
		return g_ErrorLevel->Assign(SetEnvironmentVariable(ARG1, ARG2) ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);

	case ACT_ENVUPDATE:
	{
		// From the AutoIt3 source:
		// AutoIt3 uses SMTO_BLOCK (which prevents our thread from doing anything during the call)
		// vs. SMTO_NORMAL.  Since I'm not sure why, I'm leaving it that way for now:
		ULONG nResult;
		if (SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"Environment", SMTO_BLOCK, 15000, &nResult))
			return g_ErrorLevel->Assign(ERRORLEVEL_NONE);
		else
			return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
	}

	case ACT_URLDOWNLOADTOFILE:
		return URLDownloadToFile(TWO_ARGS);

	case ACT_RUNAS:
		if (!g_os.IsWin2000orLater()) // Do nothing if the OS doesn't support it.
			return OK;
		if (mArgc < 1)
		{
			if (!g_script.mRunAsUser) // memory not yet allocated so nothing needs to be done.
				return OK;
			*g_script.mRunAsUser = *g_script.mRunAsPass = *g_script.mRunAsDomain = 0; // wide-character terminator.
			return OK;
		}
		// Otherwise, the credentials are being set or updated:
		if (!g_script.mRunAsUser) // allocate memory (only needed the first time this is done).
		{
			// It's more memory efficient to allocate a single block and divide it up.
			// This memory is freed automatically by the OS upon program termination.
			if (   !(g_script.mRunAsUser = (WCHAR *)malloc(3 * RUNAS_SIZE_IN_BYTES))   )
				return LineError(ERR_OUTOFMEM ERR_ABORT);
			g_script.mRunAsPass = g_script.mRunAsUser + RUNAS_SIZE_IN_WCHARS;   // Fixed for v1.0.47.01 to use RUNAS_SIZE_IN_WCHARS vs. RUNAS_SIZE_IN_BYTES (since pointer math adds 2 bytes not 1 due to the type of pointer).
			g_script.mRunAsDomain = g_script.mRunAsPass + RUNAS_SIZE_IN_WCHARS; //
		}
		ToWideChar(ARG1, g_script.mRunAsUser, RUNAS_SIZE_IN_WCHARS);    // Dest. size is in wchars, not bytes.
		ToWideChar(ARG2, g_script.mRunAsPass, RUNAS_SIZE_IN_WCHARS);    //
		ToWideChar(ARG3, g_script.mRunAsDomain, RUNAS_SIZE_IN_WCHARS);  //
		return OK;

	case ACT_RUN: // Be sure to pass NULL for 2nd param.
		if (strcasestr(ARG3, "UseErrorLevel"))
			return g_ErrorLevel->Assign(g_script.ActionExec(ARG1, NULL, ARG2, false, ARG3, NULL, true
				, true, ARGVAR4) ? ERRORLEVEL_NONE : "ERROR");
			// The special string ERROR is used, rather than a number like 1, because currently
			// RunWait might in the future be able to return any value, including 259 (STATUS_PENDING).
		else // If launch fails, display warning dialog and terminate current thread.
			return g_script.ActionExec(ARG1, NULL, ARG2, true, ARG3, NULL, false, true, ARGVAR4);

	case ACT_RUNWAIT:
	case ACT_CLIPWAIT:
	case ACT_KEYWAIT:
	case ACT_WINWAIT:
	case ACT_WINWAITCLOSE:
	case ACT_WINWAITACTIVE:
	case ACT_WINWAITNOTACTIVE:
		return PerformWait();

	case ACT_WINMOVE:
		return mArgc > 2 ? WinMove(EIGHT_ARGS) : WinMove("", "", ARG1, ARG2);

	case ACT_WINMENUSELECTITEM:
		return WinMenuSelectItem(ELEVEN_ARGS);

	case ACT_CONTROLSEND:
	case ACT_CONTROLSENDRAW:
		return ControlSend(SIX_ARGS, mActionType == ACT_CONTROLSENDRAW);

	case ACT_CONTROLCLICK:
		if (   !(vk = ConvertMouseButton(ARG4))   ) // Treats blank as "Left".
			return LineError(ERR_MOUSE_BUTTON ERR_ABORT, FAIL, ARG4);
		return ControlClick(vk, *ARG5 ? ATOI(ARG5) : 1, ARG6, ARG1, ARG2, ARG3, ARG7, ARG8);

	case ACT_CONTROLMOVE:
		return ControlMove(NINE_ARGS);
	case ACT_CONTROLGETPOS:
		return ControlGetPos(ARG5, ARG6, ARG7, ARG8, ARG9);
	case ACT_CONTROLGETFOCUS:
		return ControlGetFocus(ARG2, ARG3, ARG4, ARG5);
	case ACT_CONTROLFOCUS:
		return ControlFocus(FIVE_ARGS);
	case ACT_CONTROLSETTEXT:
		return ControlSetText(SIX_ARGS);
	case ACT_CONTROLGETTEXT:
		return ControlGetText(ARG2, ARG3, ARG4, ARG5, ARG6);
	case ACT_CONTROL:
		return Control(SEVEN_ARGS);
	case ACT_CONTROLGET:
		return ControlGet(ARG2, ARG3, ARG4, ARG5, ARG6, ARG7, ARG8);
	case ACT_STATUSBARGETTEXT:
		return StatusBarGetText(ARG2, ARG3, ARG4, ARG5, ARG6);
	case ACT_STATUSBARWAIT:
		return StatusBarWait(EIGHT_ARGS);
	case ACT_POSTMESSAGE:
	case ACT_SENDMESSAGE:
		return ScriptPostSendMessage(mActionType == ACT_SENDMESSAGE);
	case ACT_PROCESS:
		return ScriptProcess(THREE_ARGS);
	case ACT_WINSET:
		return WinSet(SIX_ARGS);
	case ACT_WINSETTITLE:
		return mArgc > 1 ? WinSetTitle(FIVE_ARGS) : WinSetTitle("", "", ARG1);
	case ACT_WINGETTITLE:
		return WinGetTitle(ARG2, ARG3, ARG4, ARG5);
	case ACT_WINGETCLASS:
		return WinGetClass(ARG2, ARG3, ARG4, ARG5);
	case ACT_WINGET:
		return WinGet(ARG2, ARG3, ARG4, ARG5, ARG6);
	case ACT_WINGETTEXT:
		return WinGetText(ARG2, ARG3, ARG4, ARG5);
	case ACT_WINGETPOS:
		return WinGetPos(ARG5, ARG6, ARG7, ARG8);

	case ACT_SYSGET:
		return SysGet(ARG2, ARG3);

	case ACT_WINMINIMIZEALL:
		PostMessage(FindWindow("Shell_TrayWnd", NULL), WM_COMMAND, 419, 0);
		DoWinDelay;
		return OK;
	case ACT_WINMINIMIZEALLUNDO:
		PostMessage(FindWindow("Shell_TrayWnd", NULL), WM_COMMAND, 416, 0);
		DoWinDelay;
		return OK;

	case ACT_ONEXIT:
		if (!*ARG1) // Reset to normal Exit behavior.
		{
			g_script.mOnExitLabel = NULL;
			return OK;
		}
		// If it wasn't resolved at load-time, it must be a variable reference:
		if (   !(target_label = (Label *)mAttribute)   )
			if (   !(target_label = g_script.FindLabel(ARG1))   )
				return LineError(ERR_NO_LABEL ERR_ABORT, FAIL, ARG1);
		g_script.mOnExitLabel = target_label;
		return OK;

	case ACT_HOTKEY:
		// mAttribute is the label resolved at loadtime, if available (for performance).
		return Hotkey::Dynamic(THREE_ARGS, (Label *)mAttribute);

	case ACT_SETTIMER: // A timer is being created, changed, or enabled/disabled.
		// Note that only one timer per label is allowed because the label is the unique identifier
		// that allows us to figure out whether to "update or create" when searching the list of timers.
		if (   !(target_label = (Label *)mAttribute)   ) // Since it wasn't resolved at load-time, it must be a variable reference.
			if (   !(target_label = g_script.FindLabel(ARG1))   )
				return LineError(ERR_NO_LABEL ERR_ABORT, FAIL, ARG1);
		// And don't update mAttribute (leave it NULL) because we want ARG1 to be dynamically resolved
		// every time the command is executed (in case the contents of the referenced variable change).
		// In the data structure that holds the timers, we store the target label rather than the target
		// line so that a label can be registered independently as a timers even if there another label
		// that points to the same line such as in this example:
		// Label1::
		// Label2::
		// ...
		// return
		if (*ARG2)
		{
			toggle = Line::ConvertOnOff(ARG2);
			if (!toggle && !IsPureNumeric(ARG2, true, true, true)) // Allow it to be neg. or floating point at runtime.
				return LineError(ERR_PARAM2_INVALID, FAIL, ARG2);
		}
		else
			toggle = TOGGLE_INVALID;
		// Below relies on distinguishing a true empty string from one that is sent to the function
		// as empty as a signal.  Don't change it without a full understanding because it's likely
		// to break compatibility or something else:
		switch(toggle)
		{
		case TOGGLED_ON:
		case TOGGLED_OFF: g_script.UpdateOrCreateTimer(target_label, "", ARG3, toggle == TOGGLED_ON, false); break;
		// Timer is always (re)enabled when ARG2 specifies a numeric period or is blank + there's no ARG3.
		// If ARG2 is blank but ARG3 (priority) isn't, tell it to update only the priority and nothing else:
		default: g_script.UpdateOrCreateTimer(target_label, ARG2, ARG3, true, !*ARG2 && *ARG3);
		}
		return OK;

	case ACT_CRITICAL:
		// For code size reduction, no runtime validation is done (only load-time).  Thus, anything other
		// than "Off" (especially NEUTRAL) is considered to be "On":
		toggle = ConvertOnOff(ARG1, NEUTRAL);
		if (g.ThreadIsCritical = (toggle != TOGGLED_OFF)) // Assign.
		{
			// v1.0.46: When the current thread is critical, have the script check messages less often to
			// reduce situations where an OnMesage or GUI message must be discarded due to "thread already
			// running".  Using 16 rather than the default of 5 solves reliability problems in a custom-menu-draw
			// script and probably many similar scripts -- even when the system is under load (though 16 might not
			// be enough during an extreme load depending on the exact preemption/timeslice dynamics involved).
			// DON'T GO TOO HIGH because this setting reduces response time for ALL messages, even those that
			// don't launch script threads (especially painting/drawing and other screen-update events).
			// Future enhancement: Could allow the value of 16 to be customized via something like "Critical 25".
			// However, it seems best not to allow it to go too high (say, no more than 2000) because that would
			// cause the script to completely hang if the critical thread never finishes, or takes a long time
			// to finish.  A configurable limit might also allow things to work better on Win9x because it has
			// a bigger tickcount granularity.
			if (!*ARG1 || toggle == TOGGLED_ON) // i.e. an omitted first arg is the same as "ON".
				g.PeekFrequency = 16; // Some hardware has a tickcount granularity of 15 instead of 10, so this covers more variations.
			else // ARG1 is present but it's not "On" or "Off"; so treat it as a number.
				g.PeekFrequency = ATOU(ARG1); // For flexibility (and due to rarity), don't bother checking if too large/small (even if it is it's probably inconsequential).
			g.AllowThreadToBeInterrupted = false;
			g.LinesPerCycle = -1;      // v1.0.47: It seems best to ensure SetBatchLines -1 is in effect because
			g.IntervalBeforeRest = -1; // otherwise it may check messages during the interval that it isn't supposed to.
		}
		else
		{
			g.PeekFrequency = DEFAULT_PEEK_FREQUENCY;
			g.AllowThreadToBeInterrupted = true;
		}
		// If it's being turned off, allow thread to be immediately interrupted regardless of any
		// "Thread Interrupt" settings.
		// Now that the thread's interruptibility has been explicitly set, the script is in charge
		// of managing this thread's interruptibility, thus kill the timer unconditionally:
		KILL_UNINTERRUPTIBLE_TIMER  // Done here for maintainability and performance, even though UninterruptibleTimeout() will also kill it.
		// Although the above kills the timer, it does not remove any WM_TIMER message that it might already
		// have placed into the queue.  And since we have other types of timers, purging the queue of all
		// WM_TIMERS would be too great a loss of maintainability and reliability.  To solve this,
		// UninterruptibleTimeout() checks the value of g.ThreadIsCritical.
		return OK;

	case ACT_THREAD:
		switch (ConvertThreadCommand(ARG1))
		{
		case THREAD_CMD_PRIORITY:
			g.Priority = ATOI(ARG2);
			break;
		case THREAD_CMD_INTERRUPT:
			// If either one is blank, leave that setting as it was before.
			if (*ARG1)
				g_script.mUninterruptibleTime = ATOI(ARG2);  // 32-bit (for compatibility with DWORDs returned by GetTickCount).
			if (*ARG2)
				g_script.mUninterruptedLineCountMax = ATOI(ARG3);  // 32-bit also, to help performance (since huge values seem unnecessary).
			break;
		case THREAD_CMD_NOTIMERS:
			g.AllowTimers = (*ARG2 && ATOI64(ARG2) == 0);
			break;
		// If invalid command, do nothing since that is always caught at load-time unless the command
		// is in a variable reference (very rare in this case).
		}
		return OK;

	case ACT_GROUPADD: // Adding a WindowSpec *to* a group, not adding a group.
	{
		if (   !(group = (WinGroup *)mAttribute)   )
			if (   !(group = g_script.FindGroup(ARG1, true))   )  // Last parameter -> create-if-not-found.
				return FAIL;  // It already displayed the error for us.
		target_label = NULL;
		if (*ARG4)
		{
			if (   !(target_label = (Label *)mRelatedLine)   ) // Jump target hasn't been resolved yet, probably due to it being a deref.
				if (   !(target_label = g_script.FindLabel(ARG4))   )
					return LineError(ERR_NO_LABEL ERR_ABORT, FAIL, ARG4);
			// Can't do this because the current line won't be the launching point for the
			// Gosub.  Instead, the launching point will be the GroupActivate rather than the
			// GroupAdd, so it will be checked by the GroupActivate or not at all (since it's
			// not that important in the case of a Gosub -- it's mostly for Goto's):
			//return IsJumpValid(label->mJumpToLine);
		}
		return group->AddWindow(ARG2, ARG3, target_label, ARG5, ARG6);
	}

	// Note ACT_GROUPACTIVATE is handled by ExecUntil(), since it's better suited to do the Gosub.
	case ACT_GROUPDEACTIVATE:
		if (   !(group = (WinGroup *)mAttribute)   )
			group = g_script.FindGroup(ARG1);
		if (group)
			group->Deactivate(*ARG2 && !stricmp(ARG2, "R"));  // Note: It will take care of DoWinDelay if needed.
		//else nonexistent group: By design, do nothing.
		return OK;

	case ACT_GROUPCLOSE:
		if (   !(group = (WinGroup *)mAttribute)   )
			group = g_script.FindGroup(ARG1);
		if (group)
			if (*ARG2 && !stricmp(ARG2, "A"))
				group->ActUponAll(ACT_WINCLOSE, 0);  // Note: It will take care of DoWinDelay if needed.
			else
				group->CloseAndGoToNext(*ARG2 && !stricmp(ARG2, "R"));  // Note: It will take care of DoWinDelay if needed.
		//else nonexistent group: By design, do nothing.
		return OK;

	case ACT_GETKEYSTATE:
		return GetKeyJoyState(ARG2, ARG3);

	case ACT_RANDOM:
	{
		if (!output_var) // v1.0.42.03: Special mode to change the seed.
		{
			init_genrand(ATOU(ARG2)); // It's documented that an unsigned 32-bit number is required.
			return OK;
		}
		bool use_float = IsPureNumeric(ARG2, true, false, true) == PURE_FLOAT
			|| IsPureNumeric(ARG3, true, false, true) == PURE_FLOAT;
		if (use_float)
		{
			double rand_min = *ARG2 ? ATOF(ARG2) : 0;
			double rand_max = *ARG3 ? ATOF(ARG3) : INT_MAX;
			// Seems best not to use ErrorLevel for this command at all, since silly cases
			// such as Max > Min are too rare.  Swap the two values instead.
			if (rand_min > rand_max)
			{
				double rand_swap = rand_min;
				rand_min = rand_max;
				rand_max = rand_swap;
			}
			return output_var->Assign((genrand_real1() * (rand_max - rand_min)) + rand_min);
		}
		else // Avoid using floating point, where possible, which may improve speed a lot more than expected.
		{
			int rand_min = *ARG2 ? ATOI(ARG2) : 0;
			int rand_max = *ARG3 ? ATOI(ARG3) : INT_MAX;
			// Seems best not to use ErrorLevel for this command at all, since silly cases
			// such as Max > Min are too rare.  Swap the two values instead.
			if (rand_min > rand_max)
			{
				int rand_swap = rand_min;
				rand_min = rand_max;
				rand_max = rand_swap;
			}
			// Do NOT use genrand_real1() to generate random integers because of cases like
			// min=0 and max=1: we want an even distribution of 1's and 0's in that case, not
			// something skewed that might result due to rounding/truncation issues caused by
			// the float method used above:
			// AutoIt3: __int64 is needed here to do the proper conversion from unsigned long to signed long:
			return output_var->Assign(   (int)((__int64)(genrand_int32()
				% ((__int64)rand_max - rand_min + 1)) + rand_min)   );
		}
	}

	case ACT_DRIVESPACEFREE:
		return DriveSpace(ARG2, true);

	case ACT_DRIVE:
		return Drive(THREE_ARGS);

	case ACT_DRIVEGET:
		return DriveGet(ARG2, ARG3);

	case ACT_SOUNDGET:
	case ACT_SOUNDSET:
		device_id = *ARG4 ? ATOI(ARG4) - 1 : 0;
		if (device_id < 0)
			device_id = 0;
		instance_number = 1;  // Set default.
		component_type = *ARG2 ? SoundConvertComponentType(ARG2, &instance_number) : MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
		return SoundSetGet(mActionType == ACT_SOUNDGET ? NULL : ARG1
			, component_type, instance_number  // Which instance of this component, 1 = first
			, *ARG3 ? SoundConvertControlType(ARG3) : MIXERCONTROL_CONTROLTYPE_VOLUME  // Default
			, (UINT)device_id);

	case ACT_SOUNDGETWAVEVOLUME:
	case ACT_SOUNDSETWAVEVOLUME:
		device_id = *ARG2 ? ATOI(ARG2) - 1 : 0;
		if (device_id < 0)
			device_id = 0;
		return (mActionType == ACT_SOUNDGETWAVEVOLUME) ? SoundGetWaveVolume((HWAVEOUT)device_id)
			: SoundSetWaveVolume(ARG1, (HWAVEOUT)device_id);

	case ACT_SOUNDBEEP:
		// For simplicity and support for future/greater capabilities, no range checking is done.
		// It simply calls the function with the two DWORD values provided. It avoids setting
		// ErrorLevel because failure is rare and also because a script might want play a beep
		// right before displaying an error dialog that uses the previous value of ErrorLevel.
		Beep(*ARG1 ? ATOU(ARG1) : 523, *ARG2 ? ATOU(ARG2) : 150);
		return OK;

	case ACT_SOUNDPLAY:
		return SoundPlay(ARG1, *ARG2 && !stricmp(ARG2, "wait") || !stricmp(ARG2, "1"));

	case ACT_FILEAPPEND:
		// Uses the read-file loop's current item filename was explicitly leave blank (i.e. not just
		// a reference to a variable that's blank):
		return FileAppend(ARG2, ARG1, (mArgc < 2) ? g.mLoopReadFile : NULL);

	case ACT_FILEREAD:
		return FileRead(ARG2);

	case ACT_FILEREADLINE:
		return FileReadLine(ARG2, ARG3);

	case ACT_FILEDELETE:
		return FileDelete();

	case ACT_FILERECYCLE:
		return FileRecycle(ARG1);

	case ACT_FILERECYCLEEMPTY:
		return FileRecycleEmpty(ARG1);

	case ACT_FILEINSTALL:
		return FileInstall(THREE_ARGS);

	case ACT_FILECOPY:
	{
		int error_count = Util_CopyFile(ARG1, ARG2, ATOI(ARG3) == 1, false);
		if (!error_count)
			return g_ErrorLevel->Assign(ERRORLEVEL_NONE);
		if (g_script.mIsAutoIt2)
			return g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // For backward compatibility with v2.
		return g_ErrorLevel->Assign(error_count);
	}
	case ACT_FILEMOVE:
		return g_ErrorLevel->Assign(Util_CopyFile(ARG1, ARG2, ATOI(ARG3) == 1, true));
	case ACT_FILECOPYDIR:
		return g_ErrorLevel->Assign(Util_CopyDir(ARG1, ARG2, ATOI(ARG3) == 1) ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);
	case ACT_FILEMOVEDIR:
		if (toupper(*ARG3) == 'R')
		{
			// Perform a simple rename instead, which prevents the operation from being only partially
			// complete if the source directory is in use (due to being a working dir for a currently
			// running process, or containing a file that is being written to).  In other words,
			// the operation will be "all or none":
			g_ErrorLevel->Assign(MoveFile(ARG1, ARG2) ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);
			return OK;
		}
		// Otherwise:
		return g_ErrorLevel->Assign(Util_MoveDir(ARG1, ARG2, ATOI(ARG3)) ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);

	case ACT_FILECREATEDIR:
		return FileCreateDir(ARG1);
	case ACT_FILEREMOVEDIR:
		if (!*ARG1) // Consider an attempt to create or remove a blank dir to be an error.
			return g_ErrorLevel->Assign(ERRORLEVEL_ERROR);
		return g_ErrorLevel->Assign(Util_RemoveDir(ARG1, ATOI(ARG2) == 1) ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);

	case ACT_FILEGETATTRIB:
		// The specified ARG, if non-blank, takes precedence over the file-loop's file (if any):
		#define USE_FILE_LOOP_FILE_IF_ARG_BLANK(arg) (*arg ? arg : (g.mLoopFile ? g.mLoopFile->cFileName : ""))
		return FileGetAttrib(const_cast<char*>(USE_FILE_LOOP_FILE_IF_ARG_BLANK(ARG2)));
	case ACT_FILESETATTRIB:
		FileSetAttrib(ARG1, const_cast<char*>(USE_FILE_LOOP_FILE_IF_ARG_BLANK(ARG2)), ConvertLoopMode(ARG3), ATOI(ARG4) == 1);
		return OK; // Always return OK since ErrorLevel will indicate if there was a problem.
	case ACT_FILEGETTIME:
		return FileGetTime(const_cast<char*>(USE_FILE_LOOP_FILE_IF_ARG_BLANK(ARG2)), *ARG3);
	case ACT_FILESETTIME:
		FileSetTime(ARG1, const_cast<char*>(USE_FILE_LOOP_FILE_IF_ARG_BLANK(ARG2)), *ARG3, ConvertLoopMode(ARG4), ATOI(ARG5) == 1);
		return OK; // Always return OK since ErrorLevel will indicate if there was a problem.
	case ACT_FILEGETSIZE:
		return FileGetSize(const_cast<char*>(USE_FILE_LOOP_FILE_IF_ARG_BLANK(ARG2)), ARG3);
	case ACT_FILEGETVERSION:
		return FileGetVersion(const_cast<char*>(USE_FILE_LOOP_FILE_IF_ARG_BLANK(ARG2)));

	case ACT_SETWORKINGDIR:
		SetWorkingDir(ARG1);
		return OK;

	case ACT_FILESELECTFILE:
		return FileSelectFile(ARG2, ARG3, ARG4, ARG5);

	case ACT_FILESELECTFOLDER:
		return FileSelectFolder(ARG2, ARG3, ARG4);

	case ACT_FILEGETSHORTCUT:
		return FileGetShortcut(ARG1);
	case ACT_FILECREATESHORTCUT:
		return FileCreateShortcut(NINE_ARGS);

	case ACT_KEYHISTORY:
#ifdef ENABLE_KEY_HISTORY_FILE
		if (*ARG1 || *ARG2)
		{
			switch (ConvertOnOffToggle(ARG1))
			{
			case NEUTRAL:
			case TOGGLE:
				g_KeyHistoryToFile = !g_KeyHistoryToFile;
				if (!g_KeyHistoryToFile)
					KeyHistoryToFile();  // Signal it to close the file, if it's open.
				break;
			case TOGGLED_ON:
				g_KeyHistoryToFile = true;
				break;
			case TOGGLED_OFF:
				g_KeyHistoryToFile = false;
				KeyHistoryToFile();  // Signal it to close the file, if it's open.
				break;
			// We know it's a variable because otherwise the loading validation would have caught it earlier:
			case TOGGLE_INVALID:
				return LineError(ERR_PARAM1_INVALID, FAIL, ARG1);
			}
			if (*ARG2) // The user also specified a filename, so update the target filename.
				KeyHistoryToFile(ARG2);
			return OK;
		}
#endif
		// Otherwise:
		return ShowMainWindow(MAIN_MODE_KEYHISTORY, false); // Pass "unrestricted" when the command is explicitly used in the script.
	case ACT_LISTLINES:
		return ShowMainWindow(MAIN_MODE_LINES, false); // Pass "unrestricted" when the command is explicitly used in the script.
	case ACT_LISTVARS:
		return ShowMainWindow(MAIN_MODE_VARS, false); // Pass "unrestricted" when the command is explicitly used in the script.
	case ACT_LISTHOTKEYS:
		return ShowMainWindow(MAIN_MODE_HOTKEYS, false); // Pass "unrestricted" when the command is explicitly used in the script.

	case ACT_MSGBOX:
	{
		int result;
		HWND dialog_owner = THREAD_DIALOG_OWNER; // Resolve macro only once to reduce code size.
		// If the MsgBox window can't be displayed for any reason, always return FAIL to
		// the caller because it would be unsafe to proceed with the execution of the
		// current script subroutine.  For example, if the script contains an IfMsgBox after,
		// this line, it's result would be unpredictable and might cause the subroutine to perform
		// the opposite action from what was intended (e.g. Delete vs. don't delete a file).
		if (!mArgc) // When called explicitly with zero params, it displays this default msg.
			result = MsgBox("Press OK to continue.", MSGBOX_NORMAL, NULL, 0, dialog_owner);
		else if (mArgc == 1) // In the special 1-parameter mode, the first param is the prompt.
			result = MsgBox(ARG1, MSGBOX_NORMAL, NULL, 0, dialog_owner);
		else
			result = MsgBox(ARG3, ATOI(ARG1), ARG2, ATOF(ARG4), dialog_owner); // dialog_owner passed via parameter to avoid internally-displayed MsgBoxes from being affected by script-thread's owner setting.
		// Above allows backward compatibility with AutoIt2's param ordering while still
		// permitting the new method of allowing only a single param.
		// v1.0.40.01: Rather than displaying another MsgBox in response to a failed attempt to display
		// a MsgBox, it seems better (less likely to cause trouble) just to abort the thread.  This also
		// solves a double-msgbox issue when the maximum number of MsgBoxes is reached.  In addition, the
		// max-msgbox limit is the most common reason for failure, in which case a warning dialog has
		// already been displayed, so there is no need to display another:
		//if (!result)
		//	// It will fail if the text is too large (say, over 150K or so on XP), but that
		//	// has since been fixed by limiting how much it tries to display.
		//	// If there were too many message boxes displayed, it will already have notified
		//	// the user of this via a final MessageBox dialog, so our call here will
		//	// not have any effect.  The below only takes effect if MsgBox()'s call to
		//	// MessageBox() failed in some unexpected way:
		//	LineError("The MsgBox could not be displayed." ERR_ABORT);
		return result ? OK : FAIL;
	}

	case ACT_INPUTBOX:
		return InputBox(output_var, ARG2, ARG3, toupper(*ARG4) == 'H' // 4th is whether to hide input.
			, *ARG5 ? ATOI(ARG5) : INPUTBOX_DEFAULT  // Width
			, *ARG6 ? ATOI(ARG6) : INPUTBOX_DEFAULT  // Height
			, *ARG7 ? ATOI(ARG7) : INPUTBOX_DEFAULT  // Xpos
			, *ARG8 ? ATOI(ARG8) : INPUTBOX_DEFAULT  // Ypos
			// ARG9: future use for Font name & size, e.g. "Courier:8"
			, ATOF(ARG10)  // Timeout
			, ARG11  // Initial default string for the edit field.
			);

	case ACT_SPLASHTEXTON:
		return SplashTextOn(*ARG1 ? ATOI(ARG1) : 200, *ARG2 ? ATOI(ARG2) : 0, ARG3, ARG4);
	case ACT_SPLASHTEXTOFF:
		DESTROY_SPLASH
		return OK;

	case ACT_PROGRESS:
		return Splash(FIVE_ARGS, "", false);  // ARG6 is for future use and currently not passed.

	case ACT_SPLASHIMAGE:
		return Splash(ARG2, ARG3, ARG4, ARG5, ARG6, ARG1, true);  // ARG7 is for future use and currently not passed.

	case ACT_TOOLTIP:
		return ToolTip(FOUR_ARGS);

	case ACT_TRAYTIP:
		return TrayTip(FOUR_ARGS);

	case ACT_INPUT:
		return Input();


//////////////////////////////////////////////////////////////////////////

	case ACT_COORDMODE:
	{
		bool screen_mode;
		if (!*ARG2 || !stricmp(ARG2, "Screen"))
			screen_mode = true;
		else if (!stricmp(ARG2, "Relative"))
			screen_mode = false;
		else  // Since validated at load-time, too rare to return FAIL for.
			return OK;
		CoordModeAttribType attrib = ConvertCoordModeAttrib(ARG1);
		if (attrib)
		{
			if (screen_mode)
				g.CoordMode |= attrib;
			else
				g.CoordMode &= ~attrib;
		}
		//else too rare to report an error, since load-time validation normally catches it.
		return OK;
	}

	case ACT_SETDEFAULTMOUSESPEED:
		g.DefaultMouseSpeed = (UCHAR)ATOI(ARG1);
		// In case it was a deref, force it to be some default value if it's out of range:
		if (g.DefaultMouseSpeed < 0 || g.DefaultMouseSpeed > MAX_MOUSE_SPEED)
			g.DefaultMouseSpeed = DEFAULT_MOUSE_SPEED;
		return OK;

	case ACT_SENDMODE:
		g.SendMode = ConvertSendMode(ARG1, g.SendMode); // Leave value unchanged if ARG1 is invalid.
		return OK;

	case ACT_SETKEYDELAY:
		if (!stricmp(ARG3, "Play"))
		{
			if (*ARG1)
				g.KeyDelayPlay = ATOI(ARG1);
			if (*ARG2)
				g.PressDurationPlay = ATOI(ARG2);
		}
		else
		{
			if (*ARG1)
				g.KeyDelay = ATOI(ARG1);
			if (*ARG2)
				g.PressDuration = ATOI(ARG2);
		}
		return OK;
	case ACT_SETMOUSEDELAY:
		if (!stricmp(ARG2, "Play"))
			g.MouseDelayPlay = ATOI(ARG1);
		else
			g.MouseDelay = ATOI(ARG1);
		return OK;
	case ACT_SETWINDELAY:
		g.WinDelay = ATOI(ARG1);
		return OK;
	case ACT_SETCONTROLDELAY:
		g.ControlDelay = ATOI(ARG1);
		return OK;

	case ACT_SETBATCHLINES:
		// This below ensures that IntervalBeforeRest and LinesPerCycle aren't both in effect simultaneously
		// (i.e. that both aren't greater than -1), even though ExecUntil() has code to prevent a double-sleep
		// even if that were to happen.
		if (strcasestr(ARG1, "ms")) // This detection isn't perfect, but it doesn't seem necessary to be too demanding.
		{
			g.LinesPerCycle = -1;  // Disable the old BatchLines method in favor of the new one below.
			g.IntervalBeforeRest = ATOI(ARG1);  // If negative, script never rests.  If 0, it rests after every line.
		}
		else
		{
			g.IntervalBeforeRest = -1;  // Disable the new method in favor of the old one below:
			// This value is signed 64-bits to support variable reference (i.e. containing a large int)
			// the user might throw at it:
			if (   !(g.LinesPerCycle = ATOI64(ARG1))   )
				// Don't interpret zero as "infinite" because zero can accidentally
				// occur if the dereferenced var was blank:
				g.LinesPerCycle = 10;  // The old default, which is retained for compatbility with existing scripts.
		}
		return OK;

	case ACT_SETSTORECAPSLOCKMODE:
		if (   (toggle = ConvertOnOff(ARG1, NEUTRAL)) != NEUTRAL   )
			g.StoreCapslockMode = (toggle == TOGGLED_ON);
		return OK;

	case ACT_SETTITLEMATCHMODE:
		switch (ConvertTitleMatchMode(ARG1))
		{
		case FIND_IN_LEADING_PART: g.TitleMatchMode = FIND_IN_LEADING_PART; return OK;
		case FIND_ANYWHERE: g.TitleMatchMode = FIND_ANYWHERE; return OK;
		case FIND_REGEX: g.TitleMatchMode = FIND_REGEX; return OK;
		case FIND_EXACT: g.TitleMatchMode = FIND_EXACT; return OK;
		case FIND_FAST: g.TitleFindFast = true; return OK;
		case FIND_SLOW: g.TitleFindFast = false; return OK;
		}
		return LineError(ERR_TITLEMATCHMODE ERR_ABORT, FAIL, ARG1);

	case ACT_SETFORMAT:
		// For now, it doesn't seem necessary to have runtime validation of the first parameter.
		// Just ignore the command if it's not valid:
		if (!stricmp(ARG1, "Float"))
		{
			// -2 to allow room for the letter 'f' and the '%' that will be added:
			if (ArgLength(2) >= sizeof(g.FormatFloat) - 2) // A variable that resolved to something too long.
				return OK; // Seems best not to bother with a runtime error for something so rare.
			// Make sure the formatted string wouldn't exceed the buffer size:
			__int64 width = ATOI64(ARG2);
			char *dot_pos = strchr(ARG2, '.');
			__int64 precision = dot_pos ? ATOI64(dot_pos + 1) : 0;
			if (width + precision + 2 > MAX_FORMATTED_NUMBER_LENGTH) // +2 to allow room for decimal point itself and leading minus sign.
				return OK; // Don't change it.
			// Create as "%ARG2f".  Note that %f can handle doubles in MSVC++:
			sprintf(g.FormatFloat, "%%%s%s%s", ARG2
				, dot_pos ? "" : "." // Add a dot if none was specified so that "0" is the same as "0.", which seems like the most user-friendly approach; it's also easier to document in the help file.
				, IsPureNumeric(ARG2, true, true, true) ? "f" : ""); // If it's not pure numeric, assume the user already included the desired letter (e.g. SetFormat, Float, 0.6e).
		}
		else if (!stricmp(ARG1, "Integer"))
		{
			switch(*ARG2)
			{
			case 'd':
			case 'D':
				g.FormatIntAsHex = false;
				break;
			case 'h':
			case 'H':
				g.FormatIntAsHex = true;
				break;
			// Otherwise, since the first letter isn't recongized, do nothing since 99% of the time such a
			// probably would be caught at load-time.
			}
		}
		// Otherwise, ignore invalid type at runtime since 99% of the time it would be caught at load-time:
		return OK;

	case ACT_FORMATTIME:
		return FormatTime(ARG2, ARG3);

	case ACT_MENU:
		return g_script.PerformMenu(FIVE_ARGS);

	case ACT_GUI:
		return g_script.PerformGui(FOUR_ARGS);

	case ACT_GUICONTROL:
		return GuiControl(THREE_ARGS);

	case ACT_GUICONTROLGET:
		return GuiControlGet(ARG2, ARG3, ARG4);

	////////////////////////////////////////////////////////////////////////////////////////
	// For these, it seems best not to report an error during runtime if there's
	// an invalid value (e.g. something other than On/Off/Blank) in a param containing
	// a dereferenced variable, since settings are global and affect all subroutines,
	// not just the one that we would otherwise report failure for:
	case ACT_SUSPEND:
		switch (ConvertOnOffTogglePermit(ARG1))
		{
		case NEUTRAL:
		case TOGGLE:
			ToggleSuspendState();
			break;
		case TOGGLED_ON:
			if (!g_IsSuspended)
				ToggleSuspendState();
			break;
		case TOGGLED_OFF:
			if (g_IsSuspended)
				ToggleSuspendState();
			break;
		case TOGGLE_PERMIT:
			// In this case do nothing.  The user is just using this command as a flag to indicate that
			// this subroutine should not be suspended.
			break;
		// We know it's a variable because otherwise the loading validation would have caught it earlier:
		case TOGGLE_INVALID:
			return LineError(ERR_PARAM1_INVALID, FAIL, ARG1);
		}
		return OK;
	case ACT_PAUSE:
		return ChangePauseState(ConvertOnOffToggle(ARG1), (bool)ATOI(ARG2));
	case ACT_AUTOTRIM:
		if (   (toggle = ConvertOnOff(ARG1, NEUTRAL)) != NEUTRAL   )
			g.AutoTrim = (toggle == TOGGLED_ON);
		return OK;
	case ACT_STRINGCASESENSE:
		if ((g.StringCaseSense = ConvertStringCaseSense(ARG1)) == SCS_INVALID)
			g.StringCaseSense = SCS_INSENSITIVE; // For simplicity, just fall back to default if value is invalid (normally its caught at load-time; only rarely here).
		return OK;
	case ACT_DETECTHIDDENWINDOWS:
		if (   (toggle = ConvertOnOff(ARG1, NEUTRAL)) != NEUTRAL   )
			g.DetectHiddenWindows = (toggle == TOGGLED_ON);
		return OK;
	case ACT_DETECTHIDDENTEXT:
		if (   (toggle = ConvertOnOff(ARG1, NEUTRAL)) != NEUTRAL   )
			g.DetectHiddenText = (toggle == TOGGLED_ON);
		return OK;
	case ACT_BLOCKINPUT:
		switch (toggle = ConvertBlockInput(ARG1))
		{
		case TOGGLED_ON:
			ScriptBlockInput(true);
			break;
		case TOGGLED_OFF:
			ScriptBlockInput(false);
			break;
		case TOGGLE_SEND:
		case TOGGLE_MOUSE:
		case TOGGLE_SENDANDMOUSE:
		case TOGGLE_DEFAULT:
			g_BlockInputMode = toggle;
			break;
		case TOGGLE_MOUSEMOVE:
			g_BlockMouseMove = true;
			Hotkey::InstallMouseHook();
			break;
		case TOGGLE_MOUSEMOVEOFF:
			g_BlockMouseMove = false; // But the mouse hook is left installed because it might be needed by other things. This approach is similar to that used by the Input command.
			break;
		// default (NEUTRAL or TOGGLE_INVALID): do nothing.
		}
		return OK;

	////////////////////////////////////////////////////////////////////////////////////////
	// For these, it seems best not to report an error during runtime if there's
	// an invalid value (e.g. something other than On/Off/Blank) in a param containing
	// a dereferenced variable, since settings are global and affect all subroutines,
	// not just the one that we would otherwise report failure for:
	case ACT_SETNUMLOCKSTATE:
		return SetToggleState(VK_NUMLOCK, g_ForceNumLock, ARG1);
	case ACT_SETCAPSLOCKSTATE:
		return SetToggleState(VK_CAPITAL, g_ForceCapsLock, ARG1);
	case ACT_SETSCROLLLOCKSTATE:
		return SetToggleState(VK_SCROLL, g_ForceScrollLock, ARG1);

	case ACT_EDIT:
		g_script.Edit();
		return OK;
	case ACT_RELOAD:
		g_script.Reload(true);
		// Even if the reload failed, it seems best to return OK anyway.  That way,
		// the script can take some follow-on action, e.g. it can sleep for 1000
		// after issuing the reload command and then take action under the assumption
		// that the reload didn't work (since obviously if the process and thread
		// in which the Sleep is running still exist, it didn't work):
		return OK;

	case ACT_SLEEP:
	{
		// Only support 32-bit values for this command, since it seems unlikely anyone would to have
		// it sleep more than 24.8 days or so.  It also helps performance on 32-bit hardware because
		// MsgSleep() is so heavily called and checks the value of the first parameter frequently:
		int sleep_time = ATOI(ARG1); // Keep it signed vs. unsigned for backward compatibility (e.g. scripts that do Sleep -1).

		// Do a true sleep on Win9x because the MsgSleep() method is very inaccurate on Win9x
		// for some reason (a MsgSleep(1) causes a sleep between 10 and 55ms, for example).
		// But only do so for short sleeps, for which the user has a greater expectation of
		// accuracy.  UPDATE: Do not change the 25 below without also changing it in Critical's
		// documentation.
		if (sleep_time < 25 && sleep_time > 0 && g_os.IsWin9x()) // Ordered for short-circuit performance. v1.0.38.05: Added "sleep_time > 0" so that Sleep -1/0 will work the same on Win9x as it does on other OSes.
			Sleep(sleep_time);
		else
			MsgSleep(sleep_time);
		return OK;
	}

	case ACT_INIREAD:
		return IniRead(ARG2, ARG3, ARG4, ARG5);
	case ACT_INIWRITE:
		return IniWrite(FOUR_ARGS);
	case ACT_INIDELETE:
		// To preserve maximum compatibility with existing scripts, only send NULL if ARG3
		// was explicitly omitted.  This is because some older scripts might rely on the
		// fact that a blank ARG3 does not delete the entire section, but rather does
		// nothing (that fact is untested):
		return IniDelete(ARG1, ARG2, mArgc < 3 ? NULL : ARG3);

	case ACT_REGREAD:
		if (mArgc < 2 && g.mLoopRegItem) // Uses the registry loop's current item.
			// If g.mLoopRegItem->name specifies a subkey rather than a value name, do this anyway
			// so that it will set ErrorLevel to ERROR and set the output variable to be blank.
			// Also, do not use RegCloseKey() on this, even if it's a remote key, since our caller handles that:
			return RegRead(g.mLoopRegItem->root_key, g.mLoopRegItem->subkey, g.mLoopRegItem->name);
		// Otherwise:
		if (mArgc > 4 || RegConvertValueType(ARG2)) // The obsolete 5-param method (ARG2 is unused).
			result = RegRead(root_key = RegConvertRootKey(ARG3, &is_remote_registry), ARG4, ARG5);
		else
			result = RegRead(root_key = RegConvertRootKey(ARG2, &is_remote_registry), ARG3, ARG4);
		if (is_remote_registry && root_key) // Never try to close local root keys, which the OS keeps always-open.
			RegCloseKey(root_key);
		return result;
	case ACT_REGWRITE:
		if (mArgc < 2 && g.mLoopRegItem) // Uses the registry loop's current item.
			// If g.mLoopRegItem->name specifies a subkey rather than a value name, do this anyway
			// so that it will set ErrorLevel to ERROR.  An error will also be indicated if
			// g.mLoopRegItem->type is an unsupported type:
			return RegWrite(g.mLoopRegItem->type, g.mLoopRegItem->root_key, g.mLoopRegItem->subkey, g.mLoopRegItem->name, ARG1);
		// Otherwise:
		result = RegWrite(RegConvertValueType(ARG1), root_key = RegConvertRootKey(ARG2, &is_remote_registry)
			, ARG3, ARG4, ARG5); // If RegConvertValueType(ARG1) yields REG_NONE, RegWrite() will set ErrorLevel rather than displaying a runtime error.
		if (is_remote_registry && root_key) // Never try to close local root keys, which the OS keeps always-open.
			RegCloseKey(root_key);
		return result;
	case ACT_REGDELETE:
		if (mArgc < 1 && g.mLoopRegItem) // Uses the registry loop's current item.
		{
			// In this case, if the current reg item is a value, just delete it normally.
			// But if it's a subkey, append it to the dir name so that the proper subkey
			// will be deleted as the user intended:
			if (g.mLoopRegItem->type == REG_SUBKEY)
			{
				snprintf(buf_temp, sizeof(buf_temp), "%s\\%s", g.mLoopRegItem->subkey, g.mLoopRegItem->name);
				return RegDelete(g.mLoopRegItem->root_key, buf_temp, "");
			}
			else
				return RegDelete(g.mLoopRegItem->root_key, g.mLoopRegItem->subkey, g.mLoopRegItem->name);
		}
		// Otherwise:
		result = RegDelete(root_key = RegConvertRootKey(ARG1, &is_remote_registry), ARG2, ARG3);
		if (is_remote_registry && root_key) // Never try to close local root keys, which the OS always keeps open.
			RegCloseKey(root_key);
		return result;

	case ACT_OUTPUTDEBUG:
		OutputDebugString(ARG1); // It does not return a value for the purpose of setting ErrorLevel.
		return OK;

	case ACT_SHUTDOWN:
		return Util_Shutdown(ATOI(ARG1)) ? OK : FAIL; // Range of ARG1 is not validated in case other values are supported in the future.
	} // switch()

	// Since above didn't return, this line's mActionType isn't handled here,
	// so caller called it wrong.  ACT_INVALID should be impossible because
	// Script::AddLine() forbids it.

#ifdef _DEBUG
	return LineError("DEBUG: Perform(): Unhandled action type." ERR_ABORT);
#else
	return FAIL;
#endif
}



ResultType Line::Deref(Var *aOutputVar, char *aBuf)
// Similar to ExpandArg(), except it parses and expands all variable references contained in aBuf.
{
	aOutputVar = aOutputVar->ResolveAlias(); // Necessary for proper detection below of whether it's invalidly used as a source for itself.

	// This transient variable is used resolving environment variables that don't already exist
	// in the script's variable list (due to the fact that they aren't directly referenced elsewhere
	// in the script):
	char var_name[MAX_VAR_NAME_LENGTH + 1] = "";
	Var temp_var(var_name, (void *)VAR_NORMAL, false);

	Var *var;
	VarSizeType expanded_length;
	size_t var_name_length;
	char *cp, *cp1, *dest;

	// Do two passes:
	// #1: Calculate the space needed so that aOutputVar can be given more capacity if necessary.
	// #2: Expand the contents of aBuf into aOutputVar.

	for (int which_pass = 0; which_pass < 2; ++which_pass)
	{
		if (which_pass) // Starting second pass.
		{
			// Set up aOutputVar, enlarging it if necessary.  If it is of type VAR_CLIPBOARD,
			// this call will set up the clipboard for writing:
			if (aOutputVar->Assign(NULL, expanded_length) != OK)
				return FAIL;
			dest = aOutputVar->Contents();  // Init, and for performance.
		}
		else // First pass.
			expanded_length = 0; // Init prior to accumulation.

		for (cp = aBuf; ; ++cp)  // Increment to skip over the deref/escape just found by the inner for().
		{
			// Find the next escape char or deref symbol:
			for (; *cp && *cp != g_EscapeChar && *cp != g_DerefChar; ++cp)
			{
				if (which_pass) // 2nd pass
					*dest++ = *cp;  // Copy all non-variable-ref characters literally.
				else // just accumulate the length
					++expanded_length;
			}
			if (!*cp) // End of string while scanning/copying.  The current pass is now complete.
				break;
			if (*cp == g_EscapeChar)
			{
				if (which_pass) // 2nd pass
				{
					cp1 = cp + 1;
					switch (*cp1) // See ConvertEscapeSequences() for more details.
					{
						// Only lowercase is recognized for these:
						case 'a': *dest = '\a'; break;  // alert (bell) character
						case 'b': *dest = '\b'; break;  // backspace
						case 'f': *dest = '\f'; break;  // formfeed
						case 'n': *dest = '\n'; break;  // newline
						case 'r': *dest = '\r'; break;  // carriage return
						case 't': *dest = '\t'; break;  // horizontal tab
						case 'v': *dest = '\v'; break;  // vertical tab
						default:  *dest = *cp1; // These other characters are resolved just as they are, including '\0'.
					}
					++dest;
				}
				else
					++expanded_length;
				// Increment cp here and it will be incremented again by the outer loop, i.e. +2.
				// In other words, skip over the escape character, treating it and its target character
				// as a single character.
				++cp;
				continue;
			}
			// Otherwise, it's a dereference symbol, so calculate the size of that variable's contents
			// and add that to expanded_length (or copy the contents into aOutputVar if this is the
			// second pass).
			// Find the reference's ending symbol (don't bother with catching escaped deref chars here
			// -- e.g. %MyVar`% --  since it seems too troublesome to justify given how extremely rarely
			// it would be an issue):
			for (cp1 = cp + 1; *cp1 && *cp1 != g_DerefChar; ++cp1);
			if (!*cp1)    // Since end of string was found, this deref is not correctly terminated.
				continue; // For consistency, omit it entirely.
			var_name_length = cp1 - cp - 1;
			if (var_name_length && var_name_length <= MAX_VAR_NAME_LENGTH)
			{
				strlcpy(var_name, cp + 1, var_name_length + 1);  // +1 to convert var_name_length to size.
				// The use of ALWAYS_PREFER_LOCAL below improves flexibility of assume-global functions
				// by allowing this command to resolve to a local first if such a local exists.
				// Fixed for v1.0.34: Use FindOrAddVar() vs. FindVar() so that environment or built-in
				// variables that aren't directly referenced elsewhere in the script will still work:
				if (   !(var = g_script.FindOrAddVar(var_name, var_name_length, ALWAYS_PREFER_LOCAL))   )
					// Variable doesn't exist, but since it might be an environment variable never referenced
					// directly elsewhere in the script, do special handling:
					var = &temp_var;  // Relies on the fact that var_temp.mName *is* the var_name pointer.
				else
					var = var->ResolveAlias(); // This was already done (above) for aOutputVar.
				// Don't allow the output variable to be read into itself this way because its contents
				if (var != aOutputVar) // Both of these have had ResolveAlias() called, if required, to make the comparison accurate.
				{
					if (which_pass) // 2nd pass
						dest += var->Get(dest);
					else // just accumulate the length
						expanded_length += var->Get(); // Add in the length of the variable's contents.
				}
			}
			// else since the variable name between the deref symbols is blank or too long: for consistency in behavior,
			// it seems best to omit the dereference entirely (don't put it into aOutputVar).
			cp = cp1; // For the next loop iteration, continue at the char after this reference's final deref symbol.
		} // for()
	} // for() (first and second passes)

	*dest = '\0';  // Terminate the output variable.
	aOutputVar->Length() = (VarSizeType)strlen(aOutputVar->Contents()); // Update to actual in case estimate was too large.
	return aOutputVar->Close();  // In case it's the clipboard.
}



char *Line::LogToText(char *aBuf, int aBufSize) // aBufSize should be an int to preserve negatives from caller (caller relies on this).
// aBufSize is an int so that any negative values passed in from caller are not lost.
// Translates sLog into its text equivalent, putting the result into aBuf and
// returning the position in aBuf of its new string terminator.
// Caller has ensured that aBuf is non-NULL and that aBufSize is reasonable (at least 256).
{
	char *aBuf_orig = aBuf;

	// Store the position of where each retry done by the outer loop will start writing:
	char *aBuf_log_start = aBuf + snprintf(aBuf, aBufSize, "Script lines most recently executed (oldest first)."
		"  Press [F5] to refresh.  The seconds elapsed between a line and the one after it is in parentheses to"
		" the right (if not 0).  The bottommost line's elapsed time is the number of seconds since it executed.\r\n\r\n");

	int i, lines_to_show, line_index, line_index2, space_remaining; // space_remaining must be an int to detect negatives.
	DWORD elapsed;
	bool this_item_is_special, next_item_is_special;

	// In the below, sLogNext causes it to start at the oldest logged line and continue up through the newest:
	for (lines_to_show = LINE_LOG_SIZE, line_index = sLogNext;;) // Retry with fewer lines in case the first attempt doesn't fit in the buffer.
	{
		aBuf = aBuf_log_start; // Reset target position in buffer to the place where log should begin.
		for (next_item_is_special = false, i = 0; i < lines_to_show; ++i, ++line_index)
		{
			if (line_index >= LINE_LOG_SIZE) // wrap around, because sLog is a circular queue
				line_index -= LINE_LOG_SIZE; // Don't just reset it to zero because an offset larger than one may have been added to it.
			if (!sLog[line_index]) // No line has yet been logged in this slot.
				continue;
			this_item_is_special = next_item_is_special;
			next_item_is_special = false;  // Set default.
			if (i + 1 < lines_to_show)  // There are still more lines to be processed
			{
				if (this_item_is_special) // And we know from the above that this special line is not the last line.
					// Due to the fact that these special lines are usually only useful when they appear at the
					// very end of the log, omit them from the log-display when they're not the last line.
					// In the case of a high-frequency SetTimer, this greatly reduces the log clutter that
					// would otherwise occur:
					continue;

				// Since above didn't continue, this item isn't special, so display it normally.
				elapsed = sLogTick[line_index + 1 >= LINE_LOG_SIZE ? 0 : line_index + 1] - sLogTick[line_index];
				if (elapsed > INT_MAX) // INT_MAX is about one-half of DWORD's capacity.
				{
					// v1.0.30.02: Assume that huge values (greater than 24 days or so) were caused by
					// the new policy of storing WinWait/RunWait/etc.'s line in the buffer whenever
					// it was interrupted and later resumed by a thread.  In other words, there are now
					// extra lines in the buffer which are considered "special" because they don't indicate
					// a line that actually executed, but rather one that is still executing (waiting).
					// See ACT_WINWAIT for details.
					next_item_is_special = true; // Override the default.
					if (i + 2 == lines_to_show) // The line after this one is not only special, but the last one that will be shown, so recalculate this one correctly.
						elapsed = GetTickCount() - sLogTick[line_index];
					else // Neither this line nor the special one that follows it is the last.
					{
						// Refer to the line after the next (special) line to get this line's correct elapsed time.
						line_index2 = line_index + 2;
						if (line_index2 >= LINE_LOG_SIZE)
							line_index2 -= LINE_LOG_SIZE;
						elapsed = sLogTick[line_index2] - sLogTick[line_index];
					}
				}
			}
			else // This is the last line (whether special or not), so compare it's time against the current time instead.
				elapsed = GetTickCount() - sLogTick[line_index];
			space_remaining = BUF_SPACE_REMAINING;  // Resolve macro only once for performance.
			// Truncate really huge lines so that the Edit control's size is less likely to be exhausted.
			// In v1.0.30.02, this is even more likely due to having increased the line-buf's capacity from
			// 200 to 400, therefore the truncation point was reduced from 500 to 200 to make it more likely
			// that the first attempt to fit the lines_to_show number of lines into the buffer will succeed.
			aBuf = sLog[line_index]->ToText(aBuf, space_remaining < 200 ? space_remaining : 200, true, elapsed, this_item_is_special);
			// If the line above can't fit everything it needs into the remaining space, it will fill all
			// of the remaining space, and thus the check against LINE_LOG_FINAL_MESSAGE_LENGTH below
			// should never fail to catch that, and then do a retry.
		} // Inner for()

		#define LINE_LOG_FINAL_MESSAGE "\r\nPress [F5] to refresh." // Keep the next line in sync with this.
		#define LINE_LOG_FINAL_MESSAGE_LENGTH 24
		if (BUF_SPACE_REMAINING > LINE_LOG_FINAL_MESSAGE_LENGTH || lines_to_show < 120) // Either success or can't succeed.
			break;

		// Otherwise, there is insufficient room to put everything in, but there's still room to retry
		// with a smaller value of lines_to_show:
		lines_to_show -= 100;
		line_index = sLogNext + (LINE_LOG_SIZE - lines_to_show); // Move the starting point forward in time so that the oldest log entries are omitted.

	} // outer for() that retries the log-to-buffer routine.

	// Must add the return value, not LINE_LOG_FINAL_MESSAGE_LENGTH, in case insufficient room (i.e. in case
	// outer loop terminated due to lines_to_show being too small).
	return aBuf + snprintf(aBuf, BUF_SPACE_REMAINING, LINE_LOG_FINAL_MESSAGE);
}



char *Line::VicinityToText(char *aBuf, int aBufSize) // aBufSize should be an int to preserve negatives from caller (caller relies on this).
// aBufSize is an int so that any negative values passed in from caller are not lost.
// Caller has ensured that aBuf isn't NULL.
// Translates the current line and the lines above and below it into their text equivalent
// putting the result into aBuf and returning the position in aBuf of its new string terminator.
{
	char *aBuf_orig = aBuf;

	#define LINES_ABOVE_AND_BELOW 7

	// Determine the correct value for line_start and line_end:
	int i;
	Line *line_start, *line_end;
	for (i = 0, line_start = this
		; i < LINES_ABOVE_AND_BELOW && line_start->mPrevLine != NULL
		; ++i, line_start = line_start->mPrevLine);

	for (i = 0, line_end = this
		; i < LINES_ABOVE_AND_BELOW && line_end->mNextLine != NULL
		; ++i, line_end = line_end->mNextLine);

#ifdef AUTOHOTKEYSC
	if (!g_AllowMainWindow) // Override the above to show only a single line, to conceal the script's source code.
	{
		line_start = this;
		line_end = this;
	}
#endif

	// Now line_start and line_end are the first and last lines of the range
	// we want to convert to text, and they're non-NULL.
	aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "\tLine#\n");

	int space_remaining; // Must be an int to preserve any negative results.

	// Start at the oldest and continue up through the newest:
	for (Line *line = line_start;;)
	{
		if (line == this)
			strlcpy(aBuf, "--->\t", BUF_SPACE_REMAINING);
		else
			strlcpy(aBuf, "\t", BUF_SPACE_REMAINING);
		aBuf += strlen(aBuf);
		space_remaining = BUF_SPACE_REMAINING;  // Resolve macro only once for performance.
		// Truncate large lines so that the dialog is more readable:
		aBuf = line->ToText(aBuf, space_remaining < 500 ? space_remaining : 500, false);
		if (line == line_end)
			break;
		line = line->mNextLine;
	}
	return aBuf;
}



char *Line::ToText(char *aBuf, int aBufSize, bool aCRLF, DWORD aElapsed, bool aLineWasResumed) // aBufSize should be an int to preserve negatives from caller (caller relies on this).
// aBufSize is an int so that any negative values passed in from caller are not lost.
// Caller has ensured that aBuf isn't NULL.
// Translates this line into its text equivalent, putting the result into aBuf and
// returning the position in aBuf of its new string terminator.
{
	if (aBufSize < 3)
		return aBuf;
	else
		aBufSize -= (1 + aCRLF);  // Reserve one char for LF/CRLF after each line (so that it always get added).

	char *aBuf_orig = aBuf;

	aBuf += snprintf(aBuf, aBufSize, "%03u: ", mLineNumber);
	if (aLineWasResumed)
		aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "STILL WAITING (%0.2f): ", (float)aElapsed / 1000.0);

	if (mActionType == ACT_IFBETWEEN || mActionType == ACT_IFNOTBETWEEN)
		aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "if %s %s %s and %s"
			, *mArg[0].text ? mArg[0].text : VAR(mArg[0])->mName  // i.e. don't resolve dynamic variable names.
			, g_act[mActionType].Name, RAW_ARG2, RAW_ARG3);
	else if (ACT_IS_ASSIGN(mActionType) || (ACT_IS_IF(mActionType) && mActionType < ACT_FIRST_COMMAND))
		// Only these other commands need custom conversion.
		aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "%s%s %s %s"
			, ACT_IS_IF(mActionType) ? "if " : ""
			, *mArg[0].text ? mArg[0].text : VAR(mArg[0])->mName  // i.e. don't resolve dynamic variable names.
			, g_act[mActionType].Name, RAW_ARG2);
	else
	{
		aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "%s", g_act[mActionType].Name);
		for (int i = 0; i < mArgc; ++i)
			// This method a little more efficient than using snprintfcat().
			// Also, always use the arg's text for input and output args whose variables haven't
			// been been resolved at load-time, since the text has everything in it we want to display
			// and thus there's no need to "resolve" dynamic variables here (e.g. array%i%).
			aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, ",%s", (mArg[i].type != ARG_TYPE_NORMAL && !*mArg[i].text)
				? VAR(mArg[i])->mName : mArg[i].text);
	}
	if (aElapsed && !aLineWasResumed)
		aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, " (%0.2f)", (float)aElapsed / 1000.0);
	// UPDATE for v1.0.25: It seems that MessageBox(), which is the only way these lines are currently
	// displayed, prefers \n over \r\n because otherwise, Ctrl-C on the MsgBox copies the lines all
	// onto one line rather than formatted nicely as separate lines.
	// Room for LF or CRLF was reserved at the top of this function:
	if (aCRLF)
		*aBuf++ = '\r';
	*aBuf++ = '\n';
	*aBuf = '\0';
	return aBuf;
}



void Line::ToggleSuspendState()
{
	// If suspension is being turned on:
	// It seems unnecessary, and possibly undesirable, to purge any pending hotkey msgs from the msg queue.
	// Even if there are some, it's possible that they are exempt from suspension so we wouldn't want to
	// globally purge all messages anyway.
	g_IsSuspended = !g_IsSuspended;
	Hotstring::SuspendAll(g_IsSuspended);  // Must do this prior to ManifestAllHotkeysHotstringsHooks() to avoid incorrect removal of hook.
	Hotkey::ManifestAllHotkeysHotstringsHooks(); // Update the state of all hotkeys based on the complex interdependencies hotkeys have with each another.
	g_script.UpdateTrayIcon();
	CheckMenuItem(GetMenu(g_hWnd), ID_FILE_SUSPEND, g_IsSuspended ? MF_CHECKED : MF_UNCHECKED);
}



ResultType Line::ChangePauseState(ToggleValueType aChangeTo, bool aAlwaysOperateOnUnderlyingThread)
// Returns OK or FAIL.
// Note: g_Idle must be false since we're always called from a script subroutine, not from
// the tray menu.  Therefore, the value of g_Idle need never be checked here.
{
	switch (aChangeTo)
	{
	case TOGGLED_ON:
		break; // By breaking insteading of returning, pause will be put into effect further below.
	case TOGGLED_OFF:
		// v1.0.37.06: The old method was to unpause the the nearest paused thread on the call stack;
		// but it was flawed because if the thread that made the flag true is interrupted, and the new
		// thread is paused via the pause command, and that thread is then interrupted, when the paused
		// thread resumes it would automatically and wrongly be unpaused (i.e. the unpause ticket would
		// be used at a level higher in the call stack than intended).
		// Flag this thread so that when it ends, the thread beneath it will be unpaused.  If that thread
		// (which can be the idle thread) isn't paused the following flag-change will be ignored at a later
		// stage. This method also relies on the fact that the current thread cannot itself be paused right
		// now because it is what got us here.
		g.UnderlyingThreadIsPaused = false; // Necessary even for the "idle thread" (otherwise, the Pause command wouldn't be able to unpause it).
		return OK;
	case NEUTRAL: // the user omitted the parameter entirely, which is considered the same as "toggle"
	case TOGGLE:
		// Update for v1.0.37.06: "Pause" and "Pause Toggle" are more useful if they always apply to the
		// thread immediately beneath the current thread rather than "any underlying thread that's paused".
		if (g.UnderlyingThreadIsPaused)
		{
			g.UnderlyingThreadIsPaused = false; // Flag it to be unpaused when it gets resumed.
			return OK;
		}
		// Otherwise, since the underlying thread is not paused, continue onward to do the "pause enabled"
		// logic below:
		break;
	default: // TOGGLE_INVALID or some other disallowed value.
		// We know it's a variable because otherwise the loading validation would have caught it earlier:
		return LineError(ERR_PARAM1_INVALID, FAIL, ARG1);
	}

	// Since above didn't return, pause should be turned on.
	if (aAlwaysOperateOnUnderlyingThread) // v1.0.37.06: Allow underlying thread to be directly paused rather than pausing the current thread.
	{
		g.UnderlyingThreadIsPaused = true; // If the underlying thread is already paused, this flag change will be ignored at a later stage.
		return OK;
	}
	// Otherwise, pause the current subroutine (which by definition isn't paused since it had to be
	// active to call us).  It seems best not to attempt to change the Hotkey mRunAgainAfterFinished
	// attribute for the current hotkey (assuming it's even a hotkey that got us here) or
	// for them all.  This is because it's conceivable that this Pause command occurred
	// in a background thread, such as a timed subroutine, in which case we wouldn't want the
	// pausing of that thread to affect anything else the user might be doing with hotkeys.
	// UPDATE: The above is flawed because by definition the script's quasi-thread that got
	// us here is now active.  Since it is active, the script will immediately become dormant
	// when this is executed, waiting for the user to press a hotkey to launch a new
	// quasi-thread.  Thus, it seems best to reset all the mRunAgainAfterFinished flags
	// in case we are in a hotkey subroutine and in case this hotkey has a buffered repeat-again
	// action pending, which the user probably wouldn't want to happen after the script is unpaused:
	Hotkey::ResetRunAgainAfterFinished();
	g.IsPaused = true;
	++g_nPausedThreads; // Always incremented because we're never called to pause the "idle thread", only real threads.
	g_script.UpdateTrayIcon();
	CheckMenuItem(GetMenu(g_hWnd), ID_FILE_PAUSE, MF_CHECKED);
	return OK;
}



ResultType Line::ScriptBlockInput(bool aEnable)
// Always returns OK for caller convenience.
{
	// Must be running Win98/2000+ for this function to be successful.
	// We must dynamically load the function to retain compatibility with Win95 (program won't launch
	// at all otherwise).
	typedef void (CALLBACK *BlockInput)(BOOL);
	static BlockInput lpfnDLLProc = (BlockInput)GetProcAddress(GetModuleHandle("user32"), "BlockInput");
	// Always turn input ON/OFF even if g_BlockInput says its already in the right state.  This is because
	// BlockInput can be externally and undetectibly disabled, e.g. if the user presses Ctrl-Alt-Del:
	if (lpfnDLLProc)
		(*lpfnDLLProc)(aEnable ? TRUE : FALSE);
	g_BlockInput = aEnable;
	return OK;  // By design, it never returns FAIL.
}



Line *Line::PreparseError(char *aErrorText, char *aExtraInfo)
// Returns a different type of result for use with the Pre-parsing methods.
{
	// Make all preparsing errors critical because the runtime reliability
	// of the program relies upon the fact that the aren't any kind of
	// problems in the script (otherwise, unexpected behavior may result).
	// Update: It's okay to return FAIL in this case.  CRITICAL_ERROR should
	// be avoided whenever OK and FAIL are sufficient by themselves, because
	// otherwise, callers can't use the NOT operator to detect if a function
	// failed (since FAIL is value zero, but CRITICAL_ERROR is non-zero):
	LineError(aErrorText, FAIL, aExtraInfo);
	return NULL; // Always return NULL because the callers use it as their return value.
}



ResultType Line::LineError(char *aErrorText, ResultType aErrorType, char *aExtraInfo)
{
	if (!aErrorText)
		aErrorText = "";
	if (!aExtraInfo)
		aExtraInfo = "";

	if (g_script.mErrorStdOut && !g_script.mIsReadyToExecute) // i.e. runtime errors are always displayed via dialog.
	{
		// JdeB said:
		// Just tested it in Textpad, Crimson and Scite. they all recognise the output and jump
		// to the Line containing the error when you double click the error line in the output
		// window (like it works in C++).  Had to change the format of the line to:
		// printf("%s (%d) : ==> %s: \n%s \n%s\n",szInclude, nAutScriptLine, szText, szScriptLine, szOutput2 );
		// MY: Full filename is required, even if it's the main file, because some editors (EditPlus)
		// seem to rely on that to determine which file and line number to jump to when the user double-clicks
		// the error message in the output window.
		// v1.0.47: Added a space before the colon as originally intended.  Toralf said, "With this minor
		// change the error lexer of Scite recognizes this line as a Microsoft error message and it can be
		// used to jump to that line."
		#define STD_ERROR_FORMAT "%s (%d) : ==> %s\n"
		printf(STD_ERROR_FORMAT, sSourceFile[mFileIndex], mLineNumber, aErrorText); // printf() does not signifantly increase the size of the EXE, probably because it shares most of the same code with sprintf(), etc.
		if (*aExtraInfo)
			printf("     Specifically: %s\n", aExtraInfo);
	}
	else
	{
		char source_file[MAX_PATH * 2];
		if (mFileIndex)
			snprintf(source_file, sizeof(source_file), " in #include file \"%s\"", sSourceFile[mFileIndex]);
		else
			*source_file = '\0'; // Don't bother cluttering the display if it's the main script file.

		char buf[MSGBOX_TEXT_SIZE];
		char *buf_marker = buf + snprintf(buf, sizeof(buf), "%s%s: %-1.500s\n\n"  // Keep it to a sane size in case it's huge.
			, aErrorType == WARN ? "Warning" : (aErrorType == CRITICAL_ERROR ? "Critical Error" : "Error")
			, source_file, aErrorText);
		if (*aExtraInfo)
			// Use format specifier to make sure really huge strings that get passed our
			// way, such as a var containing clipboard text, are kept to a reasonable size:
			buf_marker += snprintfcat(buf, sizeof(buf), "Specifically: %-1.100s%s\n\n"
			, aExtraInfo, strlen(aExtraInfo) > 100 ? "..." : "");
		buf_marker = VicinityToText(buf_marker, (int)(sizeof(buf) - (buf_marker - buf))); // Cast to int to avoid loss of negative values.
		if (aErrorType == CRITICAL_ERROR || (aErrorType == FAIL && !g_script.mIsReadyToExecute))
			strlcpy(buf_marker, g_script.mIsRestart ? ("\n" OLD_STILL_IN_EFFECT) : ("\n" WILL_EXIT)
				, (int)(sizeof(buf) - (buf_marker - buf))); // Cast to int to avoid loss of negative values.
		g_script.mCurrLine = this;  // This needs to be set in some cases where the caller didn't.
		//g_script.ShowInEditor();
		MsgBox(buf);
	}

	if (aErrorType == CRITICAL_ERROR && g_script.mIsReadyToExecute)
		// Also ask the main message loop function to quit and announce to the system that
		// we expect it to quit.  In most cases, this is unnecessary because all functions
		// called to get to this point will take note of the CRITICAL_ERROR and thus keep
		// return immediately, all the way back to main.  However, there may cases
		// when this isn't true:
		// Note: Must do this only after MsgBox, since it appears that new dialogs can't
		// be created once it's done.  Update: Using ExitApp() now, since it's known to be
		// more reliable:
		//PostQuitMessage(CRITICAL_ERROR);
		// This will attempt to run the OnExit subroutine, which should be okay since that subroutine
		// will terminate the script if it encounters another runtime error:
		g_script.ExitApp(EXIT_ERROR);

	return aErrorType; // The caller told us whether it should be a critical error or not.
}



ResultType Script::ScriptError(char *aErrorText, char *aExtraInfo) //, ResultType aErrorType)
// Even though this is a Script method, including it here since it shares
// a common theme with the other error-displaying functions:
{
	if (mCurrLine)
		// If a line is available, do LineError instead since it's more specific.
		// If an error occurs before the script is ready to run, assume it's always critical
		// in the sense that the program will exit rather than run the script.
		// Update: It's okay to return FAIL in this case.  CRITICAL_ERROR should
		// be avoided whenever OK and FAIL are sufficient by themselves, because
		// otherwise, callers can't use the NOT operator to detect if a function
		// failed (since FAIL is value zero, but CRITICAL_ERROR is non-zero):
		return mCurrLine->LineError(aErrorText, FAIL, aExtraInfo);
	// Otherwise: The fact that mCurrLine is NULL means that the line currently being loaded
	// has not yet been successfully added to the linked list.  Such errors will always result
	// in the program exiting.
	if (!aErrorText)
		aErrorText = "Unk"; // Placeholder since it shouldn't be NULL.
	if (!aExtraInfo) // In case the caller explicitly called it with NULL.
		aExtraInfo = "";

	if (g_script.mErrorStdOut && !g_script.mIsReadyToExecute) // i.e. runtime errors are always displayed via dialog.
	{
		// See LineError() for details.
		printf(STD_ERROR_FORMAT, Line::sSourceFile[mCurrFileIndex], mCombinedLineNumber, aErrorText);
		if (*aExtraInfo)
			printf("     Specifically: %s\n", aExtraInfo);
	}
	else
	{
		char buf[MSGBOX_TEXT_SIZE], *cp = buf;
		int buf_space_remaining = (int)sizeof(buf);

		cp += snprintf(cp, buf_space_remaining, "Error at line %u", mCombinedLineNumber); // Don't call it "critical" because it's usually a syntax error.
		buf_space_remaining = (int)(sizeof(buf) - (cp - buf));

		if (mCurrFileIndex)
		{
			cp += snprintf(cp, buf_space_remaining, " in #include file \"%s\"", Line::sSourceFile[mCurrFileIndex]);
			buf_space_remaining = (int)(sizeof(buf) - (cp - buf));
		}
		//else don't bother cluttering the display if it's the main script file.

		cp += snprintf(cp, buf_space_remaining, ".\n\n");
		buf_space_remaining = (int)(sizeof(buf) - (cp - buf));

		if (*aExtraInfo)
		{
			cp += snprintf(cp, buf_space_remaining, "Line Text: %-1.100s%s\nError: "  // i.e. the word "Error" is omitted as being too noisy when there's no ExtraInfo to put into the dialog.
				, aExtraInfo // aExtraInfo defaults to "" so this is safe.
				, strlen(aExtraInfo) > 100 ? "..." : "");
			buf_space_remaining = (int)(sizeof(buf) - (cp - buf));
		}
		snprintf(cp, buf_space_remaining, "%s\n\n%s", aErrorText, mIsRestart ? OLD_STILL_IN_EFFECT : WILL_EXIT);

		//ShowInEditor();
		MsgBox(buf);
	}
	return FAIL; // See above for why it's better to return FAIL than CRITICAL_ERROR.
}



char *Script::ListVars(char *aBuf, int aBufSize) // aBufSize should be an int to preserve negatives from caller (caller relies on this).
// aBufSize is an int so that any negative values passed in from caller are not lost.
// Translates this script's list of variables into text equivalent, putting the result
// into aBuf and returning the position in aBuf of its new string terminator.
{
	char *aBuf_orig = aBuf;
	if (g.CurrentFunc)
	{
		// This definition might help compiler string pooling by ensuring it stays the same for both usages:
		#define LIST_VARS_UNDERLINE "\r\n--------------------------------------------------\r\n"
		// Start at the oldest and continue up through the newest:
		aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "Local Variables for %s()%s", g.CurrentFunc->mName, LIST_VARS_UNDERLINE);
		Func &func = *g.CurrentFunc; // For performance.
		for (int i = 0; i < func.mVarCount; ++i)
			if (func.mVar[i]->Type() == VAR_NORMAL) // Don't bother showing clipboard and other built-in vars.
				aBuf = func.mVar[i]->ToText(aBuf, BUF_SPACE_REMAINING, true);
	}
	// v1.0.31: The description "alphabetical" is kept even though it isn't quite true
	// when the lazy variable list exists, since those haven't yet been sorted into the main list.
	// However, 99.9% of scripts do not use the lazy list, so it seems too rare to worry about other
	// than document it in the ListVars command in the help file:
	aBuf += snprintf(aBuf, BUF_SPACE_REMAINING, "%sGlobal Variables (alphabetical)%s"
		, g.CurrentFunc ? "\r\n\r\n" : "", LIST_VARS_UNDERLINE);
	// Start at the oldest and continue up through the newest:
	for (int i = 0; i < mVarCount; ++i)
		if (mVar[i]->Type() == VAR_NORMAL) // Don't bother showing clipboard and other built-in vars.
			aBuf = mVar[i]->ToText(aBuf, BUF_SPACE_REMAINING, true);
	return aBuf;
}



char *Script::ListKeyHistory(char *aBuf, int aBufSize) // aBufSize should be an int to preserve negatives from caller (caller relies on this).
// aBufSize is an int so that any negative values passed in from caller are not lost.
// Translates this key history into text equivalent, putting the result
// into aBuf and returning the position in aBuf of its new string terminator.
{
	char *aBuf_orig = aBuf; // Needed for the BUF_SPACE_REMAINING macro.
	// I was initially concerned that GetWindowText() can hang if the target window is
	// hung.  But at least on newer OS's, this doesn't seem to be a problem: MSDN says
	// "If the window does not have a caption, the return value is a null string. This
	// behavior is by design. It allows applications to call GetWindowText without hanging
	// if the process that owns the target window is hung. However, if the target window
	// is hung and it belongs to the calling application, GetWindowText will hang the
	// calling application."
	HWND target_window = GetForegroundWindow();
	char win_title[100];
	if (target_window)
		GetWindowText(target_window, win_title, sizeof(win_title));
	else
		*win_title = '\0';

	char timer_list[128] = "";
	for (ScriptTimer *timer = mFirstTimer; timer != NULL; timer = timer->mNextTimer)
		if (timer->mEnabled)
			snprintfcat(timer_list, sizeof(timer_list) - 3, "%s ", timer->mLabel->mName); // Allow room for "..."
	if (*timer_list)
	{
		size_t length = strlen(timer_list);
		if (length > (sizeof(timer_list) - 5))
			strlcpy(timer_list + length, "...", sizeof(timer_list) - length);
		else if (timer_list[length - 1] == ' ')
			timer_list[--length] = '\0';  // Remove the last space if there was room enough for it to have been added.
	}

	char LRtext[256];
	aBuf += snprintf(aBuf, aBufSize,
		"Window: %s"
		//"\r\nBlocks: %u"
		"\r\nKeybd hook: %s"
		"\r\nMouse hook: %s"
		"\r\nEnabled Timers: %u of %u (%s)"
		//"\r\nInterruptible?: %s"
		"\r\nInterrupted threads: %d%s"
		"\r\nPaused threads: %d of %d (%d layers)"
		"\r\nModifiers (GetKeyState() now) = %s"
		"\r\n"
		, win_title
		//, SimpleHeap::GetBlockCount()
		, g_KeybdHook == NULL ? "no" : "yes"
		, g_MouseHook == NULL ? "no" : "yes"
		, mTimerEnabledCount, mTimerCount, timer_list
		//, INTERRUPTIBLE ? "yes" : "no"
		, g_nThreads > 1 ? g_nThreads - 1 : 0
		, g_nThreads > 1 ? " (preempted: they will resume when the current thread finishes)" : ""
		, g_nPausedThreads, g_nThreads, g_nLayersNeedingTimer
		, ModifiersLRToText(GetModifierLRState(true), LRtext));
	GetHookStatus(aBuf, BUF_SPACE_REMAINING);
	aBuf += strlen(aBuf); // Adjust for what GetHookStatus() wrote to the buffer.
	return aBuf + snprintf(aBuf, BUF_SPACE_REMAINING, g_KeyHistory ? "\r\nPress [F5] to refresh."
		: "\r\nKey History has been disabled via #KeyHistory 0.");
}



ResultType Script::ActionExec(char *aAction, char *aParams, char *aWorkingDir, bool aDisplayErrors
	, char *aRunShowMode, HANDLE *aProcess, bool aUpdateLastError, bool aUseRunAs, Var *aOutputVar)
// Caller should specify NULL for aParams if it wants us to attempt to parse out params from
// within aAction.  Caller may specify empty string ("") instead to specify no params at all.
// Remember that aAction and aParams can both be NULL, so don't dereference without checking first.
// Note: For the Run & RunWait commands, aParams should always be NULL.  Params are parsed out of
// the aActionString at runtime, here, rather than at load-time because Run & RunWait might contain
// deferenced variable(s), which can only be resolved at runtime.
{
	HANDLE hprocess_local;
	HANDLE &hprocess = aProcess ? *aProcess : hprocess_local; // To simplify other things.
	hprocess = NULL; // Init output param if the caller gave us memory to store it.  Even if caller didn't, other things below may rely on this being initialized.
	if (aOutputVar) // Same
		aOutputVar->Assign();

	// Launching nothing is always a success:
	if (!aAction || !*aAction) return OK;

	size_t aAction_length = strlen(aAction);
	if (aAction_length >= LINE_SIZE) // Max length supported by CreateProcess() is 32 KB. But there hasn't been any demand to go above 16 KB, so seems little need to support it (plus it reduces risk of stack overflow).
	{
        if (aDisplayErrors)
			ScriptError("String too long." ERR_ABORT); // Short msg since so rare.
		return FAIL;
	}
	// Declare this buf here to ensure it's in scope for the entire function, since its
	// contents may be referred to indirectly:
	char *parse_buf = (char *)_alloca(aAction_length + 1); // v1.0.44.14: _alloca() helps conserve stack space.

	// Make sure this is set to NULL because CreateProcess() won't work if it's the empty string:
	if (aWorkingDir && !*aWorkingDir)
		aWorkingDir = NULL;

	#define IS_VERB(str) (   !stricmp(str, "find") || !stricmp(str, "explore") || !stricmp(str, "open")\
		|| !stricmp(str, "edit") || !stricmp(str, "print") || !stricmp(str, "properties")   )

	// Set default items to be run by ShellExecute().  These are also used by the error
	// reporting at the end, which is why they're initialized even if CreateProcess() works
	// and there's no need to use ShellExecute():
	char *shell_action = aAction;
	char *shell_params = const_cast<char*>(aParams ? aParams : "");
	bool shell_action_is_system_verb = false;

	///////////////////////////////////////////////////////////////////////////////////
	// This next section is done prior to CreateProcess() because when aParams is NULL,
	// we need to find out whether aAction contains a system verb.
	///////////////////////////////////////////////////////////////////////////////////
	if (aParams) // Caller specified the params (even an empty string counts, for this purpose).
		shell_action_is_system_verb = IS_VERB(shell_action);
	else // Caller wants us to try to parse params out of aAction.
	{
		// Make a copy so that we can modify it (i.e. split it into action & params):
		strcpy(parse_buf, aAction); // parse_buf is already known to be large enough.

		// Find out the "first phrase" in the string.  This is done to support the special "find" and "explore"
		// operations as well as minmize the chance that executable names intended by the user to be parameters
		// will not be considered to be the program to run (e.g. for use with a compiler, perhaps).
		char *first_phrase, *first_phrase_end, *second_phrase;
		if (*parse_buf == '"')
		{
			first_phrase = parse_buf + 1;  // Omit the double-quotes, for use with CreateProcess() and such.
			first_phrase_end = strchr(first_phrase, '"');
		}
		else
		{
			first_phrase = parse_buf;
			// Set first_phrase_end to be the location of the first whitespace char, if
			// one exists:
			first_phrase_end = StrChrAny(first_phrase, " \t"); // Find space or tab.
		}
		// Now first_phrase_end is either NULL, the position of the last double-quote in first-phrase,
		// or the position of the first whitespace char to the right of first_phrase.
		if (first_phrase_end)
		{
			// Split into two phrases:
			*first_phrase_end = '\0';
			second_phrase = first_phrase_end + 1;
		}
		else // the entire string is considered to be the first_phrase, and there's no second:
			second_phrase = NULL;
		if (shell_action_is_system_verb = IS_VERB(first_phrase))
		{
			shell_action = first_phrase;
			shell_params = const_cast<char*>(second_phrase ? second_phrase : "");
		}
		else
		{
// Rather than just consider the first phrase to be the executable and the rest to be the param, we check it
// for a proper extension so that the user can launch a document name containing spaces, without having to
// enclose it in double quotes.  UPDATE: Want to be able to support executable filespecs without requiring them
// to be enclosed in double quotes.  Therefore, search the entire string, rather than just first_phrase, for
// the left-most occurrence of a valid executable extension.  This should be fine since the user can still
// pass in EXEs and such as params as long as the first executable is fully qualified with its real extension
// so that we can tell that it's the action and not one of the params.

// This method is rather crude because is doesn't handle an extensionless executable such as "notepad test.txt"
// It's important that it finds the first occurrence of an executable extension in case there are other
// occurrences in the parameters.  Also, .pif and .lnk are currently not considered executables for this purpose
// since they probably don't accept parameters:
			strcpy(parse_buf, aAction);  // Restore the original value in case it was changed. parse_buf is already known to be large enough.
			char *action_extension;
			if (   !(action_extension = strcasestr(parse_buf, ".exe "))   )
				if (   !(action_extension = strcasestr(parse_buf, ".exe\""))   )
					if (   !(action_extension = strcasestr(parse_buf, ".bat "))   )
						if (   !(action_extension = strcasestr(parse_buf, ".bat\""))   )
							if (   !(action_extension = strcasestr(parse_buf, ".com "))   )
								if (   !(action_extension = strcasestr(parse_buf, ".com\""))   )
									// Not 100% sure that .cmd and .hta are genuine executables in every sense:
									if (   !(action_extension = strcasestr(parse_buf, ".cmd "))   )
										if (   !(action_extension = strcasestr(parse_buf, ".cmd\""))   )
											if (   !(action_extension = strcasestr(parse_buf, ".hta "))   )
												action_extension = strcasestr(parse_buf, ".hta\"");

			if (action_extension)
			{
				shell_action = parse_buf;
				// +4 for the 3-char extension with the period:
				shell_params = action_extension + 4;  // exec_params is now the start of params, or empty-string.
				if (*shell_params == '"')
					// Exclude from shell_params since it's probably belongs to the action, not the params
					// (i.e. it's paired with another double-quote at the start):
					++shell_params;
				if (*shell_params)
				{
					// Terminate the <aAction> string in the right place.  For this to work correctly,
					// at least one space must exist between action & params (shortcoming?):
					*shell_params = '\0';
					++shell_params;
					ltrim(shell_params); // Might be empty string after this, which is ok.
				}
				// else there doesn't appear to be any params, so just leave shell_params set to empty string.
			}
			// else there's no extension: so assume the whole <aAction> is a document name to be opened by
			// the shell.  So leave shell_action and shell_params set their original defaults.
		}
	}

	// This is distinct from hprocess being non-NULL because the two aren't always the
	// same.  For example, if the user does "Run, find D:\" or "RunWait, www.yahoo.com",
	// no new process handle will be available even though the launch was successful:
	bool success = false;
	char system_error_text[512] = "";

	bool use_runas = aUseRunAs && mRunAsUser && (*mRunAsUser || *mRunAsPass || *mRunAsDomain);
	if (use_runas && shell_action_is_system_verb)
	{
		if (aDisplayErrors)
			ScriptError("System verbs unsupported with RunAs." ERR_ABORT);
		return FAIL;
	}

	// If the caller originally gave us NULL for aParams, always try CreateProcess() before
	// trying ShellExecute().  This is because ShellExecute() is usually a lot slower.
	// The only exception is if the action appears to be a verb such as open, edit, or find.
	// In that case, we'll also skip the CreateProcess() attempt and do only the ShellExecute().
	// If the user really meant to launch find.bat or find.exe, for example, he should add
	// the extension (e.g. .exe) to differentiate "find" from "find.exe":
	if (!shell_action_is_system_verb)
	{
		STARTUPINFO si = {0}; // Zero fill to be safer.
		si.cb = sizeof(si);
		// The following are left at the default of NULL/0 set higher above:
		//si.lpReserved = si.lpDesktop = si.lpTitle = NULL;
		//si.lpReserved2 = NULL;
		si.dwFlags = STARTF_USESHOWWINDOW;  // This tells it to use the value of wShowWindow below.
		si.wShowWindow = (aRunShowMode && *aRunShowMode) ? Line::ConvertRunMode(aRunShowMode) : SW_SHOWNORMAL;
		PROCESS_INFORMATION pi = {0};

		// Since CreateProcess() requires that the 2nd param be modifiable, ensure that it is
		// (even if this is ANSI and not Unicode; it's just safer):
		char *command_line; // Need a new buffer other than parse_buf because parse_buf's contents may still be pointed to directly or indirectly for use further below.
		if (aParams && *aParams)
		{
			command_line = (char *)_alloca(aAction_length + strlen(aParams) + 10); // +10 to allow room for space, terminator, and any extra chars that might get added in the future.
			sprintf(command_line, "%s %s", aAction, aParams);
		}
		else // We're running the original action from caller.
		{
			command_line = (char *)_alloca(aAction_length + 1);
        	strcpy(command_line, aAction); // CreateProcessW() requires modifiable string.  Although non-W version is used now, it feels safer to make it modifiable anyway.
		}

		if (use_runas)
		{
			if (!DoRunAs(command_line, aWorkingDir, aDisplayErrors, aUpdateLastError, si.wShowWindow  // wShowWindow (min/max/hide).
				, aOutputVar, pi, success, hprocess, system_error_text)) // These are output parameters it will set for us.
				return FAIL; // It already displayed the error, if appropriate.
		}
		else
		{
			// MSDN: "If [lpCurrentDirectory] is NULL, the new process is created with the same
			// current drive and directory as the calling process." (i.e. since caller may have
			// specified a NULL aWorkingDir).  Also, we pass NULL in for the first param so that
			// it will behave the following way (hopefully under all OSes): "the first white-space � delimited
			// token of the command line specifies the module name. If you are using a long file name that
			// contains a space, use quoted strings to indicate where the file name ends and the arguments
			// begin (see the explanation for the lpApplicationName parameter). If the file name does not
			// contain an extension, .exe is appended. Therefore, if the file name extension is .com,
			// this parameter must include the .com extension. If the file name ends in a period (.) with
			// no extension, or if the file name contains a path, .exe is not appended. If the file name does
			// not contain a directory path, the system searches for the executable file in the following
			// sequence...".
			// Provide the app name (first param) if possible, for greater expected reliability.
			// UPDATE: Don't provide the module name because if it's enclosed in double quotes,
			// CreateProcess() will fail, at least under XP:
			//if (CreateProcess(aParams && *aParams ? aAction : NULL
			if (CreateProcess(NULL, command_line, NULL, NULL, FALSE, 0, NULL, aWorkingDir, &si, &pi))
			{
				success = true;
				if (pi.hThread)
					CloseHandle(pi.hThread); // Required to avoid memory leak.
				hprocess = pi.hProcess;
				if (aOutputVar)
					aOutputVar->Assign(pi.dwProcessId);
			}
			else
				GetLastErrorText(system_error_text, sizeof(system_error_text), aUpdateLastError);
		}
	}

	if (!success) // Either the above wasn't attempted, or the attempt failed.  So try ShellExecute().
	{
		if (use_runas)
		{
			// Since CreateProcessWithLogonW() was either not attempted or did not work, it's probably
			// best to display an error rather than trying to run it without the RunAs settings.
			// This policy encourages users to have RunAs in effect only when necessary:
			if (aDisplayErrors)
				ScriptError("Launch Error (possibly related to RunAs)." ERR_ABORT, system_error_text);
			return FAIL;
		}
		SHELLEXECUTEINFO sei = {0};
		// sei.hwnd is left NULL to avoid potential side-effects with having a hidden window be the parent.
		// However, doing so may result in the launched app appearing on a different monitor than the
		// script's main window appears on (for multimonitor systems).  This seems fairly inconsequential
		// since scripted workarounds are possible.
		sei.cbSize = sizeof(sei);
		// Below: "indicate that the hProcess member receives the process handle" and not to display error dialog:
		sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
		sei.lpDirectory = aWorkingDir; // OK if NULL or blank; that will cause current dir to be used.
		sei.nShow = (aRunShowMode && *aRunShowMode) ? Line::ConvertRunMode(aRunShowMode) : SW_SHOWNORMAL;
		if (shell_action_is_system_verb)
		{
			sei.lpVerb = shell_action;
			if (!stricmp(shell_action, "properties"))
				sei.fMask |= SEE_MASK_INVOKEIDLIST;  // Need to use this for the "properties" verb to work reliably.
			sei.lpFile = shell_params;
			sei.lpParameters = NULL;
		}
		else
		{
			sei.lpVerb = NULL;  // A better choice than "open" because NULL causes default verb to be used.
			sei.lpFile = shell_action;
			sei.lpParameters = *shell_params ? shell_params : NULL; // Above has ensured that shell_params isn't NULL.
			// Above was fixed v1.0.42.06 to be NULL rather than the empty string to prevent passing an
			// extra space at the end of a parameter list (this might happen only when launching a shortcut
			// [.lnk file]).  MSDN states: "If the lpFile member specifies a document file, lpParameters should
			// be NULL."  This implies that NULL is a suitable value for lpParameters in cases where you don't
			// want to pass any parameters at all.
		}
		if (ShellExecuteEx(&sei)) // Relies on short-circuit boolean order.
		{
			hprocess = sei.hProcess;
			// aOutputVar is left blank because:
			// ProcessID is not available when launched this way, and since GetProcessID() is only
			// available in WinXP SP1, no effort is currently made to dynamically load it from
			// kernel32.dll (to retain compatibility with older OSes).
			success = true;
		}
		else
			GetLastErrorText(system_error_text, sizeof(system_error_text), aUpdateLastError);
	}

	if (!success) // The above attempt(s) to launch failed.
	{
		if (aDisplayErrors)
		{
			char error_text[2048], verb_text[128];
			if (shell_action_is_system_verb)
				snprintf(verb_text, sizeof(verb_text), "\nVerb: <%s>", shell_action);
			else // Don't bother showing it if it's just "open".
				*verb_text = '\0';
			// Use format specifier to make sure it doesn't get too big for the error
			// function to display:
			snprintf(error_text, sizeof(error_text)
				, "Failed attempt to launch program or document:"
				"\nAction: <%-0.400s%s>"
				"%s"
				"\nParams: <%-0.400s%s>\n\n" ERR_ABORT_NO_SPACES
				, shell_action, strlen(shell_action) > 400 ? "..." : ""
				, verb_text
				, shell_params, strlen(shell_params) > 400 ? "..." : ""
				);
			ScriptError(error_text, system_error_text);
		}
		return FAIL;
	}

	// Otherwise, success:
	if (aUpdateLastError)
		g.LastError = 0; // Force zero to indicate success, which seems more maintainable and reliable than calling GetLastError() right here.

	// If aProcess isn't NULL, the caller wanted the process handle left open and so it must eventually call
	// CloseHandle().  Otherwise, we should close the process if it's non-NULL (it can be NULL in the case of
	// launching things like "find D:\" or "www.yahoo.com").
	if (!aProcess && hprocess)
		CloseHandle(hprocess); // Required to avoid memory leak.
	return OK;
}

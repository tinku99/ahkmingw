///////////////////////////////////////////////////////////////////////////////
//
// AutoIt
//
// Copyright (C)1999-2007:
//		- Jonathan Bennett <jon@hiddensoft.com>
//		- Others listed at http://www.autoitscript.com/autoit3/docs/credits.htm
//      - Chris Mallett (support@autohotkey.com): adaptation of this file's
//        functions to interface with AutoHotkey.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
///////////////////////////////////////////////////////////////////////////////
//
// script_registry.cpp
//
// Contains registry handling routines.  Part of script.cpp
//
///////////////////////////////////////////////////////////////////////////////


// Includes
#include "stdafx.h" // pre-compiled headers
#include "script.h"
#include "util.h" // for strlcpy()
#include "globaldata.h"


ResultType Line::IniRead(char *aFilespec, char *aSection, char *aKey, char *aDefault)
{
	if (!aDefault || !*aDefault)
		aDefault = "ERROR";  // This mirrors what AutoIt2 does for its default value.
	char	szFileTemp[_MAX_PATH+1];
	char	*szFilePart;
	char	szBuffer[65535] = "";					// Max ini file size is 65535 under 95
	// Get the fullpathname (ini functions need a full path):
	GetFullPathName(aFilespec, _MAX_PATH, szFileTemp, &szFilePart);
	GetPrivateProfileString(aSection, aKey, aDefault, szBuffer, sizeof(szBuffer), szFileTemp);
	// The above function is supposed to set szBuffer to be aDefault if it can't find the
	// file, section, or key.  In other words, it always changes the contents of szBuffer.
	return OUTPUT_VAR->Assign(szBuffer); // Avoid using the length the API reported because it might be inaccurate if the data contains any binary zeroes, or if the data is double-terminated, etc.
	// Note: ErrorLevel is not changed by this command since the aDefault value is returned
	// whenever there's an error.
}



ResultType Line::IniWrite(char *aValue, char *aFilespec, char *aSection, char *aKey)
{
	char	szFileTemp[_MAX_PATH+1];
	char	*szFilePart;
	// Get the fullpathname (ini functions need a full path) 
	GetFullPathName(aFilespec, _MAX_PATH, szFileTemp, &szFilePart);
	BOOL result = WritePrivateProfileString(aSection, aKey, aValue, szFileTemp);  // Returns zero on failure.
	WritePrivateProfileString(NULL, NULL, NULL, szFileTemp);	// Flush
	return g_script.mIsAutoIt2 ? OK : g_ErrorLevel->Assign(result ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);
}



ResultType Line::IniDelete(char *aFilespec, char *aSection, char *aKey)
// Note that aKey can be NULL, in which case the entire section will be deleted.
{
	char	szFileTemp[_MAX_PATH+1];
	char	*szFilePart;
	// Get the fullpathname (ini functions need a full path) 
	GetFullPathName(aFilespec, _MAX_PATH, szFileTemp, &szFilePart);
	BOOL result = WritePrivateProfileString(aSection, aKey, NULL, szFileTemp);  // Returns zero on failure.
	WritePrivateProfileString(NULL, NULL, NULL, szFileTemp);	// Flush
	return g_script.mIsAutoIt2 ? OK : g_ErrorLevel->Assign(result ? ERRORLEVEL_NONE : ERRORLEVEL_ERROR);
}



ResultType Line::RegRead(HKEY aRootKey, char *aRegSubkey, char *aValueName)
{
	Var &output_var = *OUTPUT_VAR;
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.
	output_var.Assign(); // Init.  Tell it not to free the memory by not calling with "".

	HKEY	hRegKey;
	DWORD	dwRes, dwBuf, dwType;
	LONG    result;
	// My: Seems safest to keep the limit just below 64K in case Win95 has problems with larger values.
	char	szRegBuffer[65535]; // Only allow reading of 64Kb from a key

	if (!aRootKey)
		return OK;  // Let ErrorLevel tell the story.

	// Open the registry key
	if (RegOpenKeyEx(aRootKey, aRegSubkey, 0, KEY_READ, &hRegKey) != ERROR_SUCCESS)
		return OK;  // Let ErrorLevel tell the story.

	// Read the value and determine the type.  If aValueName is the empty string, the key's default value is used.
	if (RegQueryValueEx(hRegKey, aValueName, NULL, &dwType, NULL, NULL) != ERROR_SUCCESS)
	{
		RegCloseKey(hRegKey);
		return OK;  // Let ErrorLevel tell the story.
	}

	char *contents, *cp;

	// The way we read is different depending on the type of the key
	switch (dwType)
	{
		case REG_DWORD:
			dwRes = sizeof(dwBuf);
			RegQueryValueEx(hRegKey, aValueName, NULL, NULL, (LPBYTE)&dwBuf, &dwRes);
			RegCloseKey(hRegKey);
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
			return output_var.Assign((DWORD)dwBuf);

		// Note: The contents of any of these types can be >64K on NT/2k/XP+ (though that is probably rare):
		case REG_SZ:
		case REG_EXPAND_SZ:
		case REG_MULTI_SZ:
			dwRes = 0; // Retained for backward compatibility because values >64K may cause it to fail on Win95 (unverified, and MSDN implies its value should be ignored for the following call).
			// MSDN: If lpData is NULL, and lpcbData is non-NULL, the function returns ERROR_SUCCESS and stores
			// the size of the data, in bytes, in the variable pointed to by lpcbData.
			if (RegQueryValueEx(hRegKey, aValueName, NULL, NULL, NULL, &dwRes) != ERROR_SUCCESS // Find how large the value is.
				|| !dwRes) // Can't find size (realistically might never happen), or size is zero.
			{
				RegCloseKey(hRegKey);
				return g_ErrorLevel->Assign(ERRORLEVEL_NONE); // For backward compatibility, indicate success (these conditions should be very rare anyway).
			}
			// Set up the variable to receive the contents, enlarging it if necessary:
			// Since dwRes includes the space for the zero terminator (if the MSDN docs
			// are accurate), this will enlarge it to be 1 byte larger than we need,
			// which leaves room for the final newline character to be inserted after
			// the last item.  But add 2 to the requested capacity in case the data isn't
			// terminated in the registry, which allows double-NULL to be put in for REG_MULTI_SZ later.
			if (output_var.Assign(NULL, (VarSizeType)(dwRes + 2)) != OK)
			{
				RegCloseKey(hRegKey);
				return FAIL; // FAIL is only returned when the error is a critical one such as this one.
			}

			contents = output_var.Contents(); // This target buf should now be large enough for the result.

			result = RegQueryValueEx(hRegKey, aValueName, NULL, NULL, (LPBYTE)contents, &dwRes);
			RegCloseKey(hRegKey);

			if (result != ERROR_SUCCESS || !dwRes) // Relies on short-circuit boolean order.
				*contents = '\0'; // MSDN says the contents of the buffer is undefined after the call in some cases, so reset it.
				// Above realistically probably never happens; for backward compatibility (and simplicity),
				// consider it a success.
			else
			{
				// See ReadRegString() for more comments about the following:
				// The MSDN docs state that we should ensure that the buffer is NULL-terminated ourselves:
				// "If the data has the REG_SZ, REG_MULTI_SZ or REG_EXPAND_SZ type, then lpcbData will also
				// include the size of the terminating null character or characters ... If the data has the
				// REG_SZ, REG_MULTI_SZ or REG_EXPAND_SZ type, the string may not have been stored with the
				// proper null-terminating characters. Applications should ensure that the string is properly
				// terminated before using it, otherwise, the application may fail by overwriting a buffer."
				//
				// Double-terminate so that the loop can find out the true end of the buffer.
				// The MSDN docs cited above are a little unclear.  The most likely interpretation is that
				// dwRes contains the true size retrieved.  For example, if dwRes is 1, the first char
				// in the buffer is either a NULL or an actual non-NULL character that was originally
				// stored in the registry incorrectly (i.e. without a terminator).  In either case, do
				// not change the first character, just leave it as is and add a NULL at the 2nd and
				// 3rd character positions to ensure that it is double terminated in every case:
				contents[dwRes] = contents[dwRes + 1] = '\0';

				if (dwType == REG_MULTI_SZ) // Convert NULL-delimiters into newline delimiters.
				{
					for (cp = contents;; ++cp)
					{
						if (!*cp)
						{
							// Unlike AutoIt3, it seems best to have a newline character after the
							// last item in the list also.  It usually makes parsing easier:
							*cp = '\n';	// Convert to \n for later storage in the user's variable.
							if (!*(cp + 1)) // Buffer is double terminated, so this is safe.
								// Double null terminator marks the end of the used portion of the buffer.
								break;
						}
					}
					// else the buffer is empty (see above notes for explanation).  So don't put any newlines
					// into it at all, since each newline should correspond to an item in the buffer.
				}
			}
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
			output_var.Length() = (VarSizeType)strlen(contents); // Due to conservative buffer sizes above, length is probably too large by 3. So update to reflect the true length.
			return output_var.Close();  // In case it's the clipboard.

		case REG_BINARY:
		{
			dwRes = sizeof(szRegBuffer);
			result = RegQueryValueEx(hRegKey, aValueName, NULL, NULL, (LPBYTE)szRegBuffer, &dwRes);
			RegCloseKey(hRegKey);

			if (result == ERROR_MORE_DATA)
				// The API docs state that the buffer's contents are undefined in this case,
				// so for no we don't support values larger than our buffer size:
				return OK; // Let ErrorLevel tell the story.  The output variable has already been made blank.

			// Set up the variable to receive the contents, enlarging it if necessary.
			// AutoIt3: Each byte will turned into 2 digits, plus a final null:
			if (output_var.Assign(NULL, (VarSizeType)(dwRes * 2)) != OK)
				return FAIL;
			contents = output_var.Contents();
			*contents = '\0';

			int j = 0;
			DWORD i, n; // i and n must be unsigned to work
			char szHexData[] = "0123456789ABCDEF";  // Access to local vars might be faster than static ones.
			for (i = 0; i < dwRes; ++i)
			{
				n = szRegBuffer[i];				// Get the value and convert to 2 digit hex
				contents[j + 1] = szHexData[n % 16];
				n /= 16;
				contents[j] = szHexData[n % 16];
				j += 2;
			}
			contents[j] = '\0'; // Terminate
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
			return output_var.Close();  // In case it's the clipboard.  Length() was already set by the earlier call to Assign().
		}
	}

	// Since above didn't return, this is an unsupported value type.
	return OK;  // Let ErrorLevel tell the story.
} // RegRead()



ResultType Line::RegWrite(DWORD aValueType, HKEY aRootKey, char *aRegSubkey, char *aValueName, char *aValue)
// If aValueName is the empty string, the key's default value is used.
{
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.

	HKEY	hRegKey;
	DWORD	dwRes, dwBuf;

	// My: Seems safest to keep the limit just below 64K in case Win95 has problems with larger values.
	char szRegBuffer[65535], *buf; // Only allow writing of 64Kb to a key for Win9x, which is all it supports.
	#define SET_REG_BUF \
		if (g_os.IsWin9x())\
		{\
			strlcpy(szRegBuffer, aValue, sizeof(szRegBuffer));\
			buf = szRegBuffer;\
		}\
		else\
			buf = aValue;

	if (!aRootKey || aValueType == REG_NONE || aValueType == REG_SUBKEY) // Can't write to these.
		return OK;  // Let ErrorLevel tell the story.

	// Open/Create the registry key
	// The following works even on root keys (i.e. blank subkey), although values can't be created/written to
	// HKCU's root level, perhaps because it's an alias for a subkey inside HKEY_USERS.  Even when RegOpenKeyEx()
	// is used on HKCU (which is probably redundant since it's a pre-opened key?), the API can't create values
	// there even though RegEdit can.
	if (RegCreateKeyEx(aRootKey, aRegSubkey, 0, "", REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hRegKey, &dwRes)
		!= ERROR_SUCCESS)
		return OK;  // Let ErrorLevel tell the story.

	// Write the registry differently depending on type of variable we are writing
	switch (aValueType)
	{
	case REG_SZ:
		SET_REG_BUF
		if (RegSetValueEx(hRegKey, aValueName, 0, REG_SZ, (CONST BYTE *)buf, (DWORD)strlen(buf)+1) == ERROR_SUCCESS)
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
		RegCloseKey(hRegKey);
		return OK;

	case REG_EXPAND_SZ:
		SET_REG_BUF
		if (RegSetValueEx(hRegKey, aValueName, 0, REG_EXPAND_SZ, (CONST BYTE *)buf, (DWORD)strlen(buf)+1) == ERROR_SUCCESS)
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
		RegCloseKey(hRegKey);
		return OK;
	
	case REG_MULTI_SZ:
	{
		// Don't allow values over 64K for this type because aValue might not be a writable
		// string, and we would need to write to it to temporarily change the newline delimiters
		// into zero-delimiters.  Even if we were to require callers to give us a modifiable string,
		// its capacity be 1 byte too small to handle the double termination that's needed
		// (i.e. if the last item in the list happens to not end in a newline):
		strlcpy(szRegBuffer, aValue, sizeof(szRegBuffer) - 1);  // -1 to leave space for a 2nd terminator.
		// Double-terminate:
		size_t length = strlen(szRegBuffer);
		szRegBuffer[length + 1] = '\0';

		// Remove any final newline the user may have provided since we don't want the length
		// to include it when calling RegSetValueEx() -- it would be too large by 1:
		if (length > 0 && szRegBuffer[length - 1] == '\n')
			szRegBuffer[--length] = '\0';

		// Replace the script's delimiter char with the zero-delimiter needed by RegSetValueEx():
		for (char *cp = szRegBuffer; *cp; ++cp)
			if (*cp == '\n')
				*cp = '\0';

		if (RegSetValueEx(hRegKey, aValueName, 0, REG_MULTI_SZ, (CONST BYTE *)szRegBuffer
			, (DWORD)(length ? length + 2 : 0)) == ERROR_SUCCESS)
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
		RegCloseKey(hRegKey);
		return OK;
	}

	case REG_DWORD:
		if (*aValue)
			dwBuf = ATOU(aValue);  // Changed to ATOU() for v1.0.24 so that hex values are supported.
		else // Default to 0 when blank.
			dwBuf = 0;
		if (RegSetValueEx(hRegKey, aValueName, 0, REG_DWORD, (CONST BYTE *)&dwBuf, sizeof(dwBuf) ) == ERROR_SUCCESS)
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
		RegCloseKey(hRegKey);
		return OK;

	case REG_BINARY:
	{
		int nLen = (int)strlen(aValue);

		// Stringlength must be a multiple of 2 
		if (nLen % 2)
		{
			RegCloseKey(hRegKey);
			return OK;  // Let ErrorLevel tell the story.
		}

		// Really crappy hex conversion
		int j = 0, i = 0, nVal, nMult;
		while (i < nLen && j < sizeof(szRegBuffer))
		{
			nVal = 0;
			for (nMult = 16; nMult >= 0; nMult = nMult - 15)
			{
				if (aValue[i] >= '0' && aValue[i] <= '9')
					nVal += (aValue[i] - '0') * nMult;
				else if (aValue[i] >= 'A' && aValue[i] <= 'F')
					nVal += (((aValue[i] - 'A'))+10) * nMult;
				else if (aValue[i] >= 'a' && aValue[i] <= 'f')
					nVal += (((aValue[i] - 'a'))+10) * nMult;
				else
				{
					RegCloseKey(hRegKey);
					return OK;  // Let ErrorLevel tell the story.
				}
				++i;
			}
			szRegBuffer[j++] = (char)nVal;
		}

		if (RegSetValueEx(hRegKey, aValueName, 0, REG_BINARY, (CONST BYTE *)szRegBuffer, (DWORD)j) == ERROR_SUCCESS)
			g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
		// else keep the default failure value for ErrorLevel
	
		RegCloseKey(hRegKey);
		return OK;
	}
	} // switch()

	// If we reached here then the requested type was unknown/unsupported.
	// Let ErrorLevel tell the story.
	RegCloseKey(hRegKey);
	return OK;
} // RegWrite()



bool Line::RegRemoveSubkeys(HKEY hRegKey)
{
	// Removes all subkeys to the given key.  Will not touch the given key.
	CHAR Name[256];
	DWORD dwNameSize;
	FILETIME ftLastWrite;
	HKEY hSubKey;
	bool Success;

	for (;;) 
	{ // infinite loop 
		dwNameSize=255;
		if (RegEnumKeyEx(hRegKey, 0, Name, &dwNameSize, NULL, NULL, NULL, &ftLastWrite) == ERROR_NO_MORE_ITEMS)
			break;
		if (RegOpenKeyEx(hRegKey, Name, 0, KEY_READ, &hSubKey) != ERROR_SUCCESS)
			return false;
		
		Success=RegRemoveSubkeys(hSubKey);
		RegCloseKey(hSubKey);
		if (!Success)
			return false;
		else if (RegDeleteKey(hRegKey, Name) != ERROR_SUCCESS)
			return false;
	}
	return true;
}



ResultType Line::RegDelete(HKEY aRootKey, char *aRegSubkey, char *aValueName)
{
	g_ErrorLevel->Assign(ERRORLEVEL_ERROR); // Set default ErrorLevel.

	HKEY	hRegKey;

	if (!aRootKey)
		return OK;  // Let ErrorLevel tell the story.

	// Open the key we want
	if (RegOpenKeyEx(aRootKey, aRegSubkey, 0, KEY_READ | KEY_WRITE, &hRegKey) != ERROR_SUCCESS)
		return OK;  // Let ErrorLevel tell the story.

	if (!aValueName || !*aValueName)
	{
		// Remove the entire Key
		bool success = RegRemoveSubkeys(hRegKey); // Delete any subitems within the key.
		RegCloseKey(hRegKey); // Close parent key.  Not sure if this needs to be done only after the above.
		if (!success)
			return OK;  // Let ErrorLevel tell the story.
		if (RegDeleteKey(aRootKey, aRegSubkey) != ERROR_SUCCESS) 
			return OK;  // Let ErrorLevel tell the story.
	}
	else
	{
		// Remove Value.  The special phrase "ahk_default" indicates that the key's default
		// value (displayed as "(Default)" by RegEdit) should be deleted.  This is done to
		// distinguish a blank (which deletes the entire subkey) from the default item.
		LONG lRes = RegDeleteValue(hRegKey, stricmp(aValueName, "ahk_default") ? aValueName : "");
		RegCloseKey(hRegKey);
		if (lRes != ERROR_SUCCESS)
			return OK;  // Let ErrorLevel tell the story.
	}

	return g_ErrorLevel->Assign(ERRORLEVEL_NONE); // Indicate success.
} // RegDelete()

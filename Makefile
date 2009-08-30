### Generated by Winemaker


SRCDIR                = .
SUBDIRS               =
DLLS                  =
EXES                  = ahkmingw.exe



### Common settings

CEXTRA                = -mno-cygwin
CXXEXTRA              = -mno-cygwin
RCEXTRA               =
INCLUDE_PATH          = -I. \
			$(MFC_INCLUDE_PATH)
DLL_PATH              =
LIBRARY_PATH          =
LIBRARIES             =


### ahkmingw.exe sources and settings

ahkmingw_exe_MODULE   = ahkmingw.exe
ahkmingw_exe_C_SRCS   =
ahkmingw_exe_CXX_SRCS = AutoHotkey.cpp \
			SimpleHeap.cpp \
			WinGroup.cpp \
			application.cpp \
			clipboard.cpp \
			globaldata.cpp \
			hook.cpp \
			hotkey.cpp \
			keyboard_mouse.cpp \
			mt19937ar-cok.cpp \
			os_version.cpp \
			script.cpp \
			script2.cpp \
			script_autoit.cpp \
			script_expression.cpp \
			script_gui.cpp \
			script_menu.cpp \
			script_registry.cpp \
			util.cpp \
			var.cpp \
			window.cpp
ahkmingw_exe_RC_SRCS  = resources/AutoHotkey.rc
ahkmingw_exe_LDFLAGS  = -mwindows \
			-mno-cygwin
ahkmingw_exe_DLL_PATH = $(MFC_LIBRARY_PATH)
ahkmingw_exe_DLLS     = odbc32 \
			ole32 \
			oleaut32 \
			winspool \
			mfc.dll
ahkmingw_exe_LIBRARY_PATH= $(MFC_LIBRARY_PATH)
ahkmingw_exe_LIBRARIES= uuid \
			mfc

ahkmingw_exe_OBJS     = $(ahkmingw_exe_C_SRCS:.c=.o) \
			$(ahkmingw_exe_CXX_SRCS:.cpp=.o) \
			$(ahkmingw_exe_RC_SRCS:.rc=.res)



### Global source lists

C_SRCS                = $(ahkmingw_exe_C_SRCS)
CXX_SRCS              = $(ahkmingw_exe_CXX_SRCS)
RC_SRCS               = $(ahkmingw_exe_RC_SRCS)


### Tools

CC = winegcc
CXX = wineg++
RC = wrc


### Generic targets

all: $(SUBDIRS) $(DLLS:%=%.so) $(EXES:%=%.so)

### Build rules

.PHONY: all clean dummy

$(SUBDIRS): dummy
	@cd $@ && $(MAKE)

# Implicit rules

.SUFFIXES: .cpp .rc .res
DEFINCL = $(INCLUDE_PATH) $(DEFINES) $(OPTIONS)

.c.o:
	$(CC) -c $(CFLAGS) $(CEXTRA) $(DEFINCL) -o $@ $<

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(CXXEXTRA) $(DEFINCL) -o $@ $<

.cxx.o:
	$(CXX) -c $(CXXFLAGS) $(CXXEXTRA) $(DEFINCL) -o $@ $<

.rc.res:
	$(RC) $(RCFLAGS) $(RCEXTRA) $(DEFINCL) -fo$@ $<

# Rules for cleaning

CLEAN_FILES     = y.tab.c y.tab.h lex.yy.c core *.orig *.rej \
                  \\\#*\\\# *~ *% .\\\#*

clean:: $(SUBDIRS:%=%/__clean__) $(EXTRASUBDIRS:%=%/__clean__)
	$(RM) $(CLEAN_FILES) $(RC_SRCS:.rc=.res) $(C_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o)
	$(RM) $(DLLS:%=%.so) $(EXES:%=%.so) $(EXES:%.exe=%)

$(SUBDIRS:%=%/__clean__): dummy
	cd `dirname $@` && $(MAKE) clean

$(EXTRASUBDIRS:%=%/__clean__): dummy
	-cd `dirname $@` && $(RM) $(CLEAN_FILES)

### Target specific build rules
DEFLIB = $(LIBRARY_PATH) $(LIBRARIES) $(DLL_PATH)

$(ahkmingw_exe_MODULE).so: $(ahkmingw_exe_OBJS)
	$(CXX) $(ahkmingw_exe_LDFLAGS) -o $@ $(ahkmingw_exe_OBJS) $(ahkmingw_exe_LIBRARY_PATH) $(DEFLIB) $(ahkmingw_exe_DLLS:%=-l%) $(ahkmingw_exe_LIBRARIES:%=-l%)



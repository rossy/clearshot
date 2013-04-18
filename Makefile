CC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
WINDRES=i686-w64-mingw32-windres
STRIP=i686-w64-mingw32-strip

CFLAGS=-Os -Wall --std=c99 -I./winextra
DLLTOOLFLAGS=-k
LDFLAGS=-static
LDLIBS=-lpng -lz -luser32 winextra/libuser32extra.a -lgdi32 -lshell32 winextra/libcomctl32extra.a -lcomdlg32 -ldwmapi -mwindows

all: clearshot.exe
.PHONY: all

lib%.a: %.def
	$(DLLTOOL) $(DLLTOOLFLAGS) --input-def $< --output-lib $@

res/clearshot_rc.o: res/clearshot.rc res/clearshot.exe.manifest
	$(WINDRES) $< -o $@

clearshot.exe: clearshot.c res/clearshot_rc.o winextra/winextra.h winextra/libcomctl32extra.a winextra/libuser32extra.a
	$(CC) $(LDFLAGS) $(CPPFLAGS) $(CFLAGS) $< res/clearshot_rc.o -o $@ $(LOADLIBES) $(LDLIBS)
	$(STRIP) -xs clearshot.exe

clean:
	-rm -rf res/clearshot_rc.o winextra/libuser32extra.a winextra/libcomctl32extra.a clearshot.exe
.PHONY: clean

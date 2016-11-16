bin_PROGRAMS = torsmo

if BUILD_SETI
seti = seti.c
endif

if BUILD_LINUX
linux = linux.c
endif

if BUILD_SOLARIS
solaris = solaris.c
endif

if BUILD_FREEBSD
freebsd = freebsd.c
endif

if BUILD_NETBSD
netbsd = netbsd.c
endif

if BUILD_NVCTRL
nvctrl = nvctrl.c
endif

torsmo_SOURCES = common.c fs.c $(linux) mail.c mixer.c $(nvctrl) $(seti) \
$(solaris) $(freebsd) $(netbsd) torsmo.c torsmo.h x11.c

torsmo_LDFLAGS = $(X11_LIBS) $(XFT_LIBS)

man_MANS = torsmo.1

if HAVE_HELP2MAN
torsmo.1: torsmo torsmo.x
	help2man -h -h -v -V -i torsmo.x -o torsmo.1 -N ./torsmo
endif

# torsmo.1 is in EXTRA_DIST in case that someone doesn't have help2man
EXTRA_DIST = readme.html changelog.html torsmorc.sample torsmo.1 torsmo.x \
             changelog2html.py

EXTRA_torsmo_SOURCES = seti.c linux.c solaris.c freebsd.c netbsd.c nvctrl.c

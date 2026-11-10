SUBDIRS = Backgrounds . Defaults IconSets Icons Pixmaps Styles Themes

prefsdatadir = $(pkgdatadir)

wpexecbindir = @wprefs_bindir@

# menus which need generated to have correct path to WPrefs or data directory
GENERATED_MENUS =\
	appearance.menu\
	menu\
	menu.ko\
	plmenu\
	plmenu.ko\
	wmmacros

dist_prefsdata_DATA =\
	autostart.sh\
	background.menu\
	exitscript.sh\
	README\
	README.themes

nodist_prefsdata_DATA = $(GENERATED_MENUS)

xsessionsdir = @datadir@/xsessions
xsessions_DATA = wmaker.desktop

EXTRA_DIST = $(addsuffix .in, $(GENERATED_MENUS)) wmaker.desktop

CLEANFILES = $(GENERATED_MENUS)

$(GENERATED_MENUS):
	$(AM_V_GEN)sed -e "s:#wprefs#:$(wpexecbindir)/WPrefs:" \
		-e "s:#wmdatadir#:$(pkgdatadir):" $(srcdir)/$@.in > $@

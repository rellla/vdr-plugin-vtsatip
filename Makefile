#
# Makefile for the vtsatip plugin
#

# The official name of this plugin (used in -P vtsatip)
PLUGIN = vtsatip

VERSION = 0.1.0

### The directory environment:

# Use package data if installed...otherwise assume we're under the VDR source directory:
PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),$(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." pkg-config --variable=$(1) vdr))
LIBDIR = $(call PKGCFG,libdir)
LOCDIR = $(call PKGCFG,locdir)
PLGCFG = $(call PKGCFG,plgcfg)
#
TMPDIR ?= /tmp

### The compiler options:

export CFLAGS   = $(call PKGCFG,cflags)
export CXXFLAGS = $(call PKGCFG,cxxflags) -std=c++17
STRIP           ?= /bin/true

### The version number of VDR's plugin API:

APIVERSION = $(call PKGCFG,apiversion)

### Allow user defined options to overwrite defaults:

-include $(PLGCFG)

### The name of the distribution archive:

ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### The name of the shared object file:

SOFILE = libvdr-$(PLUGIN).so

### Libraries

LIBS = -lpthread

### Includes and Defines (add further entries here):

INCLUDES +=

DEFINES += -DPLUGIN_NAME_I18N='"$(PLUGIN)"' -DVTSATIP_VERSION='"$(VERSION)"'

.PHONY: all all-redirect
all-redirect: all

### The object files (add further files here):

OBJS = $(PLUGIN).o device.o param.o rtsp.o sectionfilter.o tuner.o

### The main target:

all: $(SOFILE)

### Implicit rules:

%.o: %.c
	@echo CC $@
	$(Q)$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

### Dependencies:

MAKEDEP = $(CXX) -MM -MG
DEPFILE = .dependencies
$(DEPFILE): Makefile
	$(Q)$(MAKEDEP) $(CXXFLAGS) $(DEFINES) $(INCLUDES) $(OBJS:%.o=%.c) > $@

-include $(DEPFILE)

### Targets:

$(SOFILE): $(OBJS)
	@echo LD $@
	$(Q)$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared $(OBJS) $(LIBS) -o $@
	$(Q)$(STRIP) $@

install-lib: $(SOFILE)
	@echo IN $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)
	$(Q)install -D $^ $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)

install: install-lib

dist: clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

clean:
	@-rm -f $(OBJS) $(DEPFILE) *.so *.tgz core* *~

pkglib_LTLIBRARIES = eglplatform_drmadapter.la

eglplatform_drmadapter_la_SOURCES = \
	eglplatform_drmadapter.cpp

eglplatform_drmadapter_la_CXXFLAGS = \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/platforms/common \
	-I$(top_srcdir)/egl \
	-I$(top_srcdir)/egl/platforms/common \
	$(ANDROID_HEADERS_CFLAGS)

if WANT_TRACE
eglplatform_drmadapter_la_CXXFLAGS += -DDEBUG
endif
if WANT_DEBUG
eglplatform_drmadapter_la_CXXFLAGS += -ggdb -O0
endif

eglplatform_drmadapter_la_LIBADD = \
	$(top_builddir)/egl/platforms/hwcomposer/libhybris-hwcomposerwindow.la

eglplatform_drmadapter_la_LDFLAGS = \
	-avoid-version -module -shared -export-dynamic \
	-pthread \
	$(top_builddir)/egl/platforms/common/libhybris-eglplatformcommon.la \
	$(top_builddir)/gralloc/libgralloc.la \
	$(top_builddir)/hwc2/libhwc2.la

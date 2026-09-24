TESTS = test_gstd_pipeline_create 	\
	test_gstd_no_create 		\
	test_gstd_state			\
	test_gstd_stability		\
	test_gstd_pipeline_limit	\
	test_gstd_http

check_PROGRAMS = $(TESTS)

AM_CFLAGS = $(GSTD_CFLAGS) $(GST_CFLAGS) $(GIO_CFLAGS) -I$(top_srcdir)/libgstd/
AM_LDFLAGS = $(GSTD_LIBS) $(GST_LIBS) $(GIO_LIBS)
LDADD = $(top_srcdir)/libgstd/libgstd-1.0.la

cachelib_secondary_cache_SOURCES = cachelib_secondary_cache.cc
cachelib_secondary_cache_HEADERS = cachelib_secondary_cache.h
cachelib_secondary_cache_FUNC = RegisterCacheLibSecondaryCache
cachelib_secondary_cache_TESTS = cachelib_secondary_cache_test.cc

ifneq ($(strip $(USE_RTTI)),1)
$(error cachelib_secondary_cache requires USE_RTTI=1)
endif

ifeq ($(strip $(CACHELIB_INCLUDE_DIR)),)
$(error CACHELIB_INCLUDE_DIR is required for cachelib_secondary_cache)
endif

ifeq ($(strip $(CACHELIB_LIBRARY_DIR)),)
$(error CACHELIB_LIBRARY_DIR is required for cachelib_secondary_cache)
endif

cachelib_secondary_cache_CXXFLAGS = -isystem $(CACHELIB_INCLUDE_DIR) \
	$(foreach path,$(subst ;, ,$(CACHELIB_DEPENDENCY_INCLUDE_DIRS)),-isystem $(path)) \
	-fexceptions
cachelib_secondary_cache_LDFLAGS = -L$(CACHELIB_LIBRARY_DIR) \
	$(foreach path,$(subst ;, ,$(CACHELIB_DEPENDENCY_LIBRARY_DIRS)),-L$(path)) \
	-Wl,-rpath,$(CACHELIB_LIBRARY_DIR) -lcachelib_navy \
	-lthriftprotocol -lfolly

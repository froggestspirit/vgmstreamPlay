### main defs

OUTPUT_123 = vgmstream123

# -DUSE_ALLOCA

CFLAGS += -ffast-math -O3 -Wall -Werror=format-security -Wdeclaration-after-statement -Wvla -DVAR_ARRAYS -Iext_includes $(EXTRA_CFLAGS)
LDFLAGS += -Lsrc -Lext_libs -lvgmstream $(EXTRA_LDFLAGS) -lm
TARGET_EXT_LIBS = 

LIBAO_INC_PATH = ../../libao/include
LIBAO_LIB_PATH = ../../libao/bin

#ifdef VGM_DEBUG
#  CFLAGS += -DVGM_DEBUG_OUTPUT -O0
#  CFLAGS += -Wold-style-definition -Woverflow -Wpointer-arith -Wstrict-prototypes -pedantic -std=gnu90 -fstack-protector -Wformat
#endif


# config libs
VGM_ENABLE_G7221 = 1
ifeq ($(VGM_ENABLE_G7221),1) 
  CFLAGS  += -DVGM_USE_G7221
endif


### external libs

export CFLAGS LDFLAGS

### targets

vgmstream123: libvgmstream.a $(TARGET_EXT_LIBS)
	$(CC) $(CFLAGS) -I$(LIBAO_INC_PATH) vgmstream123.c $(LDFLAGS) -L$(LIBAO_LIB_PATH) -lao -o $(OUTPUT_123)

libvgmstream.a:
	$(MAKE) -C src $@

$(TARGET_EXT_LIBS):
	$(MAKE) -C ext_libs $@

clean:
	@rm -f $(OUTPUT_123)

.PHONY: clean libvgmstream.a $(TARGET_EXT_LIBS)

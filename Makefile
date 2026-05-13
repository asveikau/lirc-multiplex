MAKEFILES_ROOT=submodules/makefiles/
LIBCOMMON_ROOT=submodules/common/
LIBPOLLSTER_ROOT=submodules/pollster/

TARGET:=lirc-multiplex

.PHONY: all
all: $(TARGET)

LDFLAGS+=-L$(LIBPOLLSTER_ROOT) -lpollster
LDFLAGS+=-L$(LIBCOMMON_ROOT) -lcommon

include ${LIBPOLLSTER_ROOT}Makefile.inc

LDFLAGS += $(CXXLIBS)

SRCFILES += \
   src/main.cc

OBJS += $(shell $(SRC2OBJ) $(SRCFILES))

$(TARGET): ${LIBCOMMON} ${LIBPOLLSTER} ${OBJS}
	${CXX} -o $@ ${OBJS} ${LDFLAGS}

.PHONY: clean
clean:
	rm -f $(TARGET) ${OBJS}
	rm -f ${LIBCOMMON} ${LIBCOMMON_OBJS}
	rm -f ${LIBPOLLSTER} ${LIBPOLLSTER_OBJS}

INCLUDES+= \
   -I$(LIBCOMMON_ROOT)include \
   -I$(LIBPOLLSTER_ROOT)include
CFLAGS+=$(INCLUDES)
CXXFLAGS+=$(INCLUDES)

LIB_ROOTS= \
   LIBCOMMON_ROOT=$(LIBCOMMON_ROOT) \
   LIBPOLLSTER_ROOT=$(LIBPOLLSTER_ROOT) \
   MAKEFILES_ROOT=$(MAKEFILES_ROOT)

.PHONY: depend
depend:
	env CFLAGS="$(CFLAGS)" $(LIB_ROOTS) \
	$(DEPEND) src/*.cc > depend.mk

-include depend.mk

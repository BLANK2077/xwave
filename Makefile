VERDI_HOME ?= $(shell echo $$VERDI_HOME)

# Check if VERDI_HOME is set
ifeq ($(VERDI_HOME),)
    $(error VERDI_HOME environment variable is not set)
endif

NPI_INC     = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC  = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB     = $(VERDI_HOME)/share/NPI/lib/LINUX64

CXX         = g++
CXXFLAGS    = -Wall -std=c++11 -I$(NPI_INC) -I$(NPI_L1_INC) -Isrc
LDFLAGS     = -L$(NPI_LIB) -Wl,-rpath-link,$(NPI_LIB) -lNPI -lnpiL1 -ldl -lrt -lz

EXE         = xwave
SRCS        = src/main.cpp \
              src/session/session_registry.cpp \
              src/session/session_manager.cpp \
              src/client/client.cpp \
              src/commands/cmd_session.cpp \
              src/commands/cmd_value.cpp \
              src/commands/cmd_list.cpp \
              src/commands/cmd_scope.cpp \
              src/commands/cmd_apb.cpp \
              src/commands/cmd_axi.cpp \
              src/commands/cmd_event.cpp \
              src/commands/cmd_ai.cpp \
              src/server/server.cpp \
              src/server/fsdb_value_reader.cpp \
              src/server/fsdb_scan_utils.cpp \
              src/list/list_manager.cpp \
              src/common/time_parser.cpp \
              src/apb/apb_manager.cpp \
              src/apb/apb_analyzer.cpp \
              src/axi/axi_manager.cpp \
              src/axi/axi_analyzer.cpp \
              src/event/event_manager.cpp \
              src/event/event_expr.cpp \
              src/event/event_analyzer.cpp

OBJS        = $(SRCS:.cpp=.o)

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(EXE) $(OBJS)

.PHONY: all clean

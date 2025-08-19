CFLAGS= -c -Wall -Werror -Wfatal-errors

OBJS = main.o tmr.o

# Platform-specific settings

ifeq ($(OS),Windows_NT)
    EXE = TFTP.exe
    LIBS = -lws2_32
    RM = del /Q
else
    EXE = TFTP
    LIBS =
    RM = rm -f
endif

all: $(EXE)

$(EXE): $(OBJS)
	gcc -o $(EXE) $(OBJS) $(LIBS)

%.o: %.c
	gcc $(CFLAGS) -o $@ $<

.PHONY: clean

clean:
	$(RM) $(EXE) *.o
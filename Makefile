CC=gcc
#OPTIONS    = -Wno-unused-function -Wextra -std=c++11
OPTIONS    = -Wno-unused-function -Wextra
#LIBS       = -lrt -lstdc++ -lsigc-2.0 -L../libpifacedigital/ -lpifacedigital -L../libmcp23s17/ -lmcp23s17
LIBS       = -lrt -lstdc++ -L../libpifacedigital/ -lpifacedigital -L../libmcp23s17/ -lmcp23s17
CFLAGS     = ${OPTIONS} ${LIBS} ${INCLUDES}

APP = server

all: server

server: server.cpp
	$(CC) -pthread server.cpp -o $(APP) $(CFLAGS)

clean:
	rm -f *.o


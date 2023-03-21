/*
  This example program provides a trivial server program that listens for TCP
  connections on port 9995.  When they arrive, it writes a short message to
  each client connection, and closes each connection once it is flushed.

  Where possible, it exits cleanly in response to a SIGINT (ctrl-c).
*/

#include "server.h"
#include <ws2tcpip.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

static const unsigned short PORT = 9995;

int
main(int argc, char **argv)
{
	server_start(PORT);
}
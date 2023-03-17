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
#if 0
	int i, count;
	uint8_t tmp[6];
	uint64_t n_mac = 0;
	uint64_t n_user = 0;

	// const char *mac = "a6-4c-5e-04-cc-53";
	const char *mac = "a6:4c:5e:04:cc:53";

#if 1
	count = sscanf(mac, "%2x%*[-:]%2x%*[-:]%2x%*[-:]%2x%*[-:]%2x%*[-:]%2x",
		((uint8_t *)&n_mac) + 0, ((uint8_t *)&n_mac) + 1,
		((uint8_t *)&n_mac) + 2, ((uint8_t *)&n_mac) + 3,
		((uint8_t *)&n_mac) + 4, ((uint8_t *)&n_mac) + 5);
	if (count != 6) {
		fprintf(stderr, "%s, Invalid mac(%s)!!!\n", __FUNCTION__, mac);
		return -1;
	}
#else
	count = sscanf(mac, "%2x%*[-:]%2x%*[-:]%2x%*[-:]%2x%*[-:]%2x%*[-:]%2x", &tmp[0],
		&tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]);
	if (count != 6) {
		fprintf(stderr, "%s, Invalid mac(%s)!!!\n", __FUNCTION__, mac);
		return -1;
	}
#endif
	void *adda = &n_mac;


	unsigned long ip = inet_addr("192.168.1.5");
	unsigned long hip = ntohl(ip);
	unsigned short port = htonl(3330);

	uint64_t ip42 = 0x112233;
	uint64_t ip4 = ntohll(ip42);

	const char *user = "userabsdefg";
	uint64_t user_64 = *((uint64_t *)user);
	uint64_t user_64_1 = user_64 & 0xffffffffffff0000;
	uint64_t user_64_2 = user_64 & htonll(0xffffffffffff0000);

	void *add = &user_64_2;

	n_user = (*((uint64_t *)user)) & htonll(0xffffffffffff0000);

	printf("%s, mac: %016llx, user: %016llx\n", __FUNCTION__, n_mac, n_user);
	
	printf("%d.%d.%d.%d", 
		(hip & 0xff000000) >> 24, 
		(hip & 0x00ff0000) >> 16,
		(hip & 0x0000ff00) >> 8, 
		(hip & 0x000000ff) >> 0);


	void *ip_addr = &ip;
	void *hip_addr = &hip;

	struct in_addr addr;
	addr.s_addr = ip;

	printf("%s, ip: %s, port: %d\n", __FUNCTION__, inet_ntoa(addr), ntohs(port));

	char buf[2];
	buf[0] = 0x2;
	buf[1] = 0x3;

	unsigned short port2 = 
		((unsigned short)buf[1] << 8) + (unsigned short)buf[0];
	
	unsigned short port3 = 0;
	char *data = (char *)&port3;
	for (int i = 0; i < 2; ++i)
		data[i] = buf[1-i];

	unsigned short port33 = ntohs(port3);
#endif

	server_start(PORT);

	return 0;
}
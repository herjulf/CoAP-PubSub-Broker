#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>

#define PORT 5483
#define BUFSIZE 2048

int
main ()
{
    struct sockaddr_in myaddr;
    struct sockaddr_in remaddr;
    socklen_t addrlen = sizeof(remaddr);
    int recvlen;
    int fd;
    unsigned char buf[BUFSIZE];

/* Create UDP socket */

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
	perror("Cannot create socket");
	return 0;
    }

/* Bind the socket to any valid IP address and specific port */

    memset((char *) &myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(PORT);

    if (bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0 ) {
	perror("Bind failed");
	return 0;
    }

    for (;;)
    {
	recvlen = recvfrom(fd, buf, BUFSIZE, 0, (struct sockaddr *) &remaddr,
			&addrlen);
	int bufrec = recvlen;
	printf("received %u bytes\n", bufrec);
	for (int i = 0; i < bufrec; i ++)
	{
	    printf("%c", buf[i]);
	}
	
    }

}

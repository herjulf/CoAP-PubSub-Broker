#include "cantcoap.h"
#include "nethelper.h"
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define BUF_LEN 512
#define URI_BUF_LEN 32

typedef int (*CoapHandler)(CoapPDU *pdu, int sockfd, struct sockaddr_storage *recvFrom);

// We could use a more efficient structure than linked list
typedef struct Resource {
    char const * uri;
    CoapHandler callback;
    Resource * next;
} Resource;

static Resource* head;

Resource* find_resource(char const* uri, Resource* head) {
	Resource* node = head;
	while (node != 0) {
		if (strcmp(uri, node->uri) == 0)
			break;
		node = node->next;
	}
	
	return node;
}

// General handler function
int handler(char const* payload, CoapPDU::ContentFormat content_format, CoapPDU *request, int sockfd, struct sockaddr_storage *recvFrom) {
	// We only use IPv4
	socklen_t addrLen = sizeof(struct sockaddr_in);
	
	// TODO claim back memory
	CoapPDU *response = new CoapPDU();
	response->setVersion(1);
	// OBS:
	response->setMessageID(request->getMessageID());
	response->setToken(request->getTokenPointer(), request->getTokenLength());

	switch(request->getCode()) {
		case CoapPDU::COAP_EMPTY:
			// send RST
			break;
		case CoapPDU::COAP_GET:
			response->setCode(CoapPDU::COAP_CONTENT);
			response->setContentFormat(content_format);
			response->setPayload((uint8_t*)payload, strlen(payload));
			break;
		/* TODO
		case CoapPDU::COAP_POST:
			response->setCode(CoapPDU::COAP_CREATED);
			break;
		case CoapPDU::COAP_PUT:
			response->setCode(CoapPDU::COAP_CHANGED);
			break;
		case CoapPDU::COAP_DELETE:
			response->setCode(CoapPDU::COAP_DELETED);
			// length 9 or 10 (including null)?
			response->setPayload((uint8_t*) "DELETE OK", 9);
			break;
		*/
	}

	switch(request->getType()) {
		case CoapPDU::COAP_CONFIRMABLE:
			response->setType(CoapPDU::COAP_ACKNOWLEDGEMENT);
			break;
		/* TODO
		case CoapPDU::COAP_NON_CONFIRMABLE:
			// fel: response->setType(CoapPDU::COAP_ACKNOWLEDGEMENT);
			break;
		case CoapPDU::COAP_ACKNOWLEDGEMENT:
			break;
		case CoapPDU::COAP_RESET:
			break;
		default:
			return 1;
	*/
	};

	ssize_t sent = sendto(
		sockfd,
		response->getPDUPointer(),
		response->getPDULength(),
		0,
		(sockaddr*) recvFrom,
		addrLen
	);

	if(sent < 0) {
		return 1;
	}
	
	return 0;
}

// =============== TEST ===============

// Regular CoAP DISCOVERY
// int discover_handler(CoapPDU *pdu, int sockfd, struct sockaddr_storage *recvFrom) {
// 	handler("</temperature>", CoapPDU::COAP_CONTENT_FORMAT_APP_LINK, pdu, sockfd, recvFrom);
// 	return 0;
// }

// CoAP PUBSUB Discovery
// QUESTION: Citation marks around rt or not? Compare with Herjulf impl coap.c
int discover_handler(CoapPDU *pdu, int sockfd, struct sockaddr_storage *recvFrom) {
	handler("</ps/>;\"rt=core.ps\";ct=40", CoapPDU::COAP_CONTENT_FORMAT_APP_LINK, pdu, sockfd, recvFrom);
	return 0;
}

int temperature_handler(CoapPDU *pdu, int sockfd, struct sockaddr_storage *recvFrom) {
	handler("19", CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN, pdu, sockfd, recvFrom);
	return 0;
}

int humidity_handler(CoapPDU *pdu, int sockfd, struct sockaddr_storage *recvFrom) {
	handler("75%", CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN, pdu, sockfd, recvFrom);
	return 0;
}

void test_make_resources() {
        // Regular CoAP DISCOVERY
	// Resource* discover = (Resource*) malloc(sizeof (Resource));
	// discover->uri = "/.well-known/core";
	// discover->callback = discover_handler;

        // CoAP PUB/SUB DISCOVERY
	Resource* discover = (Resource*) malloc(sizeof (Resource));
	discover->uri = "/.well-known/core?rt=core.ps";
	discover->callback = discover_handler;

	Resource* temperature = (Resource*) malloc(sizeof (Resource));
	temperature->uri = "/temperature";
	temperature->callback = temperature_handler;
	
	Resource* humidity = (Resource*) malloc(sizeof (Resource));
	humidity->uri = "/humidity";
	humidity->callback = humidity_handler;
	
	discover->next = temperature;
	temperature->next = humidity;
	humidity->next = 0;
	head = discover;
}

// ============== /TEST ===============


int main(int argc, char **argv) {
    if (argc < 3)
    {
        printf("USAGE: %s address port", argv[0]);
        return -1;
    }
    
    // ==== TEST ====
    test_make_resources();
    // === /TEST ====
    
    char* str_address = argv[1];
    char* str_port = argv[2];
    struct addrinfo *addr;
    
    int ret = setupAddress(str_address, str_port, &addr, SOCK_DGRAM, AF_INET);
    
    if(ret != 0) {
        return -1;
    }
    
    int sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    
    if(bind(sockfd, addr->ai_addr, addr->ai_addrlen) != 0) {
		return -1;
    }
    
    char buffer[BUF_LEN];
    struct sockaddr_storage recvAddr;
    socklen_t recvAddrLen = sizeof(struct sockaddr_storage);
    
    char uri_buffer[URI_BUF_LEN];
    int recvURILen;
    
    CoapPDU *recvPDU = new CoapPDU((uint8_t*)buffer, BUF_LEN, BUF_LEN);
    
    for (;;) {
        ret = recvfrom(sockfd, &buffer, BUF_LEN, 0, (sockaddr*)&recvAddr, &recvAddrLen);
        if (ret == -1) {
            return -1;
        }
		
		printf("DEBUGGING: BEFORE ret > LEN\n");
        
        if(ret > BUF_LEN) {
            continue;
        }
        
		printf("DEBUGGING: BEFORE validate()\n");
        
        recvPDU->setPDULength(ret);
        if(recvPDU->validate() != 1) {
            continue;
        }
        
		printf("DEBUGGING: BEFORE getURI()\n");
        
        // depending on what this is, maybe call callback function
		if(recvPDU->getURI(uri_buffer, URI_BUF_LEN, &recvURILen) != 0) {
			continue;
		}
		
		printf("DEBUGGING: recvURILen = %i\n", recvURILen);
		
		if(recvURILen > 0) {
			Resource* r = find_resource(uri_buffer, head);
			r->callback(recvPDU, sockfd, &recvAddr);
		}
		
		// code 0 indicates an empty message, send RST
		// && or ||, pdu length is size of whole packet?
		if(recvPDU->getPDULength() == 0 || recvPDU->getCode() == 0) {
			
        }
    }
    
    return 0;
}

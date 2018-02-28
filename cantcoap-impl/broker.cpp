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

// TODO: Add max-age
typedef struct Resource {
    const char* uri;
    const char* rt;
    int ct;
    const char* val;
    Resource * children;
    Resource * next;
} Resource;

static Resource* head;

Resource* find_resource(const char* uri, Resource* head) {
	Resource* node = head;
	while (node != NULL) {
		if (strstr(uri, node->uri) == uri)
			break;
		node = node->next;
	}

// TODO: remove strlen(node->uri), it's not needed, replace w "uri"	
	if (node != NULL) {
	    Resource* node2 = find_resource(uri + strlen(node->uri), node->children);
	    node = node2 != NULL ? node2 : node;
    }
	return node;
}

/*const char* extract_queries(const char* uri) {
    char* q = strstr(uri, "?");
    if (q != NULL) {
        
    }
}*/

// General handler function
int handler(Resource* resource, const char* queries, CoapPDU *request, int sockfd, struct sockaddr_storage *recvFrom) {
    const char* payload = resource->val;
    int content_format = resource->ct;
	socklen_t addrLen = sizeof(struct sockaddr_in); // We only use IPv4
	
	CoapPDU *response = new CoapPDU();
	response->setVersion(1);
	response->setMessageID(request->getMessageID()); // OBS
	response->setToken(request->getTokenPointer(), request->getTokenLength());

	switch(request->getCode()) {
		case CoapPDU::COAP_EMPTY: // send RST
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

// TODO IMPLEMENT NON-CONFIRMABLE
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
    
    delete response;
    
	if(sent < 0) {
		return 1;
	}
	
	return 0;
}

// =============== TEST ===============

// CoAP PUBSUB Discovery
// QUESTION: Citation marks around rt or not? Compare with Herjulf impl coap.c

void test_make_resources() {
    // CoAP PUB/SUB DISCOVERY
	Resource* discover = (Resource*) malloc(sizeof (Resource));
	discover->uri = "/.well-known/core"; //?rt=core.ps;rt=core.ps.discover";
	discover->rt = "";
	discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
	discover->val = "";
    
    Resource* ps = (Resource*) malloc(sizeof (Resource));
	ps->uri = "/ps/";
	temperature->rt = "";
	temperature->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
	temperature->val = "";
    
	Resource* temperature = (Resource*) malloc(sizeof (Resource));
	temperature->uri = "/ps/temperature";
	temperature->rt = "temperature";
	temperature->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
	temperature->val = "19";
	
	Resource* humidity = (Resource*) malloc(sizeof (Resource));
	humidity->uri = "/ps/humidity";
	humidity->rt = "humidity";
	humidity->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
	humidity->val = "75%";
	
	discover->next = ps;
	ps->children = temperature;
	temperature->children = NULL;
	humidity->children = NULL;
	temperature->next = humidity;
	ps->next = NULL;
	head = discover;
}

// ============== /TEST ===============
// TODO: Extract queries
void handle_request(CoapPDU *recvPDU, int sockfd, struct sockaddr_storage* recvAddr) {
	Resource* resource = find_resource(uri_buffer, head);
	char* queries = strstr(uri_buffer, "?");
    if (queries != NULL) {
        queries++;
    }
    
    handler(resource, queries, recvPDU, sockfd, &recvAddr);
}

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
    
    while (1) {
        ret = recvfrom(sockfd, &buffer, BUF_LEN, 0, (sockaddr*)&recvAddr, &recvAddrLen);
        if (ret == -1) {
            return -1;
        }
        
        if(ret > BUF_LEN) {
            continue;
        }
        
        recvPDU->setPDULength(ret);
        if(recvPDU->validate() != 1) {
            continue;
        }
        
        // depending on what this is, maybe call callback function
		if(recvPDU->getURI(uri_buffer, URI_BUF_LEN, &recvURILen) != 0) {
			continue;
		}
		
		if(recvURILen > 0) {
			handle_request(uri_buffer, recvPDU, sockfd, &recvAddr);
		}
		
		// code 0 indicates an empty message, send RST
		// && or ||, pdu length is size of whole packet?
		if(recvPDU->getPDULength() == 0 || recvPDU->getCode() == 0) {
			
        }
    }
    
    return 0;
}

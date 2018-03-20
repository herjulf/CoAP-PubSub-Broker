#include "cantcoap.h"
#include "nethelper.h"
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <cstdlib>
#include <stdio.h>
#include <sstream>
#include <iostream>
#include "yuarel.h"

#define BUF_LEN 512
#define URI_BUF_LEN 128
#define QRY_NUM 6
#define PS_DISCOVERY "/.well-known/core?rt=core.ps"
#define DISCOVERY "/.well-known/core"

/* TODO (Wrong replies)
coap get "coap://127.0.0.1:5683/.well-known/core/?ct=0&rt=temperature"
coap get "coap://127.0.0.1:5683/.well-known/core/?rt=temperature&ct=1"
coap get "coap://127.0.0.1:5683/.well-known/core/?rt=temperature&ct=0"

; and
, or?
*/

// TODO: Remove this typedef?
typedef int (*CoapHandler)(CoapPDU *pdu, int sockfd, struct sockaddr_storage *recvFrom);

// TODO: Add max-age
typedef struct Resource {
    const char* uri;
    const char* rt;
    CoapPDU::ContentFormat ct;
    const char* val;
    Resource * children;
    Resource * next;
} Resource;

static Resource* head;
static Resource* ps_discover;

// Generic list
template<typename T> 
struct Item {
    T val;
    struct Item<T>* next;
};

Resource* find_resource(const char* uri, Resource* head) {
	Resource* node = head;
	while (node != NULL) {
		if (strstr(uri, node->uri) == uri)
			break;
		node = node->next;
	}

	if (node != NULL) {
	    Resource* node2 = find_resource(uri, node->children);
	    node = node2 != NULL ? node2 : node;
    }
	return node;
}

void find_resource_by_rt(const char* rt, Resource* head, struct Item<Resource*>* &item, bool visited) {
    if (!visited) {
	    if (head->children != NULL) {
	        find_resource_by_rt(rt, head->children, item, visited);
	    } else if (strcmp(head->rt, rt) == 0) {
	        struct Item<Resource*>* new_item = new struct Item<Resource*>();
	        new_item->val = head;
	        new_item->next = NULL;
	        if (item != NULL) {
	            new_item->next = item;
	        }
	        item = new_item;
	    }
	
	    if (head->next != NULL) {
	        find_resource_by_rt(rt, head->next, item, visited);
	    }
    } else if (item != NULL) {
        struct Item<Resource*>* current = item;
        bool is_head = true;
        bool head_removed = false;
        
        while (current) {
            if (strcmp(current->val->rt, rt) != 0) {
                struct Item<Resource*>* tmp = current->next;
                delete current;
                current = tmp;
                
                if (is_head) {
                    item = current;
                	head_removed = true;
                }
            } else {
                current = current->next;
            }
            
            if (head_removed) {
            	head_removed = false;
            } else if (is_head) {
            	is_head = false;
            }
        }
    }
}

void find_resource_by_ct(int ct, Resource* head, struct Item<Resource*>* &item, bool visited) {
    if (!visited) {
		if (head->children != NULL) {
			find_resource_by_ct(ct, head->children, item, visited);
		} else if (head->ct == ct) {
			struct Item<Resource*>* new_item = new struct Item<Resource*>();

			new_item->val = head;
			new_item->next = NULL;
			if (item != NULL) {
				new_item->next = item; 
			}
			item = new_item;
		}

		if (head->next != NULL) {
			find_resource_by_ct(ct, head->next, item, visited);
		}

    } else if (item != NULL) {
        struct Item<Resource*>* current = item;
        bool is_head = true;
        bool head_removed = false;
        
        while (current) {
            if (current->val->ct != ct) {
                struct Item<Resource*>* tmp = current->next;
                delete current;
                current = tmp;
                
                if (is_head) {
                    item = current;
                	head_removed = true;
                }
            } else {
                current = current->next;
            }
            
            if (head_removed) {
            	head_removed = false;
            } else if (is_head) {
            	is_head = false;
            }
        }
    }
}

struct yuarel_param* find_query(struct yuarel_param* params, char* key) {
    while (params != NULL) {
        if(strcmp(params->key, key) == 0)
            return params;
        params++;
    }
    
    return NULL;
}

// General handler function
int handler(Resource* resource, struct yuarel_param* queries, int num_queries, CoapPDU *request, int sockfd, struct sockaddr_storage recvFrom) {
    const char* payload = NULL;
    std::string payload_str;
    CoapPDU::ContentFormat content_format = resource->ct;
	socklen_t addrLen = sizeof(struct sockaddr_in); // We only use IPv4
	
	if (strcmp(resource->uri, PS_DISCOVERY) == 0) {
	    payload = resource->val;
	} else if (strstr(resource->uri, DISCOVERY) != NULL) {
		std::stringstream* val = new std::stringstream();
	    struct yuarel_param* query = queries;
        struct Item<Resource*>* item = NULL;
        bool visited = false;
        
        for (int i = 0; i < num_queries; i++) {
            if (strcmp(queries[i].key, "rt") == 0) {
                find_resource_by_rt(queries[i].val, head, item, visited);
                visited = true;
            } else if (strcmp(queries[i].key, "ct") == 0) {
                find_resource_by_ct(std::strtol(queries[i].val,NULL,10), head, item, visited);
                visited = true;
            }
        }
        
        struct Item<Resource*>* current = item;
        while(current) {
        	*val << "<" << current->val->uri << ">;rt=\"" 
        		<< current->val->rt << "\";ct=" << current->val->ct;
        	current = current->next;
        	
        	if (current) {
        		*val << ",";
        	}
        }
        
        if (num_queries >= 0) {
        	payload_str = val->str();
        	payload = payload_str.c_str();
       	} else {
       		payload = resource->val;
       	}
       	
        delete val;
	} else {
	    payload = resource->val;
	}
	
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
		(struct sockaddr*) &recvFrom,
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
	ps_discover = (Resource*) malloc(sizeof (Resource));
	ps_discover->uri = PS_DISCOVERY; //?rt=core.ps;rt=core.ps.discover;ct=40";
	ps_discover->rt = "";
	ps_discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
	ps_discover->val = "</ps/>;rt=core.ps;rt=core.ps.discover;ct=40";
	ps_discover->next = NULL;
	ps_discover->children = NULL;
    
	Resource* discover = (Resource*) malloc(sizeof (Resource));
	discover->uri = DISCOVERY;
	discover->rt = "";
	discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
	discover->val = "</temperature>;</humidity>"; // TODO
	discover->next= NULL;
	discover->children = NULL;
    
    Resource* ps = (Resource*) malloc(sizeof (Resource));
	ps->uri = "/ps/";
	ps->rt = "";
	ps->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
	ps->val = "";
	ps->next= NULL;
	ps->children = NULL;
    
	Resource* temperature = (Resource*) malloc(sizeof (Resource));
	temperature->uri = "/ps/temperature";
	temperature->rt = "temperature";
	temperature->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
	temperature->val = "19";
	temperature->next= NULL;
	temperature->children = NULL;
	
	Resource* humidity = (Resource*) malloc(sizeof (Resource));
	humidity->uri = "/ps/humidity";
	humidity->rt = "humidity";
	humidity->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
	humidity->val = "75%";
	humidity->next= NULL;
	humidity->children = NULL;
	
	ps_discover->next = discover;
	discover->next = ps;
	ps->children = temperature;
	temperature->next = humidity;
	head = ps_discover;
}

// ============== /TEST ===============
// TODO: Extract queries
void handle_request(char *uri_buffer, CoapPDU *recvPDU, int sockfd, struct sockaddr_storage recvAddr) {
	Resource* resource = NULL;
	if (strcmp(uri_buffer, PS_DISCOVERY) == 0) {
		resource = ps_discover;
	} else {
		resource = find_resource(uri_buffer, head);
	}
	
	if (resource == NULL)
		return; // SEND RST, RETURN
	
	char* queries = strstr(uri_buffer, "?");
    if (queries != NULL) {
        queries++;
    }
    
    struct yuarel_param params[QRY_NUM];
    int q = yuarel_parse_query(queries, '&', params, QRY_NUM);
    
    handler(resource, params, q, recvPDU, sockfd, recvAddr);
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
		
		// uri_buffer[recvURILen] = '\0';
		
		if(recvURILen > 0) {
			handle_request(uri_buffer, recvPDU, sockfd, recvAddr);
		}
		
		// code 0 indicates an empty message, send RST
		// && or ||, pdu length is size of whole packet?
		if(recvPDU->getPDULength() == 0 || recvPDU->getCode() == 0) {
			
        }
    }
    
    return 0;
}

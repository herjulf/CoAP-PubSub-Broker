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
#define OPT_NUM 6
#define PS_DISCOVERY "/.well-known/core?rt=core.ps"
#define DISCOVERY "/.well-known/core"

/* Testing
coap get "coap://127.0.0.1:5683/.well-known/core?ct=0&rt=temperature"
coap get "coap://127.0.0.1:5683/.well-known/core?rt=temperature&ct=1"
coap get "coap://127.0.0.1:5683/.well-known/core?rt=temperature&ct=0"
echo "<topic1>" | coap post "coap://127.0.0.1:5683/ps"
coap get "coap://127.0.0.1:5683/.well-known/core"
*/

// TODO forbid ps(LINK)->topic1(TEXT)->topic2(TEXT)

// TODO: Add max-age
// IMPORTANT: All uri should be dynamically allocated
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
static Resource* discover;

// Generic list
template<typename T> 
struct Item {
    T val;
    struct Item<T>* next;
};

void get_all_resources(struct Item<Resource*>* &item, Resource* head) {
	if (head->children != NULL) {
		get_all_resources(item, head->children);
	} else {
		struct Item<Resource*>* new_item = new struct Item<Resource*>();
		new_item->val = head;
		new_item->next = item;
		item = new_item;
	}

	if (head->next != NULL) {
		get_all_resources(item, head->next);
	}
}

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

void update_discovery(Resource* discover) {
    // TODO Do not include regular and ps discover in discover response
	struct Item<Resource*>* current = NULL;
	get_all_resources(current, head);
	std::stringstream val("");
    while(current) {
    	val << "<" << current->val->uri << ">;rt=\"" 
    		<< current->val->rt << "\";ct=" << current->val->ct;
    		
    	struct Item<Resource*>* tmp = current->next;
    	current = tmp;
    	
    	if (current) {
    		val << ",";
    	}
    }
    
    std::string s = val.str();
    char* d = (char*) malloc(s.length());
    std::memcpy(d, s.c_str(), s.length());
    discover->val = d;
}

void get_handler(Resource* resource, std::stringstream* &payload, struct yuarel_param* queries, int num_queries) {
    payload = NULL;
    std::stringstream* val = new std::stringstream();
	
	if (strcmp(resource->uri, PS_DISCOVERY) == 0) {
	    *val << resource->val;
	} else if (strstr(resource->uri, DISCOVERY) != NULL) {
		if (num_queries < 1) {
		    update_discovery(discover);
			*val << resource->val;
			payload = val;
			return;
		}
		
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
        		
        	struct Item<Resource*>* tmp = current->next;
        	delete current;
        	current = tmp;
        	
        	if (current) {
        		*val << ",";
        	}
        }
       	
        //delete val;
	} else {
	    *val << resource->val;
	}
	
	payload = val;
}

// TODO This only implements CREATE
Resource* post_handler(Resource* resource, const char* in, char* &payload, struct yuarel_param* queries, int num_queries) {
    std::stringstream* payload_stream = new std::stringstream();
    char * p = strchr(in, '<');
    int start = (int)(p-in);
    p = strchr(in, '>');
    int end = (int)(p-in);
    p = strchr(in, ';');
	struct yuarel_param params[OPT_NUM];
    int q = -1;
    if (p != NULL)
        yuarel_parse_query(p+1, ';', params, OPT_NUM);  // TODO, fix options
	
	int uri_len = strlen(resource->uri);
	int len = end-start+uri_len+1;
	char* resource_uri = (char*) malloc(len);
	memcpy(resource_uri, resource->uri, uri_len);
	resource_uri[uri_len] = '/';
	memcpy(resource_uri + uri_len + 1, in + start + 1, end-start-1);
	resource_uri[len-1] = '\0';
	payload = resource_uri;
	
	Resource* new_resource = (Resource*) malloc(sizeof (Resource));
	new_resource->uri = resource_uri;
	new_resource->rt = "";  // TODO Options (Both)
	new_resource->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN; 
	new_resource->val = "val_hard";
    new_resource->next = resource->children;
    resource->children = new_resource;
	new_resource->children = NULL;
	
	update_discovery(discover); // TODO Behöver vi det?
	return new_resource;
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
    
	discover = (Resource*) malloc(sizeof (Resource));
	discover->uri = DISCOVERY;
	discover->rt = "";
	discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
	discover->val="";
	discover->next= NULL;
	discover->children = NULL;
    
    Resource* ps = (Resource*) malloc(sizeof (Resource));
	ps->uri = "/ps";
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
	
    update_discovery(discover);
}

// ============== /TEST ===============
// TODO: Extract queries
int handle_request(char *uri_buffer, CoapPDU *recvPDU, int sockfd, struct sockaddr_storage recvAddr) {
	Resource* resource = NULL;
	if (strcmp(uri_buffer, PS_DISCOVERY) == 0) {
		resource = ps_discover;
	} else {
		resource = find_resource(uri_buffer, head);
	}
	
	if (resource == NULL) 
		return 1; // SEND RST, RETURN
	
	char* queries = strstr(uri_buffer, "?");
    if (queries != NULL) {
        queries++;
    }
    
    struct yuarel_param params[OPT_NUM];
    int q = yuarel_parse_query(queries, '&', params, OPT_NUM);
	socklen_t addrLen = sizeof(struct sockaddr_in); // We only use IPv4
	
	CoapPDU *response = new CoapPDU();
	response->setVersion(1);
	response->setMessageID(recvPDU->getMessageID()); // OBS
	response->setToken(recvPDU->getTokenPointer(), recvPDU->getTokenLength());
	
    switch(recvPDU->getCode()) {
		case CoapPDU::COAP_EMPTY: { // send RST
			break;
		}
		case CoapPDU::COAP_GET: {
			std::stringstream* payload_stream = NULL;
			get_handler(resource, payload_stream, params, q);
			std::string payload_str = payload_stream->str();
			delete payload_stream;
			char payload[payload_str.length()];
			std::strcpy(payload, payload_str.c_str());
			response->setCode(CoapPDU::COAP_CONTENT);
			response->setContentFormat(resource->ct);
			response->setPayload((uint8_t*)payload, strlen(payload));
			break;
		}
		case CoapPDU::COAP_POST: {
			char* payload = NULL;
			post_handler(resource, (const char*)recvPDU->getPayloadPointer(), payload, params, q);
			response->setCode(CoapPDU::COAP_CREATED);
			response->setContentFormat(resource->ct);
			response->setPayload((uint8_t*)payload, strlen(payload));
			break;
		}
		/* TODO 
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
	switch(recvPDU->getType()) {
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
		(struct sockaddr*) &recvAddr,
		addrLen
	);
    
    delete response;
    
	if(sent < 0) {
		return 1;
	}
	
	return 0;
}

int main(int argc, char **argv) { 
    if (argc < 3)
    {
        printf("USAGE: %s address port\n", argv[0]);
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

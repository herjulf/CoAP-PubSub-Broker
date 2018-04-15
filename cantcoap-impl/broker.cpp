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
coap get "coap://127.0.0.1:5683/ps/?rt=temperature"

echo "<topic1>;ct=40" | coap post "coap://127.0.0.1:5683/ps"
echo "22" | coap put "coap://127.0.0.1:5683/ps/topic1"
coap get "coap://127.0.0.1:5683/ps/topic1"
*/

// TODO forbid ps(LINK)->topic1(TEXT)->topic2(TEXT)
// Meaning, only allow leaves to have values.

// TODO: Add max-age
// IMPORTANT: All uri should be dynamically allocated
// IMPORTANT: rt's will be dynamically allocated, probably val's too
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
        } else if (head->rt != NULL && strcmp(head->rt, rt) == 0) {
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
            if (current->val->rt != NULL && strcmp(current->val->rt, rt) != 0) {
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
    struct Item<Resource*>* current = NULL;
    get_all_resources(current, head);
    std::stringstream val("");
    while(current) {
        if (current->val->rt == NULL) {
            val << "<" << current->val->uri << ">;ct=" << current->val->ct;
        } else {
            val << "<" << current->val->uri << ">;rt=\"" 
                << current->val->rt << "\";ct=" << current->val->ct;
        }
                
        struct Item<Resource*>* tmp = current->next;
        current = tmp;
        
        if (current) {
            val << ",";
        }
    }
    
    std::string s = val.str();
    char* d = new char[s.length()];
    std::memcpy(d, s.c_str(), s.length());
    discover->val = d;
}

CoapPDU::Code get_handler(Resource* resource, std::stringstream* &payload, struct yuarel_param* queries, int num_queries) {
    payload = NULL;
    std::stringstream* val = new std::stringstream();
    bool is_discovery = false;
    
    if (strcmp(resource->uri, PS_DISCOVERY) == 0) {
        *val << resource->val;
        payload = val;
        return CoapPDU::COAP_CONTENT;
    } else if (strstr(resource->uri, DISCOVERY) != NULL) {
        is_discovery = true;
        if (num_queries < 1) {
            update_discovery(discover);
            *val << resource->val;
            payload = val;
            return CoapPDU::COAP_CONTENT;
        }
    } else if (num_queries < 1) {
        *val << resource->val;
        payload = val;
        return CoapPDU::COAP_CONTENT;
    }
        
    struct yuarel_param* query = queries;
    struct Item<Resource*>* item = NULL;
    bool visited = false;
    Resource* source = is_discovery ? head : resource;
    
    for (int i = 0; i < num_queries; i++) {
        if (strcmp(queries[i].key, "rt") == 0) {
            find_resource_by_rt(queries[i].val, source, item, visited);
            visited = true;
        } else if (strcmp(queries[i].key, "ct") == 0) {
            find_resource_by_ct(std::strtol(queries[i].val,NULL,10), source, item, visited);
            visited = true;
        }
    }
    
    struct Item<Resource*>* current = item;
    while(current) {
        if (current->val->rt == NULL) {
            *val << "<" << current->val->uri << ">;ct=" << current->val->ct;
        } else {
            *val << "<" << current->val->uri << ">;rt=\"" 
                << current->val->rt << "\";ct=" << current->val->ct;
        }
                
        struct Item<Resource*>* tmp = current->next;
        delete current;
        current = tmp;
        
        if (current) {
            *val << ",";
        }
    }
        
    payload = val;
    return CoapPDU::COAP_CONTENT;
}

CoapPDU::Code post_create_handler(Resource* resource, const char* in, char* &payload, struct yuarel_param* queries, int num_queries) {
    std::stringstream* payload_stream = new std::stringstream();
    char * p = strchr(in, '<');
    int start = (int)(p-in);
    p = strchr(in, '>');
    int end = (int)(p-in);
    p = strchr(in, ';');
    
    int uri_len = strlen(resource->uri);
    int len = end-start+uri_len+1;
    char* resource_uri = new char[len];
    memcpy(resource_uri, resource->uri, uri_len);
    resource_uri[uri_len] = '/';
    memcpy(resource_uri + uri_len + 1, in + start + 1, end-start-1);
    resource_uri[len-1] = '\0';
    payload = resource_uri;
    
    Resource* next = resource->children;
    while (next != NULL) {
        if (strcmp(next->uri, resource_uri) == 0)
            return CoapPDU::COAP_FORBIDDEN;
        next = next->next;
    }
    
    Resource* new_resource = new Resource;
    new_resource->uri = resource_uri;
    new_resource->rt = NULL;
    new_resource->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
    
    struct yuarel_param params[OPT_NUM];
    int q = -1;
    if (p != NULL)
        q = yuarel_parse_query(p+1, ';', params, OPT_NUM);
    while (q > 0) {
        if (strcmp(params[--q].key, "rt") == 0) {
            char* rt = new char[strlen(params[q].val)+1];
            strcpy(rt, params[q].val);
            new_resource->rt = rt;
        } else if (strcmp(params[q].key, "ct") == 0) {
            new_resource->ct = static_cast<CoapPDU::ContentFormat>(atoi(params[q].val));
        } 
    }
    
    new_resource->val = NULL;
    new_resource->next = resource->children;
    resource->children = new_resource;
    new_resource->children = NULL;
    
    update_discovery(discover); // TODO Behöver vi det?
    return CoapPDU::COAP_CREATED;
}

CoapPDU::Code put_publish_handler(Resource* resource, CoapPDU* pdu) {
    // TODO: return COAP_NOT_FOUND?
    if (resource->children != NULL)
        return CoapPDU::COAP_NOT_FOUND;     
    
    CoapPDU::CoapOption* options = pdu->getOptions();
    int num_options = pdu->getNumOptions();
    while (num_options-- > 0) {
        if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_CONTENT_FORMAT) {
            int val = 0;
            uint8_t* option_value = options[num_options].optionValuePointer;
            for (int i = 0; i < options[num_options].optionValueLength; i++) {
                val <<= 8;
                val += *option_value;
                option_value++;
            }
            
            if (resource->ct != val)
                return CoapPDU::COAP_NOT_FOUND;
            break;
        }
    }
    
    const char* payload = (const char*)pdu->getPayloadPointer();
    char* val = new char[strlen(payload)+1];
    strcpy(val, payload);
    //const char* val = (const char*)pdu->getPayloadCopy();
    delete resource->val;
    resource->val = val;
    return CoapPDU::COAP_CHANGED;
}

// =============== TEST ===============

// CoAP PUBSUB Discovery
// QUESTION: Citation marks around rt or not? Compare with Herjulf impl coap.c

void test_make_resources() {
    ps_discover = new Resource;
    ps_discover->uri = PS_DISCOVERY; //?rt=core.ps;rt=core.ps.discover;ct=40";
    ps_discover->rt = NULL;
    ps_discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
    ps_discover->val = "</ps/>;rt=core.ps;rt=core.ps.discover;ct=40";
    ps_discover->next = NULL;
    ps_discover->children = NULL;

    discover = new Resource;
    discover->uri = DISCOVERY;
    discover->rt = NULL;
    discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
    discover->val = NULL;
    discover->next= NULL;
    discover->children = NULL;

    Resource* ps = new Resource;
    ps->uri = "/ps";
    ps->rt = NULL;
    ps->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
    ps->val = NULL;
    ps->next= NULL;
    ps->children = NULL;

    Resource* temperature = new Resource;
    temperature->uri = "/ps/temperature";
    temperature->rt = "temperature";
    temperature->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
    temperature->val = "19";
    temperature->next= NULL;
    temperature->children = NULL;

    Resource* humidity = new Resource;
    humidity->uri = "/ps/humidity";
    humidity->rt = "humidity";
    humidity->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
    humidity->val = "75%";
    humidity->next= NULL;
    humidity->children = NULL;

    ps->children = temperature;
    temperature->next = humidity;
    head = ps;

    update_discovery(discover);
}
// ============== /TEST ===============

int handle_request(char *uri_buffer, CoapPDU *recvPDU, int sockfd, struct sockaddr_storage recvAddr) {
    Resource* resource = NULL;
    if (strcmp(uri_buffer, PS_DISCOVERY) == 0) {
        resource = ps_discover;
    } else if (strstr(uri_buffer, DISCOVERY) != NULL) {
        resource = discover;
    } else {
        resource = find_resource(uri_buffer, head);
    }
    
    CoapPDU *response = new CoapPDU();
    response->setVersion(1);
    response->setMessageID(recvPDU->getMessageID()); // OBS
    response->setToken(recvPDU->getTokenPointer(), recvPDU->getTokenLength());
    socklen_t addrLen = sizeof(struct sockaddr_in); // We only use IPv4
        
    // TODO Check setContentFormat() invocations
    bool resource_found = true;
    if (resource == NULL) {
        response->setCode(CoapPDU::COAP_NOT_FOUND);
        response->setContentFormat(resource->ct);
        resource_found = false;
    }
        
    if (resource_found) {
        char* queries = strstr(uri_buffer, "?");
        if (queries != NULL) {
            queries++;
        }
        
        struct yuarel_param params[OPT_NUM];
        int q = yuarel_parse_query(queries, '&', params, OPT_NUM);
        
        switch(recvPDU->getCode()) {
            case CoapPDU::COAP_EMPTY: { // TODO: send RST
                break;
            }
            case CoapPDU::COAP_GET: {
                std::stringstream* payload_stream = NULL;
                CoapPDU::Code code = get_handler(resource, payload_stream, params, q);
                std::string payload_str = payload_stream->str();
                delete payload_stream;
                char payload[payload_str.length()];
                std::strcpy(payload, payload_str.c_str());
                response->setCode(code);
                response->setContentFormat(resource->ct);
                response->setPayload((uint8_t*)payload, strlen(payload));
                break;
            }
            case CoapPDU::COAP_POST: {
                char* payload = NULL;
                CoapPDU::Code code = post_create_handler(resource, (const char*)recvPDU->getPayloadPointer(), payload, params, q);
                response->setCode(code);
                response->setContentFormat(resource->ct);
                response->setPayload((uint8_t*)payload, strlen(payload));
                break;
            }
            case CoapPDU::COAP_PUT:
                CoapPDU::Code code = put_publish_handler(resource, recvPDU);
                response->setCode(code);
                response->setContentFormat(resource->ct);
                break;
            /* TODO case CoapPDU::COAP_DELETE:
                response->setCode(CoapPDU::COAP_DELETED);
                // length 9 or 10 (including null)?
                response->setPayload((uint8_t*) "DELETE OK", 9);
                break;
            */
        }
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

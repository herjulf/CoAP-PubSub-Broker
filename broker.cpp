#include "cantcoap.h"
#include "nethelper.h"
#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <cstdlib>
#include <stdio.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sstream>
#include <iterator>
#include <iostream>
#include <map>
#include "yuarel.h"

#define BUF_LEN 512
#define URI_BUF_LEN 128
#define OPT_NUM 6
#define PS_DISCOVERY "/.well-known/core?rt=core.ps"
#define DISCOVERY "/.well-known/core"

#define GC_TIMEOUT 1
#define MAX_AGE_DEFAULT 15

#define MAX_TOPIC 10
static int topic_count;

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

// Generic list
template<typename T> 
struct Item {
    T val;
    struct Item<T>* next;
};

struct SubscriberInfo {
    uint16_t subscriptions;
};

static bool subscriber_compare(const sockaddr_in& a, const sockaddr_in& b) {
    if (a.sin_addr.s_addr < b.sin_addr.s_addr)
        return true;
    else if (a.sin_addr.s_addr > b.sin_addr.s_addr)
        return false;
    else if (a.sin_port < b.sin_port)
        return true;
    else
        return false;
}

struct SubscriberComparator {
    bool operator() (const sockaddr_in& a, const sockaddr_in& b) const {
	    return subscriber_compare(a, b);
    }
};

typedef struct SubItem {
    std::map<sockaddr_in,struct SubscriberInfo,SubscriberComparator>::iterator it;
    uint32_t observe;
    uint64_t token;
    int token_len;
    struct SubItem* next;
} SubItem;

// All uri's, rt's and val's must be dynamically allocated
typedef struct Resource {
    const char* uri;
    const char* rt;
    CoapPDU::ContentFormat ct;
    const char* val;
    uint32_t expire;
    Resource * children;
    Resource * next;
    SubItem* subs;
} Resource;

static Resource* head;
static Resource* ps_discover;
static Resource* discover;
static int num_topics_deleted;
static std::map<sockaddr_in,struct SubscriberInfo,SubscriberComparator> subscribers;

int background = 0; // Run as daemon

void get_all_topics(struct Item<Resource*>* &item, Resource* head) {
    if (head->children != NULL) {
        get_all_topics(item, head->children);
    } else {
        struct Item<Resource*>* new_item = new struct Item<Resource*>();
        new_item->val = head;
        new_item->next = item;
        item = new_item;
    }

    if (head->next != NULL) {
        get_all_topics(item, head->next);
    }
}

Resource* find_resource(const char* uri, Resource* head, Resource** parent, Resource** prev) {
    Resource* node = head;
    Resource* p = NULL;
    while (node != NULL) {
        if (strstr(uri, node->uri) == uri) {
            char c = uri[strlen(node->uri)];
            if (c == '\0') {
                // *parent = head;
                *prev = p;
                return node;
            }
            if (c == '/')
                break;
        }
        p = node;
        node = node->next;
    }

    if (node != NULL && node->children != NULL) {
	*parent = head;
        node = find_resource(uri, node->children, parent, prev);
    } else if (node == NULL) {
	*prev = p;
	*parent = head;
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
            if (current->val->rt == NULL || strcmp(current->val->rt, rt) != 0) {
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
    get_all_topics(current, head);
    std::stringstream val("");
    while(current) {
        if (current->val->rt == NULL) {
            val << "<" << current->val->uri << ">;ct=" << current->val->ct;
        } else {
            val << "<" << current->val->uri << ">;rt=\"" 
                << current->val->rt << "\";ct=" << current->val->ct;
        }
                
        struct Item<Resource*>* tmp = current->next;
        delete current;
        current = tmp;
        
        if (current) {
            val << ",";
        }
    }
    
    delete discover->val;
    std::string s = val.str();
    char* d = new char[s.length()];
    std::memcpy(d, s.c_str(), s.length());
    discover->val = d;
}

void remove_all_resources(Resource* r, bool is_head, Resource* parent, Resource* prev, int sockfd, int addrLen);
int sockfd;

void run_gc(void) {
  struct Item<Resource*>* current = NULL;
  socklen_t addrLen = sizeof(struct sockaddr_in); // We only use IPv4
  get_all_topics(current, head);
  while(current) {

    printf("run_gc: uri=%s ct=%u rt=%u val=%s expire=%u current=%x next=%x child=%x\n",
	   current->val->uri, current->val->ct, current->val->rt, current->val->val,
	   current->val->expire, current, current->val->next, current->val->children);

    /* We shall not GC permanent extries (with 0) */
    if(current->val->expire) {
      if(current->val->expire > GC_TIMEOUT) {
	current->val->expire -= GC_TIMEOUT;
      }
      else {
	Resource *r, *prev, *parent;
	printf("GC ");
	r = find_resource(current->val->uri, head, &parent, &prev);
	printf(" r=%p parent=%p prev=%p", r, parent, prev);
	remove_all_resources(r, true, parent, prev, sockfd, addrLen);
	printf(" Done\n");
      }
    }
    struct Item<Resource*>* tmp = current->next;
    delete current;
    current = tmp;
  }
}


bool is_expired(Resource *r)
{
  // decrement 'expire'
  printf("run_gc: uri=%s ct=%u rt=%u val=%s expire=%u current=%x next=%x child=%x\n",
	 r->uri, r->ct, r->rt, r->val,
	 r->expire, r, r->next, r->children);

  /* We shall not GC permanent extries (with 0) */
  if(! r->expire)
    return 0;
    
  if(r->expire > GC_TIMEOUT) {
    r->expire -= GC_TIMEOUT;
    return 0;
  }
  return 1;
}
// parent är förälder till head
// prev är den föregående granne till head
void do_gc(Resource* head, Resource* parent, Resource* prev) {
  socklen_t addrLen = sizeof(struct sockaddr_in); // We only use IPv4
  if (! is_expired (head)) {

    if (head->children != NULL) {
      // 'children' har 'head' som förälder och ingen föregående granne
      do_gc(head->children, head, NULL);
    }

    if (head->next != NULL) {
      // 'parent' är fortfarande förälder till 'next'
      // och 'head' är den föregående grannen till 'next'
      do_gc(head->next, parent, head);
    }
  } else {
    // ...
    Resource* r = head->next;
    printf("run_gc: Remove uri=%s r=%p parent=%p prev=%p\n", head->uri, head, parent, prev);
    remove_all_resources(head, true, parent, prev, sockfd, addrLen);
    if (r) {    // Om r!=NULL
      do_gc(r, parent, prev); // Vi har ju tagit bort 'head' ur kedjan.
    }
  }
}

CoapPDU::Code get_discover_handler(Resource* resource, std::stringstream* &payload, struct yuarel_param* queries, int num_queries) {
    payload = NULL;
    std::stringstream* val = new std::stringstream();
    bool empty_stringstream = true;
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
        if (resource->val) { 
            *val << resource->val;
            payload = val;
            return CoapPDU::COAP_CONTENT;
        }
        delete val;
        return CoapPDU::COAP_NO_CONTENT;
    }
        
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
    if (current != NULL)
        empty_stringstream = false;
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
    
    if (empty_stringstream) {
        delete val;
        return CoapPDU::COAP_NOT_FOUND;
    }
    
    payload = val;
    return CoapPDU::COAP_CONTENT;
}

CoapPDU::Code get_subscription_handler(Resource* resource, CoapPDU* pdu, struct sockaddr_in* recvAddr, CoapPDU* response, std::stringstream* &payload, bool subscribe) {
    if (resource->ct == CoapPDU::COAP_CONTENT_FORMAT_APP_LINK)
        return CoapPDU::COAP_NOT_FOUND;    
    payload = new std::stringstream(); 
/*    CoapPDU::CoapOption* options = pdu->getOptions();
    int num_options = pdu->getNumOptions();
    bool ct_exists = false;
    while (num_options-- > 0) {
        if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_CONTENT_FORMAT) {
            ct_exists = true;
            uint32_t val = 0;   // TODO Why 32? 
            uint8_t* option_value = options[num_options].optionValuePointer;
            for (int i = 0; i < options[num_options].optionValueLength; i++) {
                val <<= 8;
                val += *option_value;
                option_value++;
            }
            
            if (resource->ct != val)
                return CoapPDU::COAP_UNSUPPORTED_CONTENT_FORMAT;
            break;
        }
    }
    
    if (!ct_exists)
        return CoapPDU::COAP_BAD_REQUEST;
*/   
    bool already_subscribed = false;
	auto it = subscribers.find(*recvAddr);
    SubItem* sub = NULL;
    if (it != subscribers.end()) {
        sub = resource->subs;
        SubItem* prev = sub;
        while (sub != NULL) {
            if (sub->it->first.sin_addr.s_addr == recvAddr->sin_addr.s_addr 
                && sub->it->first.sin_port == recvAddr->sin_port) {
                already_subscribed = true;
                
                if (!subscribe) {
                    if (prev != sub) {
                        prev->next = sub->next;
                    } else {
                        resource->subs = sub->next;
                    }
                    delete sub;
                }
                
                break;
            }
            
            prev = sub;
            sub = sub->next;
        }
    }
    
    if (already_subscribed && !subscribe) {
        struct SubscriberInfo& subscriber = subscribers[*recvAddr];
        subscriber.subscriptions--;
        if (subscriber.subscriptions == 0)
            subscribers.erase(*recvAddr);
    } else if (already_subscribed || subscribe) {
        if (!already_subscribed && subscribe) {
            struct SubscriberInfo& subscriber = subscribers[*recvAddr];
            
            SubItem* new_item = new SubItem;
            sub = new_item;
		    
		    if (it == subscribers.end())
                new_item->it = subscribers.find(*recvAddr);
            else
                new_item->it = it;
            new_item->next = resource->subs;
            resource->subs = new_item;
			
		    new_item->token = 0;
		    int len = pdu->getTokenLength();
		    uint8_t* token_pointer = pdu->getTokenPointer();
		    new_item->token_len = len;
		    int i = 0;
		    while (i < len) {
			    new_item->token += *token_pointer << 8*i;
			    token_pointer++;
			    i++;
		    }
		    new_item->observe = 0; // TODO: Implement it differently
		
		    if (it == subscribers.end()) {
			    subscriber.subscriptions = 1;						
		    } else {
			    subscriber.subscriptions++;
		    }
        } else if (already_subscribed && subscribe) {
            sub->token = 0;
            sub->token_len = pdu->getTokenLength();
            uint8_t* token_pointer = pdu->getTokenPointer();
            int i = 0;
		    while (i < sub->token_len) {
			    sub->token += *token_pointer << 8*i;
			    token_pointer++;
			    i++;
		    }
        }
        
        response->addOption(CoapPDU::COAP_OPTION_OBSERVE, 1, (uint8_t*)&sub->observe); // TODO FIX 1 to 3
        sub->observe++;
        sub->observe &= 0x7FFFFF;
    }
    
    if (resource->val != NULL) {
        *payload << resource->val;
        return CoapPDU::COAP_CONTENT;
    }
    
    return CoapPDU::COAP_NO_CONTENT;
}

CoapPDU::Code get_handler(Resource* resource, CoapPDU* pdu, struct sockaddr_in* recvAddr, CoapPDU* response, std::stringstream* &payload, struct yuarel_param* queries, int num_queries) {
    CoapPDU::CoapOption* options = pdu->getOptions();
    int num_options = pdu->getNumOptions();
    bool is_subscribe = false;
    bool is_unsubscribe = false;
    while (num_options-- > 0) {
        if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_OBSERVE) {
            is_subscribe = true;
            uint8_t* observe = options[num_options].optionValuePointer;
            for (int i = 0; i < options[num_options].optionValueLength; i++) {
                if (*observe != 0) {
                    is_subscribe = false;
                    break;
                }
                observe++;
            }
            
            if (!is_subscribe && *observe == 1)
                is_unsubscribe = true;
            break;
        }
    }
    
    if (is_subscribe)
        return get_subscription_handler(resource, pdu, recvAddr, response, payload, true);
    else if (is_unsubscribe)
        return get_subscription_handler(resource, pdu, recvAddr, response, payload, false);
    return get_discover_handler(resource, payload, queries, num_queries);
}

CoapPDU::Code post_create_handler(Resource* resource, const char* in, char* &payload, struct yuarel_param* queries, int num_queries) {
    if (resource->ct != CoapPDU::COAP_CONTENT_FORMAT_APP_LINK)
        return CoapPDU::COAP_BAD_REQUEST;
    if( topic_count == MAX_TOPIC)
        return CoapPDU::COAP_FORBIDDEN;

    char * p = (char *) strchr(in, '<');
    int start = (int)(p-in);
    p = (char *) strchr(in, '>');
    int end = (int)(p-in);
    p = (char *) strchr(in, ';');
    
    int uri_len = strlen(resource->uri);
    int len = end-start+uri_len+1;
    char* resource_uri = new char[len];
    memcpy(resource_uri, resource->uri, uri_len);
    resource_uri[uri_len] = '/';
    memcpy(resource_uri + uri_len + 1, in + start + 1, end-start-1);
    resource_uri[len-1] = '\0';
    
    Resource* next = resource->children;
    while (next != NULL) {
        if (strcmp(next->uri, resource_uri) == 0)
            return CoapPDU::COAP_FORBIDDEN;
        next = next->next;
    }
    
    char* rt = NULL;
    CoapPDU::ContentFormat ct;
    
    bool ct_exists = false;
    struct yuarel_param params[OPT_NUM];
    int q = -1;
    if (p != NULL)
        q = yuarel_parse_query(p+1, ';', params, OPT_NUM);
    while (q > 0) {
        if (strcmp(params[--q].key, "rt") == 0) {
            rt = new char[strlen(params[q].val)+1];
            strcpy(rt, params[q].val);
        } else if (strcmp(params[q].key, "ct") == 0) {
            ct_exists = true;
            ct = static_cast<CoapPDU::ContentFormat>(atoi(params[q].val));
        } 
    }
    
    if (!ct_exists) {
        return CoapPDU::COAP_BAD_REQUEST;
    }
    
    Resource* new_resource = new Resource;
    new_resource->uri = resource_uri;
    new_resource->rt = rt;
    new_resource->ct = ct;
    
    new_resource->val = NULL;
    new_resource->next = resource->children;
    resource->children = new_resource;
    new_resource->children = NULL;
    new_resource->subs = NULL;
    new_resource->expire=MAX_AGE_DEFAULT;
    payload = resource_uri;
    // update_discovery(discover);
    topic_count = topic_count + 1;
    return CoapPDU::COAP_CREATED;
}

CoapPDU::Code put_publish_handler(Resource* resource, CoapPDU* pdu) {
    if (resource->ct == CoapPDU::COAP_CONTENT_FORMAT_APP_LINK)
        return CoapPDU::COAP_NOT_FOUND;     

    // Retrieve ct through getOptions
    CoapPDU::CoapOption* options = pdu->getOptions();
    int num_options = pdu->getNumOptions();
    bool ct_exists = false;
    bool maxage_exists = false;
    uint32_t expire = 0;

    while (num_options-- > 0) {
        if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_CONTENT_FORMAT) {
            ct_exists = true;
            uint16_t val = 0;
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

        if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_MAX_AGE) {
            maxage_exists = true;
            uint8_t* option_value = options[num_options].optionValuePointer;
            for (int i = 0; i < options[num_options].optionValueLength; i++) {
                expire <<= 8;
                expire += *option_value;
		option_value++;
            }
        }
    }
    
    if (!ct_exists) {
        return CoapPDU::COAP_BAD_REQUEST;
    }

    //// 
    const char* payload = (const char*)pdu->getPayloadPointer();
    //char* payload = (char*)pdu->getPayloadPointer();
    //payload[URI_BUF_LEN] = '\0';
    char* val = new char[strlen(payload)+1];

    if(strlen(payload) == URI_BUF_LEN)
      printf("*** ERROR\n");

    strcpy(val, payload);
    //const char* val = (const char*)pdu->getPayloadCopy();
    delete[] resource->val;
    resource->val = val;
    if(maxage_exists)
      resource->expire = expire;
    else
      resource->expire = MAX_AGE_DEFAULT;

    return CoapPDU::COAP_CHANGED;
}

void remove_all_resources(Resource* resource, bool is_head, Resource* parent, Resource* prev, int sockfd, int addrLen) {
    if (resource->children != NULL) {
        remove_all_resources(resource->children, false, resource, NULL, sockfd, addrLen);
    }
    
    delete[] resource->uri;
    delete[] resource->rt;
    delete[] resource->val;
    
    SubItem* sub = resource->subs;
    SubItem* rm_sub;
    CoapPDU* response = new CoapPDU();
    response->setType(CoapPDU::COAP_CONFIRMABLE);
    response->setCode(CoapPDU::COAP_NOT_FOUND);
    while (sub != NULL) {
        response->setToken((uint8_t*)&sub->token, sub->token_len);
    	sendto(
    	    sockfd,
            response->getPDUPointer(),
            response->getPDULength(),
            0,
            (struct sockaddr*) &(sub->it->first),
            addrLen
        );
        
        struct SubscriberInfo& val = sub->it->second;
        if (--val.subscriptions == 0) {
            subscribers.erase(sub->it->first);
        } 
	    rm_sub = sub;
	    sub = sub->next;
	    delete rm_sub;
    }
    delete response;
    
    if (parent != NULL && parent->children == resource) {
        parent->children = resource->next;
    } else if (prev != NULL) {
        prev->next = resource->next;
    }
    
    if (!is_head) { 
        Resource *p = resource->next, *q;
        delete resource;
        while (p != NULL) {
            q = p->next;
            remove_all_resources(p, false, parent, prev, sockfd, addrLen);
            p = q;
        }
    } else {
        delete resource;
	// std::cerr<<"**************** is_head == TRUE Deleted resource in ELSE \n";
    }
    num_topics_deleted+=1;
    //if (topic_count >= 0 ){
    topic_count -= 1;
//}
    printf("num topics deleted = %d\n", num_topics_deleted);
}

CoapPDU::Code delete_remove_handler(Resource* resource, Resource* parent, Resource* prev, int sockfd, socklen_t addrLen) {
    /*if (parent != NULL && parent->children == resource) {
        parent->children = resource->next;
    } else if (prev != NULL) {
        prev->next = resource->next;
    }*/
    
    remove_all_resources(resource, true, parent, prev, sockfd, addrLen);
    //topic_count = topic_count - 1;
    return CoapPDU::COAP_DELETED;
}

void initialize() {
    ps_discover = new Resource;
    ps_discover->uri = PS_DISCOVERY; //?rt=core.ps;rt=core.ps.discover;ct=40";
    ps_discover->rt = NULL;
    ps_discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
    ps_discover->val = "</ps/>;rt=core.ps;rt=core.ps.discover;ct=40";
    ps_discover->next = NULL;
    ps_discover->children = NULL;
    ps_discover->subs = NULL;

    discover = new Resource;
    discover->uri = DISCOVERY;
    discover->rt = NULL;
    discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
    discover->val = NULL;
    discover->next= NULL;
    discover->children = NULL;
    discover->subs = NULL;

    Resource* ps = new Resource;
    ps->uri = "/ps";
    ps->rt = NULL;
    ps->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
    ps->val = NULL;
    ps->next= NULL;
    ps->children = NULL;
    ps->subs = NULL;

    /* Resource* temperature = new Resource;
    temperature->uri = "/ps/temperature";
    temperature->rt = "temperature";
    temperature->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
    temperature->val = "19";
    temperature->next= NULL;
    temperature->children = NULL;
    temperature->subs = NULL;

    Resource* humidity = new Resource;
    humidity->uri = "/ps/humidity";
    humidity->rt = "humidity";
    humidity->ct = CoapPDU::COAP_CONTENT_FORMAT_TEXT_PLAIN;
    humidity->val = "75%";
    humidity->next= NULL;
    humidity->children = NULL;
    humidity->subs = NULL;

    ps->children = temperature;
    temperature->next = humidity; */
    head = ps;

    // update_discovery(discover);
    num_topics_deleted = 0;
}

int handle_request(char *uri_buffer, CoapPDU *recvPDU, int sockfd, struct sockaddr_in* recvAddr) {
    Resource* resource = NULL;
    Resource* parent = NULL;
    Resource* prev = NULL;
    if (strcmp(uri_buffer, PS_DISCOVERY) == 0) {
        resource = ps_discover;
    } else if (strstr(uri_buffer, DISCOVERY) != NULL) {
        resource = discover;
    } else {
        resource = find_resource(uri_buffer, head, &parent, &prev);
/*	std::cerr<<"resource="<<resource<<", parent="<<parent<<", prev="<<prev<<std::endl;
    if(parent != NULL)
	std::cerr<<"************parent->uri: "<<parent->uri<<"\n";
    if(prev != NULL)
	std::cerr<<"************prev->uri: "<<prev->uri<<"\n"; */
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
        //response->setContentFormat(resource->ct);
        resource_found = false;
    }
    
    bool publish_to_all = false;    
    if (resource_found) {
        char* queries = strstr(uri_buffer, "?");
        if (queries != NULL) {
            queries++;
        }
        
        struct yuarel_param params[OPT_NUM];
        int q = yuarel_parse_query(queries, '&', params, OPT_NUM);
        
        switch(recvPDU->getCode()) {
            case CoapPDU::COAP_GET: {
                std::stringstream* payload_stream = NULL;
                CoapPDU::Code code = get_handler(resource, recvPDU, recvAddr, response, payload_stream, params, q);
                response->setCode(code);
                if (code == CoapPDU::COAP_CONTENT) {
                    std::string payload_str = payload_stream->str();
                    char payload[payload_str.length()+1];
                    std::strcpy(payload, payload_str.c_str());
                    response->setContentFormat(resource->ct);
                    response->setPayload((uint8_t*)payload, strlen(payload));
                }
                delete payload_stream;
                break;
            }
            case CoapPDU::COAP_POST: {
                char* payload = NULL;
                CoapPDU::Code code = post_create_handler(resource, (const char*)recvPDU->getPayloadPointer(), payload, params, q);
                response->setCode(code);
                if (payload != NULL) {
                    response->setContentFormat(CoapPDU::COAP_CONTENT_FORMAT_APP_LINK);
                    response->setPayload((uint8_t*)payload, strlen(payload));
                }
                break;
            }
            case CoapPDU::COAP_PUT: {
                CoapPDU::Code code = put_publish_handler(resource, recvPDU);
                response->setCode(code);
                //response->setContentFormat(resource->ct);
                publish_to_all = true;
                break;
             }
            case CoapPDU::COAP_DELETE: {
                CoapPDU::Code code = delete_remove_handler(resource, parent, prev, sockfd, addrLen);
                response->setCode(code);
                // length 9 or 10 (including null)?
                // response->setPayload((uint8_t*) "DELETE OK", 9);
                break;
            }
        }
    }

    // TODO Is the switch statement redundant here?
    switch(recvPDU->getType()) {
        case CoapPDU::COAP_CONFIRMABLE:
                response->setType(CoapPDU::COAP_ACKNOWLEDGEMENT);
                break;
    };
    ssize_t sent = sendto(
        sockfd,
        response->getPDUPointer(),
        response->getPDULength(),
        0,
        (struct sockaddr*) &(*recvAddr),
        addrLen
    );
    
    //delete response;
    if (publish_to_all) {
        SubItem* subscriber = resource->subs;
        while (subscriber != NULL) {
            response->setVersion(1);
            response->setMessageID(recvPDU->getMessageID()); // OBS
            response->setContentFormat(resource->ct);
            response->setType(CoapPDU::COAP_CONFIRMABLE);
            response->setCode(CoapPDU::COAP_CONTENT);
            response->setPayload(recvPDU->getPayloadPointer(), recvPDU->getPayloadLength());
            response->setToken((uint8_t*)&subscriber->token, subscriber->token_len);
            response->addOption(CoapPDU::COAP_OPTION_OBSERVE, 1, (uint8_t*)&subscriber->observe); // TODO FIX 1 to 3
            sendto(
                sockfd,
                response->getPDUPointer(),
                response->getPDULength(),
                0,
                (struct sockaddr*) &(subscriber->it->first),
                addrLen
            );
            
            response->reset();
            subscriber->observe++;
            subscriber->observe &= 0x7FFFFF; // TODO: Implement it differently
            subscriber = subscriber->next;
        }   
    }
    
    delete response;
    if(sent < 0) {
        return 1;
    }
    
    return 0;
}

int main(int argc, char **argv) { 

  fd_set read_fds, write_fds;

    if (argc < 3)
    {
        printf("USAGE: %s address port\n", argv[0]);
        return -1;
    }
    
    initialize();
    
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
    struct sockaddr_in recvAddr;
    socklen_t recvAddrLen = sizeof(struct sockaddr_in);
    
    char uri_buffer[URI_BUF_LEN];
    int recvURILen;
    
    CoapPDU *recvPDU = new CoapPDU((uint8_t*)buffer, BUF_LEN, BUF_LEN);
    
      struct timeval tv;
      tv.tv_sec = GC_TIMEOUT;
      tv.tv_usec = 0;

      if(background) {
	pid_t pid, sid;    
	pid = fork();
      
	if (pid < 0) {
	  std::cerr << "Failed to fork, error code [" << pid << "]. Exitting";
	  return EXIT_FAILURE;
	} else if(pid > 0) {
	  return EXIT_SUCCESS;
      }
	
	umask(0);
	/* Set new signature ID for the child */
	
	sid = setsid();
      
	if (sid < 0) {
	  std::cerr << "Failed to setsid, error code [" << sid << "]. Exiting";
	  return EXIT_FAILURE;
	}

	if ((chdir("/")) < 0) {
	  std::cerr << "Failed to change directory to /. Exiting";
	  return EXIT_FAILURE;
	}
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
      }

      while (1) {

      FD_ZERO(&read_fds);
      FD_ZERO(&write_fds);
      FD_SET(sockfd, &read_fds);

      int n = select(sockfd+1, &read_fds, NULL, 0, &tv);
      
      if(n < 0) {
	perror("ERROR Server : select()\n");
	close(sockfd);
	exit(1);
      }
      if (n == 0)  {
	tv.tv_sec = GC_TIMEOUT;
	tv.tv_usec = 0;
	/* TIMEOUT */
	//run_gc();
	do_gc(head, NULL, NULL);
	continue;
      }
      if(FD_ISSET(sockfd, &read_fds)) {
        ret = recvfrom(sockfd, &buffer, BUF_LEN, 0, (sockaddr*)&recvAddr, &recvAddrLen);
	FD_CLR(sockfd, &read_fds);
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
	  // TODO: What if it's an incoming CON message with code COAP_EMPTY? Must not ACK be sent back?
	  if (recvPDU->getType() == CoapPDU::COAP_CONFIRMABLE && recvPDU->getCode() != CoapPDU::COAP_EMPTY)
	    handle_request(uri_buffer, recvPDU, sockfd, &recvAddr);
        }

        // code 0 indicates an empty message, send RST
        // && or ||, pdu length is size of whole packet?
        if(recvPDU->getPDULength() == 0 || recvPDU->getCode() == 0) {

        }
	// Necessary to reset PDU to prevent garbage values residing in next message
	recvPDU->reset();
      }
    }
    return 0;
}

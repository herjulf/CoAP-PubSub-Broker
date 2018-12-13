diff --git a/broker.cpp b/broker.cpp
index 02cd178..e28a956 100644
--- a/broker.cpp
+++ b/broker.cpp
@@ -22,6 +22,9 @@
 #define PS_DISCOVERY "/.well-known/core?rt=core.ps"
 #define DISCOVERY "/.well-known/core"
 
+#define GC_TIMEOUT 1
+#define MAX_AGE_DEFAULT 300
+
 /* Testing
 coap get "coap://127.0.0.1:5683/.well-known/core?ct=0&rt=temperature"
 coap get "coap://127.0.0.1:5683/.well-known/core?rt=temperature&ct=1"
@@ -77,6 +80,7 @@ typedef struct Resource {
     const char* rt;
     CoapPDU::ContentFormat ct;
     const char* val;
+    uint32_t maxage;
     Resource * children;
     Resource * next;
     SubItem* subs;
@@ -87,6 +91,8 @@ static Resource* ps_discover;
 static Resource* discover;
 static std::map<sockaddr_in,struct SubscriberInfo,SubscriberComparator> subscribers;
 
+void __update(Resource* discover) ;
+
 void get_all_topics(struct Item<Resource*>* &item, Resource* head) {
     if (head->children != NULL) {
         get_all_topics(item, head->children);
@@ -260,6 +266,43 @@ void update_discovery(Resource* discover) {
     discover->val = d;
 }
 
+void __update(Resource* discover) {
+    struct Item<Resource*>* current = NULL;
+    get_all_topics(current, head);
+    std::stringstream val("");
+    while(current) {
+#if 0 
+      if (current->val->rt == NULL) {
+            val << "<" << current->val->uri << ">;ct=" << current->val->ct;
+        } else {
+            val << "<" << current->val->uri << ">;rt=\"" 
+                << current->val->rt << "\";ct=" << current->val->ct;
+        }
+#endif
+	printf("UPDATE: uri=%s ct=%u rt=%u val=%s maxage=%u current=%x next=%x child=%x\n",
+	       current->val->uri, current->val->ct, current->val->rt, current->val->val,
+	       current->val->maxage, current, current->val->next, current->val->children);
+
+	if(current->val->maxage > GC_TIMEOUT) {
+	  current->val->maxage -= GC_TIMEOUT;
+	}
+
+        struct Item<Resource*>* tmp = current->next;
+        delete current;
+        current = tmp;
+        
+        if (current) {
+            val << ",";
+        }
+    }
+    
+    delete discover->val;
+    std::string s = val.str();
+    char* d = new char[s.length()];
+    std::memcpy(d, s.c_str(), s.length());
+    discover->val = d;
+}
+
 CoapPDU::Code get_discover_handler(Resource* resource, std::stringstream* &payload, struct yuarel_param* queries, int num_queries) {
     payload = NULL;
     std::stringstream* val = new std::stringstream();
@@ -534,7 +577,7 @@ CoapPDU::Code post_create_handler(Resource* resource, const char* in, char* &pay
     resource->children = new_resource;
     new_resource->children = NULL;
     new_resource->subs = NULL;
-    
+    new_resource->maxage = MAX_AGE_DEFAULT;
     payload = resource_uri;
     // update_discovery(discover);
     return CoapPDU::COAP_CREATED;
@@ -548,6 +591,9 @@ CoapPDU::Code put_publish_handler(Resource* resource, CoapPDU* pdu) {
     CoapPDU::CoapOption* options = pdu->getOptions();
     int num_options = pdu->getNumOptions();
     bool ct_exists = false;
+    bool maxage_exists = false;
+    uint32_t maxage = 0;
+
     while (num_options-- > 0) {
         if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_CONTENT_FORMAT) {
             ct_exists = true;
@@ -563,6 +609,16 @@ CoapPDU::Code put_publish_handler(Resource* resource, CoapPDU* pdu) {
                 return CoapPDU::COAP_NOT_FOUND;
             break;
         }
+
+        if (options[num_options].optionNumber == CoapPDU::COAP_OPTION_MAX_AGE) {
+            maxage_exists = true;
+            uint8_t* option_value = options[num_options].optionValuePointer;
+            for (int i = 0; i < options[num_options].optionValueLength; i++) {
+                maxage <<= 8;
+                maxage += *option_value;
+		option_value++;
+            }
+        }
     }
     
     if (!ct_exists) {
@@ -575,6 +631,7 @@ CoapPDU::Code put_publish_handler(Resource* resource, CoapPDU* pdu) {
     //const char* val = (const char*)pdu->getPayloadCopy();
     delete[] resource->val;
     resource->val = val;
+    resource->maxage = maxage;
     return CoapPDU::COAP_CHANGED;
 }
 
@@ -594,8 +651,8 @@ void remove_all_resources(Resource* resource, bool is_head, Resource* parent, Re
     response->setCode(CoapPDU::COAP_NOT_FOUND);
     while (sub != NULL) {
         response->setToken((uint8_t*)&sub->token, sub->token_len);
-        sendto(
-            sockfd,
+	sendto(
+	    sockfd,
             response->getPDUPointer(),
             response->getPDULength(),
             0,
@@ -649,7 +706,7 @@ void initialize() {
     ps_discover->uri = PS_DISCOVERY; //?rt=core.ps;rt=core.ps.discover;ct=40";
     ps_discover->rt = NULL;
     ps_discover->ct = CoapPDU::COAP_CONTENT_FORMAT_APP_LINK;
-    ps_discover->val = "</ps/>;rt=core.ps;rt=core.ps.discover;ct=40";
+    ps_discover->val = "</ps>;rt=core.ps;rt=core.ps.discover;ct=40";
     ps_discover->next = NULL;
     ps_discover->children = NULL;
     ps_discover->subs = NULL;
@@ -785,7 +842,6 @@ int handle_request(char *uri_buffer, CoapPDU *recvPDU, int sockfd, struct sockad
                 response->setType(CoapPDU::COAP_ACKNOWLEDGEMENT);
                 break;
     };
-
     ssize_t sent = sendto(
         sockfd,
         response->getPDUPointer(),
@@ -868,26 +924,28 @@ int main(int argc, char **argv) {
     
     CoapPDU *recvPDU = new CoapPDU((uint8_t*)buffer, BUF_LEN, BUF_LEN);
     
-    while (1) {
+      struct timeval tv;
+      tv.tv_sec = GC_TIMEOUT;
+      tv.tv_usec = 0;
+
+      while (1) {
 
       FD_ZERO(&read_fds);
       FD_ZERO(&write_fds);
       FD_SET(sockfd, &read_fds);
 
-      struct timeval tv;
-      tv.tv_sec = 10;
-      tv.tv_usec = 0;
-
-      int n = select(sockfd+1, &read_fds, &write_fds, 0, &tv);
-
+      int n = select(sockfd+1, &read_fds, NULL, 0, &tv);
+      
       if(n < 0) {
 	perror("ERROR Server : select()\n");
 	close(sockfd);
 	exit(1);
       }
       if (n == 0)  {
-	      /* TIMEOUT */
-	printf("Timeout\n");
+	tv.tv_sec = GC_TIMEOUT;
+	tv.tv_usec = 0;
+	/* TIMEOUT */
+	__update(discover);
 	continue;
       }
       if(FD_ISSET(sockfd, &read_fds)) {

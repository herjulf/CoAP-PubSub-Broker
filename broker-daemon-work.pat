diff --git a/broker.cpp b/broker.cpp
index 02cd178..05cdc91 100644
--- a/broker.cpp
+++ b/broker.cpp
@@ -2,6 +2,7 @@
 #include "nethelper.h"
 #include <netdb.h>
 #include <sys/types.h>
+#include <sys/stat.h>
 #include <sys/socket.h>
 #include <stdlib.h>
 #include <string.h>
@@ -868,6 +869,37 @@ int main(int argc, char **argv) {
     
     CoapPDU *recvPDU = new CoapPDU((uint8_t*)buffer, BUF_LEN, BUF_LEN);
     
+
+    if(1) {
+      pid_t pid, sid;    
+      pid = fork();
+      
+      if (pid < 0) {
+        std::cerr << "Failed to fork, error code [" << pid << "]. Exitting";
+        return EXIT_FAILURE;
+      } else if(pid > 0) {
+        return EXIT_SUCCESS;
+      }
+      
+      umask(0);
+      /* Set new signature ID for the child */
+
+      sid = setsid();
+
+      if (sid < 0) {
+        std::cerr << "Failed to setsid, error code [" << sid << "]. Exiting";
+        return EXIT_FAILURE;
+      }
+
+      if ((chdir("/")) < 0) {
+        std::cerr << "Failed to change directory to /. Exiting";
+        return EXIT_FAILURE;
+      }
+      close(STDIN_FILENO);
+      close(STDOUT_FILENO);
+      close(STDERR_FILENO);
+    }
+
     while (1) {
 
       FD_ZERO(&read_fds);
@@ -881,13 +913,11 @@ int main(int argc, char **argv) {
       int n = select(sockfd+1, &read_fds, &write_fds, 0, &tv);
 
       if(n < 0) {
-	perror("ERROR Server : select()\n");
 	close(sockfd);
-	exit(1);
+	exit(-1);
       }
       if (n == 0)  {
 	      /* TIMEOUT */
-	printf("Timeout\n");
 	continue;
       }
       if(FD_ISSET(sockfd, &read_fds)) {

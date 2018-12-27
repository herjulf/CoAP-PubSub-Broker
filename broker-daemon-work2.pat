--- broker.cpp.orig	2018-12-22 22:10:08.089172211 +0100
+++ broker.cpp	2018-12-24 11:44:10.259392087 +0100
@@ -2,6 +2,7 @@
 #include "nethelper.h"
 #include <netdb.h>
 #include <sys/types.h>
+#include <sys/stat.h>
 #include <sys/socket.h>
 #include <stdlib.h>
 #include <string.h>
@@ -931,6 +932,37 @@
       tv.tv_sec = GC_TIMEOUT;
       tv.tv_usec = 0;
 
+
+      if(1) {
+	pid_t pid, sid;    
+	pid = fork();
+      
+	if (pid < 0) {
+	  std::cerr << "Failed to fork, error code [" << pid << "]. Exitting";
+	  return EXIT_FAILURE;
+	} else if(pid > 0) {
+	  return EXIT_SUCCESS;
+      }
+	
+	umask(0);
+	/* Set new signature ID for the child */
+	
+	sid = setsid();
+      
+	if (sid < 0) {
+	  std::cerr << "Failed to setsid, error code [" << sid << "]. Exiting";
+	  return EXIT_FAILURE;
+	}
+
+	if ((chdir("/")) < 0) {
+	  std::cerr << "Failed to change directory to /. Exiting";
+	  return EXIT_FAILURE;
+	}
+	close(STDIN_FILENO);
+	close(STDOUT_FILENO);
+	close(STDERR_FILENO);
+      }
+
       while (1) {
 
       FD_ZERO(&read_fds);

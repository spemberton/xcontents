/* xcontents.c
   XContents is a minimal fileserver over http.
   It reads and writes files only in the directory where is is started, and its subdirectories.

   Methods
   The following methods are supported.
   GET: If the specified resource exists, it is returned.
   HEAD: Like GET, except only the headers are returned, with no content.
   PUT: The file is created or overwritten.
   POST: 
      If the resource does not exist, it is created, with the content surrounded by the tags <post>...</post>.
      If it exists, the content is appended before the last closing tag, if there is one, and otherwise at the end.
      Therefore, if you don't like the tags <post>...</post> when the file is created, 
         first PUT an empty file, or one containing only the open and closing tags, e.g. <data></data>,
         before you POST to it.
   OPTIONS: Access control allows access from anywhere.

   Errors
   404: File not found for GET or HEAD
   403: No access to directories (for reading or writing)
   403: If file can't be opened for writing for a PUT or POST
   400: Bad request, if the URI contains "/../" (Most browsers pre-process ".." path steps, so this should never arise.)
   501: Not implemented, for other methods.

   Mostly not robust or secure. 
   For demonstration and test purposes only: do not use for production.
   Based on tiny.c by Dave O'Hallaron, Carnegie Mellon

   compile: cc xcontents.c -o xcontents
   usage:   xcontents <port>
*/

/* Notes to self: cookies 
   Set a session cookie with
      Set-Cookie: name=value;
              or  name="value";
   Cookie expires at the end of the 'session'.
   To specify an expiry date and time add
         Expires=<date>;
   Stupid date format Expires=Thu, 01 Jan 1970 00:00:00 GMT;
   Must be GMT.
   or
         Max-Age: <seconds>;
   86400 is one day.
     
   Delete a cookie with 
     Set-Cookie: name=value; Max-Age=0;
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h> /* ? */
#include <netinet/in.h> /* ? */
#include <arpa/inet.h>

#define BUFSIZE 1024
#define MAXERRS 16
#define SERVER "XContents Web Server"

#define LOG printf

/* For storing the incoming content header heading, should we ever need it */
char content_type[BUFSIZE];
/* For storing the value of the incoming cookie */
char cookie_value[BUFSIZE];
#define COOKIE "XContents"
int add_cookie=0;
int delete_cookie=0;

/* halt - when syscalls fail */
void halt(char *message) {
   perror(message);
   exit(1);
}

void cookie_delete(FILE *stream, char *cookie) {
   fprintf(stream, "Set-Cookie: %s=0; Max-Age: 0;\n", cookie);
   delete_cookie= 0;
}

void cookie_add(FILE *stream, char *cookie, char *value, char *limit) {
   if (limit) {
      fprintf(stream, "Set-Cookie: %s=%s; %s;\n", cookie, value, limit);
   } else {
      fprintf(stream, "Set-Cookie: %s=%s;\n", cookie, value);
   }
   add_cookie= 0;
}

void cookie_read(char *buf, char *name, char *value) {
   char *place;
   char format[BUFSIZE];

   sprintf(format, " %s=", name);
   place=strstr(buf, format);
   if (place) {
      sprintf(format, " %s=%%[^ ;]", name);
      sscanf(place, format, value);
      LOG("... cookie=%s\n", value);
   }
}

/* error - returns an error message to the client */
void error(FILE *stream, char *shortmsg, char *longmsg, char *cause) {
   fprintf(stream, "HTTP/1.1 %s\n", shortmsg);
   fprintf(stream, "Content-type: text/html\n\n");
   fprintf(stream, "<html><head><title>Server Error</title></head><body>");
   fprintf(stream, "<h1>%s</h1>", shortmsg);
   fprintf(stream, "<p>%s: %s</p>", longmsg, cause);
   fprintf(stream, "<hr><p><em>%s</em></p></body></html>\n", SERVER);
   LOG("=> %s %s\n\n", shortmsg, cause);
}

void respond(FILE *stream, char *response, char *h1, char *h2, char *h3, char *h4) {
   fprintf(stream, "HTTP/1.1 %s\n", response);
   fprintf(stream, "Server: %s\n", SERVER);
   if (add_cookie) cookie_add(stream, COOKIE, cookie_value, NULL);
   else if (delete_cookie) cookie_delete(stream, COOKIE);
   if (h1) fprintf(stream, "%s\n", h1);
   if (h2) fprintf(stream, "%s\n", h2);
   if (h3) fprintf(stream, "%s\n", h3);
   if (h4) fprintf(stream, "%s\n", h4);
   fprintf(stream, "\r\n"); 
   fflush(stream);
   LOG("=> %s\n\n", response);
}

void respondOK(FILE *stream, int content_length, char *filetype) {
   fprintf(stream, "HTTP/1.1 200 OK\n");
   fprintf(stream, "Server: %s\n", SERVER);
   if (add_cookie) cookie_add(stream, COOKIE, cookie_value, NULL);
   else if (delete_cookie) cookie_delete(stream, COOKIE);
   fprintf(stream, "Content-length: %d\n", content_length);
   fprintf(stream, "Content-type: %s\n", filetype);
   fprintf(stream, "\r\n"); 
   fflush(stream);
   LOG("=> 200 %s %d\n\n", filetype, content_length);
}

/* These ought really to be in a configuration file */
char *mediatype(char *filename) {
   char *ext= rindex(filename, '.');
   if (ext==NULL) return "text/plain";
   if (strcmp(ext,  ".html")==0) return "text/html";
   if (strcmp(ext,   ".css")==0) return "text/css";
   if (strcmp(ext,   ".xml")==0) return "text/xml";
   if (strcmp(ext,   ".xsl")==0) return "text/xsl";
   if (strcmp(ext, ".xhtml")==0) return "application/xhtml+xml";
   if (strcmp(ext,    ".js")==0) return "application/javascript";
   if (strcmp(ext,   ".gif")==0) return "image/gif";
   if (strcmp(ext,   ".jpg")==0) return "image/jpg";
   if (strcmp(ext,   ".png")==0) return "image/png";
   if (strcmp(ext,   ".svg")==0) return "image/svg+xml";
   return "text/plain";
}

int openport(int portno) {
   int parentfd;
   int optval;        /* flag value for setsockopt */
   static struct sockaddr_in serveraddr; /* server's addr */

   /* open socket descriptor */
   parentfd = socket(AF_INET, SOCK_STREAM, 0);
   if (parentfd < 0) halt("ERROR opening socket");

   /* allows us to restart server immediately */
   optval = 1;
   setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));

   /* bind port to socket */
   bzero((char *) &serveraddr, sizeof(serveraddr));
   serveraddr.sin_family = AF_INET;
   serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
   serveraddr.sin_port = htons((unsigned short)portno);
   if (bind(parentfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) 
      halt("ERROR on binding");

   /* get us ready to accept connection requests */
   /* allow 5 requests to queue up */ 
   if (listen(parentfd, 5) < 0) halt("ERROR on listen");

   return parentfd;
}

int connection(int parentfd) {
   int childfd;                  /* child socket */
   static struct sockaddr_in clientaddr; /* client addr */
   socklen_t clientlen;          /* byte size of client's address */
   static struct hostent *hostp; /* client host info */
   char *hostaddrp;              /* dotted decimal host addr string */

   /* wait for a connection request */
   clientlen = sizeof(clientaddr);
   childfd = accept(parentfd, (struct sockaddr *) &clientaddr, &clientlen);
   if (childfd < 0) halt("ERROR on accept");
   return childfd;

#ifdef NOTDEFINED
   /* determine who sent the message */ /* Not sure why... */
   /* "The more modern interface to convert an IP address to a host
   name is getnameinfo(). It works with IPv4 and IPv6, and should work
   identically on Unix and Windows." */

   hostp = gethostbyaddr((const
   char *)&clientaddr.sin_addr.s_addr,
   sizeof(clientaddr.sin_addr.s_addr), AF_INET);
   if (hostp == NULL)
     halt("ERROR on gethostbyaddr");
     /* gethostbyaddr doesn't set errno, so this is wrong */
   hostaddrp = inet_ntoa(clientaddr.sin_addr);
   if (hostaddrp == NULL) halt("ERROR on inet_ntoa\n");
#endif
}

/* read (and mostly ignore) the HTTP headers; content length is needed for PUT/POST */
int headers(FILE *stream, int is_putpost) {
   char buf[BUFSIZE];
   int content_length;
   char *p;
   p=buf;
   buf[0]='\0'; content_type[0]= '\0'; cookie_value[0]='\0'; content_length=0;
   while (strcmp(buf, "\r\n") != 0 && p != NULL) {
      p= fgets(buf, BUFSIZE, stream);
      LOG(" | %s", buf);
      if (is_putpost && strncmp(buf, "Content-Length: ", 16)==0) {
	 sscanf(buf, "Content-Length: %d\n", &content_length);
      } else if (strncmp(buf, "Cookie: ", 8)==0) {
	cookie_read(buf, COOKIE, cookie_value);
      } else if (strncmp(buf, "Content-Type: ", 14)) {
	 sscanf(buf, "Content-Type: %s\n", content_type);
      }
   }
   return content_length;
}

/* Extract the fields from the initial HTTP line, like: GET filename HTTP/1.1 */
void parse_input(char *buf, char *method, char *filename, char *params) {
   char version[BUFSIZE]; /* HTTP version (ignored) */
   char uri[BUFSIZE];     /* request uri */
   char *p;

   sscanf(buf, "%s %s %s\n", method, uri, version);	 
   /* Ignore all parameters; preserve them if we ever decide to use them in a future version */
   p = index(uri, '?');
   if (p) {
      strcpy(params, p+1);
      *p = '\0';
   } else {
      strcpy(params, "");
   }
   
   /* construct the filename; the URI should start with /, but we protect against bad actors */
   strcpy(filename, ".");
   if (uri[0] != '/') strcat(filename, "/");
   strcat(filename, uri);
   if (uri[strlen(uri)-1] == '/') 
      strcat(filename, "index.html");
}

void do_options(FILE *stream) {
   respond(stream, "204 No Content\n",
	   "Access-Control-Allow-Origin: *",
	   "Access-Control-Allow-Methods: PUT, GET, POST, HEAD, OPTIONS", 
	   "Access-Control-Allow-Headers: *",
	   "Allow: GET, PUT, POST, HEAD, OPTIONS");
}

void do_get_head(FILE *stream, char *filename, int is_get) {
   struct stat sbuf;      /* file status */
   char *p;               /* temporary pointer */
   int fd;                /* static content filedes */   
   
   if (stat(filename, &sbuf) < 0) { /* make sure the file exists */
      error(stream, "404 Not found", "Server couldn't find this file", filename);
   } else if (!S_ISREG(sbuf.st_mode)) {
      error(stream, "403 Forbidden", "No access to directory", filename);
   } else { /* Handle regular file */
      
      respondOK(stream, (int)sbuf.st_size, mediatype(filename));
      
      if (is_get) {
	 /* Memory map the file and copy it out */
	 fd = open(filename, O_RDONLY);
	 p = mmap(0, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	 fwrite(p, 1, sbuf.st_size, stream);
	 munmap(p, sbuf.st_size);
	 close(fd);
      }
   }
}

void do_put (FILE* stream, char *filename, int content_length) {
   struct stat sbuf;      /* file status */
   FILE *putfile;         /* Output for PUT method */
   int ch;
   int created=0;

   if (stat(filename, &sbuf) < 0) { created=1; }
   putfile = fopen(filename, "w");
   if (putfile) {
      if (created)
	 LOG("=> create %d %s\n\n", content_length, filename);
      else
	 LOG("=> overwrite %d %s\n\n", content_length, filename);
      ch=' ';
      while (content_length-- && ch!=EOF) {
	 ch= fgetc(stream);
	 fputc(ch, putfile);
	 putchar(ch);
      }
      fclose(putfile);
      if (content_length != -1) LOG("Content-length mismatch?\n");
      LOG("\n\n");
      if (created) {
	 respond(stream, "201 Created", "Content-length: 0", NULL, NULL, NULL);
      } else {
	 respond(stream, "204 No Content", NULL, NULL, NULL, NULL);
      }
   } else { 
      error(stream, "403 Forbidden", "Couldn't write file", filename);
   }
}

/* POST: if the file doesn't exist, creates "<post>...data...</post>"
   If the file exists, appends the new data before the closing tag of the file.
   If no ending tag is found, the content is just appended.
 */

void do_post (FILE* stream, char *filename, int content_length) {
   struct stat sbuf;       /* file status */
   FILE *putfile;          /* Output for PUT method */
   char closetag[BUFSIZE]; /* Closing tag */
   int ch;
   int err=0;
   int created=0;
   long seek;

   if (stat(filename, &sbuf) < 0) {
      created=1;
      putfile = fopen(filename, "w");
   } else {
      putfile = fopen(filename, "r+");
   }
   if (putfile) {
      if (created) {
	 LOG("=> create %d %s\n\n", content_length, filename);
	 fputs("<post>\n", putfile);
	 strcpy(closetag, "post");
      } else {
	 /* search backwards for the closing tag */
	 ch=' '; seek=-3L; /* The smallest tag is </a> */
	 while (ch!='<' && err!=-1) {
	    seek-=1L;
	    err=fseek(putfile, seek, SEEK_END);
	    if (err != -1) ch= fgetc(putfile);
	 }
	 if (err==-1) {
	    /* If no closing tag found, just append the content */
	    LOG("... no closing tag at %ld\n", seek);
	    fseek(putfile, 0L, SEEK_END);
	 } else {
	    fscanf(putfile, "/%[^>]>", closetag);
	    LOG("... tag=%s at %ld\n", closetag, seek);
	    /* position just before the closing tag */
	    fseek(putfile, seek, SEEK_END);
	 }
	 LOG("=> append %d %s\n\n", content_length, filename);
      }
      ch=' ';
      while (content_length-- && ch!=EOF) {
	 ch= fgetc(stream);
	 fputc(ch, putfile);
	 putchar(ch);
      }
      if (err!=-1) fprintf(putfile, "\n</%s>\n", closetag);
      fclose(putfile);
      if (content_length != -1) LOG("Content-length mismatch?\n");
      LOG("\n\n");
      if (created) {
	 respond(stream, "201 Created", "Content-length: 0", NULL, NULL, NULL);
      } else {
	 respond(stream, "204 No Content", NULL, NULL, NULL, NULL);
      }
   } else { 
      error(stream, "403 Forbidden", "Couldn't write file", filename);
   }
}

int main(int argc, char **argv) {
   /* connection management */
   int parentfd;          /* parent socket */
   int childfd;           /* child socket */
   int portno;            /* port to listen on */
   
   /* connection I/O */
   FILE *stream;          /* stream version of childfd */
   char buf[BUFSIZE];     /* message buffer */
   char method[BUFSIZE];  /* request method */
   char params[BUFSIZE];  /* everything after # or ? in uri */
   char filename[BUFSIZE];/* path derived from uri */
   char *p;               /* temporary pointer */
   int is_get;            /* GET */
   int is_head;           /* HEAD */
   int is_put;            /* PUT */
   int is_post;           /* POST */
   int is_options;        /* OPTIONS */
   int content_length;    /* Content length for PUT */

   /* check command line args */
   if (argc != 2) {
      fprintf(stderr, "usage: %s <port>\n", argv[0]);
      exit(1);
   }
   portno = atoi(argv[1]);

   parentfd= openport(portno);

   /* main loop: wait for a connection request, parse HTTP,
    * serve requested content, close connection.
    */
   while (1) {
     
      /* open the child socket descriptor as a stream */
      childfd= connection(parentfd);   
      if ((stream = fdopen(childfd, "r+")) == NULL) halt("ERROR on fdopen");

      /* get the HTTP request line */
      LOG("WAITING\n"); fflush(stdout);
      p= fgets(buf, BUFSIZE, stream);
      if (p == NULL) { /* Don't know why it would be, but it is sometimes */
	perror("fgets returned NULL");
	/* fgets returned NULL: Bad file descriptor */
      } else {
	 LOG("%s", buf);
	 parse_input(buf, method, filename, params);

	 /* Most browsers seem to do ".." processing first, but just in case... */
	 if (strstr(filename, "/../")) {
	    error(stream, "400 Bad Request", "URI contains '/../'", filename);
	 } else {
	    /* only support GET, PUT, HEAD, POST, and OPTIONS */
	    is_get= is_head= is_put= is_post= is_options=0;
	    if (strcasecmp(method, "GET") == 0) { is_get=1; }
	    else if (strcasecmp(method, "HEAD") == 0) { is_head=1; }
	    else if (strcasecmp(method, "PUT") == 0) { is_put=1; }
	    else if (strcasecmp(method, "POST") == 0) { is_post=1; }
	    else if (strcasecmp(method, "OPTIONS") == 0) { is_options=1; }
	    
	    content_length=headers(stream, is_put||is_post);
	    
	    if (is_get || is_head) {
	       do_get_head(stream, filename, is_get);
	    } else if (is_put) {
	       do_put(stream, filename, content_length);
	    } else if (is_post) {
	       do_post(stream, filename, content_length);
	    } else if (is_options) {
	       do_options(stream);
	    } else {
	       error(stream, "501 Not Implemented", "Server does not implement this method", method);
	    }
	 }
      }
      /* clean up */
      fclose(stream);
      close(childfd);
   } /* while(1) */
} /* main */

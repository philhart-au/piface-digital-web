/***********************************

File: server.cpp

Basic usage:    $ sudo ./server 80
Verbose usage:  $ sudo ./server 80 v
Version usage:  $ ./server 80 a

This is a very simple web server that supplies a web page that
can be used to drive a PiFace Digital 2.

The port number (80) is mandatory, and provides HTTP connection
only. HTTPS is not supported.

In lieu of <?PHP ... ?> tag with corresponding *.php files, this
server uses "*.qif" pseudo files, which "files" are redirected
to routines within the application. (qif = Query InterFace.) See
file "piface_digital_2.js" for an example of *.qif usage.

General operation:
The state of the PiFace Digital 2 digital inputs is sent to all
known web connections using Server-Side Events (SSE).

When any web connection alters the state of an output, the new
output state is stored within the server and added as an
additional 8 bits to the digital input SSE mesages. The new
output state is sent once to each known web connection.

Note about socket names

listen_socket_fd:
Listens for connection requests from web browsers. This runs
in the main thread.

service_socket_fd:
Responds to HTPP requests. Runs in a separate thread.

event_socket_fd:
This is identical to service_socket_fd. It is separate from
service_socket_fd for the purpose of clarity.

***********************************/

#include <iostream>
#include <exception>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sched.h>
#include <time.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include "pifacedigital.h"

#include "utils.c"

using namespace std;

//  Constant declarations
#define MAX_DISK_PAGE_SIZE       40000
#define MAX_EXPANDED_PAGE_SIZE   80000

#define REQUEST_UNDEFINED 0
#define REQUEST_GET       1
#define REQUEST_PUT       2

//  Forward declarations
void  *accept_connection (void *ptr);
void   cleanup_server_connections(int);
void   error(const char *);
int    expand_page(char *, char *, int);
int    get_page( char *, char *, int, char *, int );
int    get_page_name( char *, char *, int, char *, char * );
int    get_request_type ( char * );
void   initialise();
int    locate_char (char, char *);
int    main(int, char *[]);
void   open_event_stream ( int );
void   process_get_request ( char *, int );
void   process_put_request ( char *, int );
void   register_event_stream ( int );
int    send_error( char * );
void   send_events(int);
void   serve_page(int, char *, int, bool);
void   server( int );
void   sigpipe_handler ( int );
void   thread_error(const char *);
void   unregister_event_stream ( int );
void   write_header ( int, char *, int);

//  Version control
char   version[] = "v0.0.1";

//  Two handy veriables
int   verbose;
int   try_catch_count;

//  Interface between the main application and the threads
//  used to service web browser requests
pthread_t thread;
struct Thread_Args {
	int fd;
	int counter_unused;
	char * extra_unused;
};

//  When an output is changed in one web browser, all the other
//  currently connected web browsers need to be informed.
//  The variables below support this need.
#define MAX_EVENT_STREAM 10
static int  event_stream[MAX_EVENT_STREAM];
static bool event_waiting[MAX_EVENT_STREAM];
static char output[8];

//  PiFace digital 2 variables
int   pif_input;
int   pif_hw_addr;
int   pif_interrupts_enabled;
int   pif_output;

/*
This procedure accepts incoming connection requests from
web browsers. It runs in a separate thread.
*/
void *accept_connection ( void *ptr ) {
	struct Thread_Args * args;
	args = ( struct Thread_Args * ) ptr;
	char  from_browser[5000];
	int   i;
	int   j;
	int   n;
	int   enable = 1;
	int   service_socket_fd;
	service_socket_fd = (*args).fd;
	int   request_type;

	try {

		//  Configure the socket
		i = setsockopt ( service_socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int) );
		if ( i < 0 ) {
			thread_error ("ERROR setsockopt SO_REUSERADDR");
		}
		if ( verbose ) {
			printf ("accept_connection: attempting to read request from browser.\n");
		}

		//  Get the request from the web browser
		n = read (service_socket_fd, from_browser, sizeof(from_browser)-2);
		if (n < 0) {
			thread_error("ERROR reading from socket.\n");
		}
		if ( verbose ) {
			printf ("Read %d from browser at -a\n", n);
		}
		from_browser[n] = 0;
		i = test_in_string (from_browser, "Content-Length:");

		//  Wait for more if necessary
		if (i>0) {
			if ( verbose ) {
				printf ("Expected more from the browser at -b\n");
			}
			//  Here for more from the browser
			sleep (1);
			j = recv (service_socket_fd, &from_browser[n], sizeof(from_browser) - 2 - n, MSG_DONTWAIT);
			if (j<0) j=0;
			from_browser[n+j] = 0;
		}
		if ( verbose ) {
			printf ("%d, %d\n%s\n", n, j, from_browser);
		}
		if (n>10) {

			//  Process the request
			request_type = get_request_type ( from_browser );
			if ( request_type == REQUEST_GET ) {
				process_get_request( from_browser, service_socket_fd );
			} else
			if ( request_type == REQUEST_PUT ) {
				int fd = service_socket_fd;
				process_put_request ( from_browser, fd );
			};
		}
		if ( verbose ) {
			printf ("accept_connection: about to close socket.\n");
		}

		//  Close the socket
		close( service_socket_fd );
		if ( verbose ) {
			printf ("accept_connection: socket closed.\n");
		}

	//  Deal with exceptions
	} catch ( exception &e ) {
		printf ("accept_connection exception: %s\n", e.what());
		cleanup_server_connections( service_socket_fd );
	} catch ( ... ) {
		printf ("accept_connection unknown exception.\n");
		unregister_event_stream ( service_socket_fd );
		throw;
	}
	return 0;
}

/*
This procedure attempts to close the indicated file descriptor

It also removes the indicated fiel descriptor from the list of
server-side event receivers.
*/
void cleanup_server_connections( int fd ) {
	if ( fd > 0 ) {
		close ( fd );
		unregister_event_stream ( fd );
	}
	try_catch_count++;
}


/*
Prints the indicated error message
*/
void error(const char *msg)
{
    perror(msg);
}

/*
Copy the page on disk into the output buffer
*/
int expand_page (char * disk, char * expanded, int max_length) {
	char * ptr_in;
	char * ptr_in_;
	char * ptr_out;
	ptr_in = disk;
	ptr_out = expanded;
	for (;;) {
		*ptr_out = *ptr_in;
		if (*ptr_out==0) {
			break;
		}
		ptr_in++;
		ptr_out++;
	}
	return ptr_out - expanded;
}

/*
Copies the indicated file from disk to the indicated buffer
*/
int get_page (char * browser_request, char * page_name, int name_max_length, char * disk_page, int disk_max_length) {

	char * page_parameters;
	int page_file;
	int page_file_length;
#pragma GCC diagnostic ignored "-Wuninitialized"
//  page_parameters gets warned as being uninitialized
	get_page_name ( browser_request, page_name, name_max_length, page_parameters, disk_page );
#pragma GCC diagnostic warning "-Wuninitialized"
	//  Try to open the required page
	if ( verbose ) {
		printf("get_page: Requested +%s+\n", page_name);
	}
	page_file = open (page_name, O_RDONLY);
	if (page_file<1) {
		return send_error( disk_page );
	}
	page_file_length = read (page_file, disk_page, disk_max_length - 2);
	*(disk_page+page_file_length) = 0;
	close(page_file);
	if ( verbose ) {
		printf("get_page: Page length %d\n", page_file_length);
	}
	return page_file_length;
}

/*
Extracts the name of the page requested by the web browser.
Supplies "index.html" if no page name is requested.

Returns 0 on success, -1 oterhwise.
*/
int get_page_name ( char * browser_request, char * page_name, int max_length, char * page_parameters, char * disk_page ) {
	char * ptr;
	char * page_name_end;
	int file_length = 0;

	//  Decode the required page
	ptr = browser_request;
	while (*ptr!='/') {
		if (*ptr==0) {
			return send_error( disk_page );
		}
		ptr++;
	}
	ptr++;
	page_name_end = ptr;
	page_parameters = 0;
	while (*page_name_end!=' ' && *page_name_end!='?') {
		if (*page_name_end==0) {
			return send_error( disk_page );
		}
		page_name_end++;
	}
	if (*page_name_end=='?') {
		page_parameters = page_name_end + 1;
	}
	*page_name_end = 0;
	strcpy (page_name, ptr);
	if (page_name_end==ptr) {
		strcpy (page_name, "index.html");
	}
	return 0;
}

/*
Returns the type of request made by the web browser.

Expected HTTP types are GET and PUT.
*/

int get_request_type ( char * p) {
	if ( test_lead_string ( p, "GET " ) ) {
		return REQUEST_GET;
	}
	if ( test_lead_string ( p, "PUT " ) ) {
		return REQUEST_PUT;
	}
	return REQUEST_UNDEFINED;
}

/*
Initialise everything that needs it
*/
void initialise() {
//	int mmap_fd;
//	int length;
	struct sigaction act;

	try_catch_count = 0;

	//  Configure the PiFace digital 2 things
	pifacedigital_open( pif_hw_addr );
	if ( verbose ) {
		printf("Opened PiFace Digfital 2 with hardware addess %d\n", pif_hw_addr );
	}
	pif_interrupts_enabled = !pifacedigital_enable_interrupts();
	if ( pif_interrupts_enabled ) {
		if ( verbose ) {
			printf("PiFace Digfital 2 interrups enabled.\n" );
		}
	} else {
		printf("PiFace Digfital 2 interrups NOT enabled.\n" );
	}
	pif_output = 0;

	//  Set up the event streams
	int i;
	for ( i = 0; i < MAX_EVENT_STREAM; i++ ) {
		event_stream[i] = -1;
		event_waiting[i] = false;
	}
	//  Zero the outputs
	for ( i = 0; i < 8 ; i++ ) {
		output[i] = 0;
	}

	//  Be graceful about web browser closing down
	memset ( &act, 0, sizeof(act));
	act.sa_handler = SIG_IGN;
	act.sa_flags = SA_RESTART;
	sigaction (SIGPIPE, &act, NULL);
}

/*
This procedure advises the connected web browser to expect
server-side events. It is response to the request for the
pseudo file "events.qif".
*/
void open_event_stream ( int fd ) {
	char header[300];
	int header_length;
	header_length = sprintf (header, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=UTF-8\n\n" );
	if ( verbose ) {
		printf ("Event header created.\n");
	}
	write_header ( fd, header, header_length );
	register_event_stream ( fd );
}

/*
This procedure returns the web page requested by the web browser.

Initiates server-side events if the requested page is "events.qif".
*/
void process_get_request ( char * from_browser, int service_socket_fd ) {
	char  page_name[300];
	char  disk_page[MAX_DISK_PAGE_SIZE];
	int   disk_page_length;
	int   expanded_page_length;
	char  expanded_page[MAX_EXPANDED_PAGE_SIZE];
	int   event_socket_fd;

	//  Get the request file from disk
	disk_page_length = get_page ( from_browser, page_name, sizeof ( page_name ), disk_page, sizeof ( disk_page ) );

	//  Deal with *.png file
	if (test_tail_string (page_name, ".png")) {
		if ( verbose ) {
			printf ("Serving image\n");
		}
		serve_page (service_socket_fd, disk_page, disk_page_length, false);

	//  Deal with *.qif files
	} else if (test_tail_string (page_name, ".qif")) {
		if ( verbose ) {
			printf ("Serving qif\n");
		}
		if ( test_lead_string ( page_name, "events." ) ) {
			if ( verbose ) {
				printf ("Serving events\n");
			}
			event_socket_fd = service_socket_fd;
			open_event_stream ( event_socket_fd );
			send_events ( event_socket_fd );
		}
		if ( verbose ) {
			printf ("PiFace events sent\n");
		}
	//  Deal with all other file types
	} else {
		if ( verbose ) {
			printf ("Serving text\n");
		}
		expanded_page_length = expand_page (disk_page, expanded_page, sizeof ( expanded_page ) - 2);
		if ( verbose ) {
			printf ("Text now expanded\n");
		}
		serve_page (service_socket_fd, expanded_page, expanded_page_length, false);
	}
}

/*
This procedure services PUT requests from the web browser.

It is used to provide the functionality needed when the user
changes an output.
*/
void process_put_request ( char * from_browser, int fd ) {
	int i;
	int mask;
	if ( verbose ) {
		printf ("Started process_put_request\n");
	}
	char header[300];
	int header_length;
	char *ptr = from_browser;
	while ( *ptr != '?' ) {
		ptr++;
	}
	ptr++;

	//  Extract which bit is being modified
	ptr++;
	int bit = (*ptr) - '0';

	//  Extract the required value
	ptr += 2;
	int value = (*ptr) - '0';
	if ( value ) {
		value <<= bit;
		pif_output |= value;
	} else {
		mask = 1;
		mask <<= bit;
		pif_output &= ~mask;
	}

	//  Write to the PiFace Digital 2
	pifacedigital_write_reg ( pif_output, OUTPUT, pif_hw_addr );

	try {
		//  Send off the acknowledgement to the web browser
		header_length = sprintf (header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %d\n\n", 0);
		write_header ( fd, header, header_length );

		//  Update the master copy of the output bits
		mask = 1;
		for ( i = 0; i < 8; i++ ) {
			if ( mask & pif_output ) {
				output[i] = '1';
			} else {
				output[i] = '0';
			}
			mask <<= 1;
		}

		//  Advise all currently connected browsers and future
		//  connected web browser that the output has changed.
		for ( i = 0; i < MAX_EVENT_STREAM; i++ ) {
			event_waiting[i] = true;
		}

	//  Catch and deal with the expections
	} catch ( exception &e ) {
		printf ("Process put exception: %s\n", e.what());
		cleanup_server_connections( fd );
		throw;
		pthread_exit ( NULL );
	} catch ( ... ) {
		printf ("Process put unknown exception.\n");
		unregister_event_stream( fd ) ;
		throw;
		pthread_exit ( NULL );
	}
	if ( verbose ) {
		printf ("Exit process_page.\n");
	}
}

/*
Attempt to register the given file descriptor as an event stream.
Silently ignore if there is no spare slot
*/
void register_event_stream ( int fd ) {
	int i;
	for ( i = 0; i < MAX_EVENT_STREAM; i++ ) {
		if ( event_stream[i] < 0 ) {
			event_stream[i] = fd;
			if ( verbose ) {
				printf ( "Registered event stream %d\n", fd);
			}
			break;
		}
	}
}

/*
Send the current state of the digital inputs to the connected
web browser.

If the digital outputs have changed, append that to the event
message.
*/
void send_events( int event_socket_fd ) {
	int i;
	int j;
	int event_length;
	char event[200];
	if ( verbose ) {
		printf ("Send events entered\n");
	}
	for ( ;; ) {

		//  Prepare the first mart of the event message
		event_length = sprintf (event, "event: piface\ndata: ");

		//  Get the current state of the digital inputs
		pif_input = pifacedigital_read_reg( INPUT, pif_hw_addr);

		//  Write the current state as a binary number
		write_binary ( pif_input, &event[event_length], 8 );
		event_length += 8;

		//  See if there is an output waiting to be sent
		for ( i = 0; i < MAX_EVENT_STREAM; i++ ) {
			if ( event_stream[i] == event_socket_fd ) {
				if ( event_waiting[i] ) {
					event_waiting[i] = false;
					for ( j = 0; j < 8; j++ ) {
						event[event_length] = output[j];
						event_length++;
					}
				}
			}
		}

		//  Add the terminator to the event message
		event[event_length] = '\n';
		event_length++;
		event[event_length] = '\n';
		event_length++;
		event[event_length] = 0;

		//  Send the event to the connected web browser
		serve_page ( event_socket_fd, event, event_length, true);
		if ( verbose ) {
			printf (event);
		}
		if ( verbose ) {
			printf ("Send event sleep started\n");
		}
		sleep(1);
		if ( verbose ) {
			printf ("Send event sleep ended\n");
		}
	}

	//  This should never be reached
	if ( verbose ) {
		printf ("Send event exited\n");
	}
}

/*
Send a 404 file not found error message to the connected web browser
*/
int send_error ( char * disk_page ) {
	int i;
	strcpy (disk_page, "<html><head></head><body>404: File not found</body></html>");
	for ( i = 0; i < MAX_DISK_PAGE_SIZE; i++ ) {
		if (disk_page[i]==0) {
			break;
		}
	}
	return -1;
}

/*
Send the requested page to the connected wbe browser
*/
void serve_page (int fd, char * page, int page_length, bool event) {
	char header[300];
	int n;
	int header_length;
	try {
		//  If it is not an event, write the required HTTP heaxder
		if ( !event ) {
			header_length = sprintf (header, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %d\n\n", page_length);
			write_header ( fd, header, header_length );
		}

		//  Send the requestd page to the connecetd web browser
		if ( verbose ) {
			printf ("About to write %d bytes of content.\n", page_length);
		}
		n = write( fd, page, page_length);
		if ( verbose ) {
			printf ("Wrote %d bytes of content.\n",n);
		}

		//  Deal with sock issues
		if (n < 0) {
			thread_error("ERROR writing to socket");
		}

	//  Deal with otehr socket issues
	} catch ( exception &e ) {
		printf ("Serve page exception: %s\n", e.what());
		cleanup_server_connections( fd );
		throw;
		pthread_exit ( NULL );
	} catch ( ... ) {
		printf ("Serve page unknown exception.\n");
		throw;
		pthread_exit ( NULL );
	}
	if ( verbose ) {
		printf ("Exit serve_page.\n");
	}
}

/*
The procedure runs on the main thread. It listens to connection
requests from web browsers.

When a connection request is received, it initiates a separate
thread and passes the connection request to it.

*/
void server( int listen_socket_fd ) {
	struct Thread_Args *thread_args;
	int service_socket_fd;
	int n;
	struct sockaddr_in cli_addr;
	socklen_t clilen;
	clilen = sizeof(cli_addr);
	char output_put[9];
	printf ("Enter server.\n");
	thread_args = ( Thread_Args * ) malloc(sizeof(Thread_Args));
	thread_args->fd = -1;
/*
The following lines are to provide extensibility to provide additional
parameters to the newly created thread.
	thread_args->counter_unused = -1;
	// As in "Mary had a little lamb."
	output_put[0] = 'M';
	output_put[1] = 'a';
	output_put[2] = 'r';
	output_put[3] = 'y';
	output_put[4] = 0;
	thread_args->extra_unused = output_put;
*/
	for (;;) {
		try {
			if ( verbose ) {
				printf ("server: try catch count: %d\n", try_catch_count);
			}
			if ( verbose ) {
				printf ("server: waiting for connection.\n");
			}

			//  Accept the new connection request from a web browser
			service_socket_fd = accept(listen_socket_fd,
				(struct sockaddr *) &cli_addr,
				&clilen);
			if ( verbose ) {
				printf ("server: connection request received.\n");
			}
			if (service_socket_fd < 0) {
				error("ERROR on accept");
			}

			//  Create a new thread to deal with request from the web browser
			thread_args->fd = service_socket_fd;
			n = pthread_create ( &thread, NULL, accept_connection, (void*) thread_args );
			if ( verbose ) {
				printf ("server: thread created.\n");
			}

		//  Deal with exceptions
		} catch ( exception &e ) {
			printf ("Server exception: %s\n", e.what());
			cleanup_server_connections( service_socket_fd );
		} catch ( ... ) {
			printf ("Server unknown exception.\n");
		}
	}

//  Should never reach here
	printf ("Exit server.\n");
}

/*
Handler for TCP connection issues. It does nothing.
*/
void sigpipe_handler ( int i ) {
}

/*
Attempts to remove the given file descriptor as an event stream.
Silently ignore if there it is not found
*/
void unregister_event_stream ( int fd ) {
	int i;
	for ( i = 0; i < MAX_EVENT_STREAM; i++ ) {
		if ( event_stream[i] == fd ) {
			event_stream[i] = -1;
		}
	}

}

/*
Prints an error message and terminates the calling thread.
*/
void thread_error(const char *msg)
{
    perror(msg);
    pthread_exit(NULL);
}

/*
Writes the indicated HTTP header to the connected web browser.
*/
void write_header ( int fd, char * header, int length ) {
	int n;
	try {
		if ( verbose ) {
			printf ("About to write header.\n");
		}
		n = write( fd, header, length);
		if ( verbose ) {
			printf ("Wrote %d bytes of header.\n", n);
		}

		//  Deal with an exception
		if (n < 0) {
			thread_error("ERROR writing to socket");
			unregister_event_stream ( fd );
		}
	//  Deal with all other exceptions
	} catch ( exception &e ) {
		printf ("Write header exception: %s\n", e.what());
		unregister_event_stream ( fd );
	}
}

/*
Entry point
*/
int main(int argc, char *argv[])
{
    int   listen_socket_fd;
    int   portno;
    int   page_served = 0;
    char  from_browser[5000];
    char  page_name[300];
    char *page_parameters;

    struct sockaddr_in serv_addr;
    struct ifreq ifr;

    int enable = 1;
    int result;
    verbose = 0;

    //   Get the port number
    if (argc < 2) {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }

    //  See if we need to be verbose or are being asked about version
    if ( argc >= 3 ) {
        if ( *argv[2] == 'v' ) {
            verbose = 1;
        }
        if ( *argv[2] == 'a' ) {
            printf ( "Version: %s\n", version);
            exit (0);
        }
    }

    //  Initialise everything that needs to be initalised
    initialise();

    //  Create and configure a socket to listen to connection requests
    listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_fd < 0)
        error("ERROR opening socket");
    result = setsockopt ( listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int) );
    if ( result < 0 )
	error ("ERROR setsockopt SO_REUSERADDR");
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy ( ifr.ifr_name, "wlan0", IFNAMSIZ-1);
    ioctl ( listen_socket_fd, SIOCGIFADDR, &ifr);
    fprintf(stdout, "My IP address: %s\n", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));

    bzero((char *) &serv_addr, sizeof(serv_addr));
    portno = atoi(argv[1]);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);
    if (bind(listen_socket_fd, (struct sockaddr *) &serv_addr,
              sizeof(serv_addr)) < 0)
              error("ERROR on binding");
    listen( listen_socket_fd, 5 );

    //  Start the web server
    server( listen_socket_fd );

    //  Should never get here
    close( listen_socket_fd );
    return 0;
}

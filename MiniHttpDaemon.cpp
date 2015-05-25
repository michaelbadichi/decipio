//
//  MiniHttpDaemon.cpp
//
//  Created by Michael Badichi 
//

#include <stdio.h>
#ifndef _WINDOWS
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <sys/wait.h>
#define DoLog printf
#else
#include <Winsock2.h>
#include "log.h"
#pragma warning(disable: 4996)
#endif
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <assert.h>
#include <map>
#include <boost/thread/mutex.hpp>
#include <boost/tokenizer.hpp>
#include <boost/thread/thread.hpp>
#include "MiniHttpDaemon.h"

#define DEBUG_LOG       1
#define ENABLE_CGI      0
#define APPEND_INDEX    1

#ifndef SD_BOTH
#define SD_BOTH 2
#endif //SD_BOTH

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: MiniHttpServer by Michael Badichi/1.1\r\n"


typedef struct {
    int size;
} IncompleteInfo_t;
typedef std::map< std::string, IncompleteInfo_t > incomplete2size_t;

class AcceptRequest_t;
typedef std::map< AcceptRequest_t *, int > RunningClients_t;

typedef struct
{
    volatile bool thread_started;   //todo: use condition
    volatile bool thread_shouldrun;
    boost::thread server_thread;
    int server_sock;
    std::string root_folder;
    int port;
    int bytes_sent;
    int bytes_received;
    bool local_only;
    bool serve_diskfiles;
    boost::mutex activeClientsLock;
    RunningClients_t activeClients;
    incomplete2size_t incomplete2size;
    MiniHttpDaemon::FileHandlerCallback_t fileHandlerCallback;
    MiniHttpDaemon::SocketHandlerCallback_t socketHandlerCallback;
    MiniHttpDaemon::FileUploadCallback_t fileUploadCallback;
    void * fileHandlerUserdata;
    void * socketHandlerUserdata;
    void * fileUploadUserdata;
} MiniHtmlInfo_t;

typedef std::map< std::string, std::string > HeaderPeoperties_t;

class AcceptRequest_t
{
    public:
    AcceptRequest_t( int sock ) 
    {
        client = sock;
    }
    ~AcceptRequest_t() 
    {
#ifdef _WINDOWS
        closesocket( client );
#else
        close( client );
#endif 
    }
    void Kill() 
    { 
        //set socket to nonblocking
#ifdef _WINDOWS
        u_long iMode = 1;
	    ioctlsocket( client, FIONBIO, &iMode );
#else
        int opts;
        opts = fcntl ( client, F_GETFL );
        if( opts >= 0 )
        {
            fcntl ( client, F_SETFL, opts | O_NONBLOCK );
        }
#endif
        //and shut it down
        shutdown( client, SD_BOTH );
    }
    int client;
    HeaderPeoperties_t properties;
    std::string method;
    std::string query_string;
    std::string post_data;
    std::string url;
    MiniHtmlInfo_t * serverinfo;
    boost::thread thread;
};

void error_die(const char *);
void execute_cgi(AcceptRequest_t *, const char *);
int get_line( AcceptRequest_t *, char *, int );

static size_t logged_send( AcceptRequest_t * ar, const void * buf, int len, int mode )
{
    int ret = send( ar->client, reinterpret_cast<const char*>(buf), len, mode );
    if( ret > 0 )
    {
        ar->serverinfo->bytes_sent += ret;
    }
    return ret;
}


/**********************************************************************/
/* Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor. */
/**********************************************************************/
static void cannot_execute( AcceptRequest_t * ar )
{
    const char * buf =  "HTTP/1.1 500 Internal Server Error\r\n"
                        SERVER_STRING
                        "Content-type: text/html\r\n"
                        "\r\n"
                        "<P>Error prohibited CGI execution.\r\n"
                        ;
    logged_send( ar, buf, (int)strlen(buf), 0 );
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
static void not_found( AcceptRequest_t * ar )
{
    const char * buf =  "HTTP/1.1 404 NOT FOUND\r\n"
                        SERVER_STRING
                        "Content-Type: text/html\r\n"
                        "\r\n"
                        "<HTML><TITLE>Not Found</TITLE>\r\n"
                        "<BODY><P>The server could not fulfill\r\n"
                        "your request because the resource specified\r\n"
                        "is unavailable or nonexistent.\r\n"
                        "</BODY></HTML>\r\n"
                        ;
    logged_send( ar, buf, (int)strlen(buf), 0 );
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
/**********************************************************************/
static void unimplemented( AcceptRequest_t * ar )
{
    static const char * buf =   "HTTP/1.1 501 Method Not Implemented\r\n"
                                SERVER_STRING
                                "Content-Type: text/html\r\n"
                                "\r\n"
                                "<HTML><HEAD><TITLE>Method Not Implemented\r\n"
                                "</TITLE></HEAD>\r\n"
                                "<BODY><P>HTTP request method not supported.\r\n"
                                "</BODY></HTML>\r\n"
                                ;
    logged_send( ar, buf, (int)strlen(buf), 0 );
}

static void bin_headers( AcceptRequest_t * ar, const char *filename, int size, const char * _content_type, bool cacheable, time_t modifytime )
{
    char buf[2*1024];
    (void)filename;  /* could use filename to determine file type */
    
    std::string content_type;
    if( _content_type )
    {
        content_type = "Content-Type: ";
        content_type += _content_type;
        content_type += "\r\n";
    }
    
    char cache[1024];
    if( cacheable )
    {
        struct tm * ptm = gmtime( &modifytime );
        const char * days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        const char * months[] = { "Jan", "Feb", "Mer", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        sprintf( cache,
                "Cache-Control: max-age=3600\r\n"
                "Expires: Tue, 01 Jan 2030 00:00:00 GMT\r\n"
                "Last-Modified: %s, %d %s %d %02d:%02d:%02d GMT\r\n"
                , days[ ptm->tm_wday ]
                , ptm->tm_mday
                , months[ ptm->tm_mon ]
                , ptm->tm_year + 1900
                , ptm->tm_hour
                , ptm->tm_min
                , ptm->tm_sec
                );

    }
    else 
    {
        cache[0]=0;
    }
        
    if( DEBUG_LOG ) DoLog("Sending %s headers, filesize = %d\n",_content_type, size);
    sprintf(buf, 
            "HTTP/1.1 200 OK\r\n"
            SERVER_STRING
            "%s"
            "%s"
            "Accept-Ranges: 0-%d\r\n"
            "Content-Length: %d\r\n"
            "Keep-Alive: timeout=15, max=100\r\n"
            "Connection: Keep-alive\r\n"
            "\r\n"
            , content_type.c_str()
            , cache
            , size, size
            );
    logged_send( ar, buf, (int)strlen(buf), 0 );
}

static void bin_range_headers( AcceptRequest_t * ar, const char *filename, int from, int to, int size, const char * _content_type)
{
    char buf[2*1024];
    (void)filename;  /* could use filename to determine file type */
    
    std::string content_type;
    if( _content_type )
    {
        content_type = "Content-Type: ";
        content_type += _content_type;
        content_type += "\r\n";
    }
    
    if( DEBUG_LOG ) DoLog( "sending (%s) %d - %d / %d\n",_content_type,from, to, size );
    sprintf(buf, 
            "HTTP/1.1 206 Partial Content\r\n"
            SERVER_STRING
            "Cache-Control: private, max-age=21915\r\n"
            "%s"
            "Accept-Ranges: bytes 0-%d\r\n"
            "Content-Range: bytes %d-%d/%d\r\n"
            "Content-Length: %d\r\n"
            "Connection: Keep-alive\r\n"
            "\r\n"
            ,content_type.c_str()
            ,size
            ,from, to, size
            ,to-from+1
            );
    logged_send( ar, buf, (int)strlen(buf), 0 );
}


static bool ends_with_ext( const char * path, const char * ext )
{
    int extlen = (int)strlen(ext);
    int pathlen = (int)strlen(path);
    if( pathlen <= extlen ) return false;
    if( path[pathlen-extlen-1] != '.' ) return false;
    for( int i=0; i<extlen; i++ )
    {
        char c = path[pathlen-extlen+i];
        c = (c>='A' && c<='Z') ? (c-'A'+'a') : c;
        if( c != ext[i] ) return false;
    }
    return true;
}

/**********************************************************************/
/* Send a binary file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
static void serve_bin_file( AcceptRequest_t* ar, const char *filename, const char * content_type, bool emulated, MiniHttpDaemon::FileBuffer & emulatedResult )
{
    //if( DEBUG_LOG ) DoLog("serving binary file\n");
    
    int from = -1;
    int to = -1;
    
    HeaderPeoperties_t::iterator i = ar->properties.find( "Range" );
    if( i != ar->properties.end() )
    {
        const char * buf = i->second.c_str();
        const char * b = strstr(buf, "=");
        if(b)
        {
            b++;
            from=0;
            while( *b >= '0' && *b <= '9' ) 
            { 
                from = from * 10 + ( *b - '0' ); 
                b++; 
            }
            if( *b == '-' ) 
            {
                b++;
                to=0;
                bool to_supplied = false;
                while( *b >= '0' && *b <= '9' ) 
                { 
                    to_supplied = true;
                    to = to * 10 + ( *b - '0' ); 
                    b++; 
                }
                if( !to_supplied )
                {
                    to = -1;
                }
            }
        }
        
    }
    
    struct stat stt;
    int statresult = ar->serverinfo->serve_diskfiles ? stat( filename, &stt ) : -1;
    if( emulated || statresult != -1 )
    {
        int size = emulated ? emulatedResult.size : stt.st_size;
        int remain = size;
        bool cacheable = emulated ? emulatedResult.cacheable : true;
        time_t modifydate = emulated ? emulatedResult.modifydate : stt.st_mtime;
        
        if ( from == -1 ) 
        {
            bin_headers( ar, filename, size, content_type, cacheable, modifydate );
        }
        else 
        {
            //send range
            //check if file might be incomplete, if so, send its expected size in the headers
            while( filename[0] == '/' && filename[1] == '/' ) filename++;
            
            if( !emulated )
            {
                
                incomplete2size_t::iterator it = ar->serverinfo->incomplete2size.find( filename );
                if( it != ar->serverinfo->incomplete2size.end() )
                {
                    //when size is -1, this is a complete file, so keep size we got from stat
                    if( it->second.size != -1 )  
                    {
                        //if( DEBUG_LOG ) DoLog("incomplete --> %s\n", filename);
                        if( size == it->second.size )
                        {
                            //can be removed from the pool, file is complete
                            ar->serverinfo->incomplete2size.erase( it );
                        }
                        else 
                        {
                            size = it->second.size;
                        }
                    }
                }
                else 
                {
                    //if( DEBUG_LOG ) DoLog("unknown --> %s\n", filename);
                }
                
            }
            
            if( to == -1 ) to = size-1;
            
            bin_range_headers( ar, filename, from, to, size, content_type );
            
            remain = to - from + 1;
        }
        
#ifndef min
#define min(x,y) ( (x)<(y) ? (x) : (y) )
#endif
        
        if( from == -1 ) from = 0;
        
        char buf[8*1024];
        static const int bufsize = sizeof(buf);
        int numchars;
        
        while (remain>0)
        {
            if( emulated )
            {
                numchars = min(bufsize,remain);
                memmove(buf, emulatedResult.data+from, numchars );
            }
            else 
            {
                //incomplete2size_t::iterator i2si = ar->serverinfo->incomplete2size.find( filename );
                
                FILE * resource = fopen(filename, "rb");
                if (resource == NULL)
                {
                    //huh???
                    return;
                }
                if( fseek( resource, from, SEEK_SET ) == 0 )
                {
                    numchars = (int)fread(buf, 1, min(bufsize,remain), resource);
                }
                else 
                {
                    //error seeking to destination position
                    numchars = 0;
                }
                fclose(resource);
                                
            }
            if( numchars )
            {
                remain -= numchars;
                from += numchars;
                //if( DEBUG_LOG ) DoLog("Sending %d bytes\n", numchars);
                size_t sent = logged_send( ar, buf, numchars, 0 );
                //if( DEBUG_LOG ) DoLog("sent %ld bytes\n",sent);
                if( sent == -1 )
                {
                    //if( DEBUG_LOG ) DoLog("send pipe broken\n");
                    break;
                }
                if( sent == 0 )
                {
                    boost::thread::yield();
                }
            }
            else 
            {
                boost::thread::yield();
            }
        }
        
    }
    else 
    {
        not_found( ar );
    }
    
    //if( DEBUG_LOG ) DoLog("finished serving file\n");
}

void urldecode(char *pszDecodedOut, size_t nBufferSize, const char *pszEncodedIn)
{
    memset(pszDecodedOut, 0, nBufferSize);
    
    enum DecodeState_e
    {
        STATE_SEARCH = 0, ///< searching for an ampersand to convert
        STATE_CONVERTING, ///< convert the two proceeding characters from hex
    };
    
    DecodeState_e state = STATE_SEARCH;
    
    for(unsigned int i = 0; i < strlen(pszEncodedIn); ++i)
    {
        switch(state)
        {
            case STATE_SEARCH:
            {
                if(pszEncodedIn[i] != '%')
                {
                    strncat(pszDecodedOut, &pszEncodedIn[i], 1);
                    assert(strlen(pszDecodedOut) < nBufferSize);
                    break;
                }
                
                // We are now converting
                state = STATE_CONVERTING;
            }
                break;
                
            case STATE_CONVERTING:
            {
                // Conversion complete (i.e. don't convert again next iter)
                state = STATE_SEARCH;
                
                // Create a buffer to hold the hex. For example, if %20, this
                // buffer would hold 20 (in ASCII)
                char pszTempNumBuf[3] = {0};
                strncpy(pszTempNumBuf, &pszEncodedIn[i], 2);
                
                // Ensure both characters are hexadecimal
                bool bBothDigits = true;
                
                for(int j = 0; j < 2; ++j)
                {
                    if(!isxdigit(pszTempNumBuf[j]))
                        bBothDigits = false;
                }
                
                if(!bBothDigits)
                    break;
                
                // Convert two hexadecimal characters into one character
                int nAsciiCharacter;
                sscanf(pszTempNumBuf, "%x", &nAsciiCharacter);
                
                // Ensure we aren't going to overflow
                assert(strlen(pszDecodedOut) < nBufferSize);
                
                // Concatenate this character onto the output
                strncat(pszDecodedOut, (char*)&nAsciiCharacter, 1);
                
                // Skip the next character
                i++;
            }
                break;
        }
    }
}

static void parse_headers( AcceptRequest_t * ar )
{
    int j;
    char buf[4*1024];
    char _url[1024];
    int numchars = get_line( ar, buf, sizeof(buf) );
    j = 0;
    ar->method.clear();
    ar->url.clear();
    //get method name
    while (!ISspace(buf[j]) && (j < numchars) ) { ar->method += buf[j]; j++; }
    //skip whitespace
    while ( ISspace(buf[j]) && (j < numchars) ) j++;
    //get url
    while (!ISspace(buf[j]) && buf[j]!='?' && (j < numchars) ) { ar->url += buf[j]; j++; }
    urldecode(_url, sizeof(_url), ar->url.c_str() );
    ar->url = _url;
    if( buf[j] == '?' )
    {
        j++;
        while (!ISspace(buf[j]) && (j < numchars) ) { ar->query_string += buf[j]; j++; }
    }
    //get all parameters
    do 
    {
        numchars = get_line( ar, buf, sizeof(buf) );
        if( ( numchars > 0 ) && strcmp( "\n", buf ) ) 
        {
            //get key
            char * sep = strstr( buf, ":" );
            if( sep )
            {
                //left of the : is key right after whitespace is value
                j=0;
                while ( ISspace(buf[j]) && (j < numchars) ) j++;
                *sep = 0;
                char * val = sep+1;
                while( sep != &buf[j] && ISspace(*(--sep)) ) *sep = 0;
                std::string key = &buf[j];
                while( strlen(val) && ISspace(val[strlen(val)-1]) ) val[ strlen(val)-1 ] = 0;
                HeaderPeoperties_t::iterator i = ar->properties.find( key );
                if( i != ar->properties.end() )
                {
                    i->second = val;
                }
                else 
                {
                    ar->properties.insert( std::make_pair( key, val ) );
                }
            }
        }
        else
        {
            break;
        }
    } while( true );

}

static size_t logged_recv( AcceptRequest_t * ar, void * data, int len, int mode )
{
    int ret = recv( ar->client, reinterpret_cast<char*>(data), len, mode );
    if( ret > 0 && (mode&MSG_PEEK)==0 )
    {
        ar->serverinfo->bytes_received += ret;
    }
    return ret;
}

#ifdef _WINDOWS
#define strcasecmp stricmp
void *memmem(const void *haystack, size_t hlen, const void *needle, size_t nlen)
{
    int needle_first;
    const char *p = reinterpret_cast<const char *>(haystack);
    size_t plen = hlen;

    if (!nlen)
        return NULL;

    needle_first = *(unsigned char *)needle;

    while (plen >= nlen && (p = reinterpret_cast<const char *>(memchr(p, needle_first, plen - nlen + 1))) )
    {
        if (!memcmp(p, needle, nlen))
            return (void *)p;

        p++;
        plen = hlen - (p - reinterpret_cast<const char *>(haystack));
    }

    return NULL;
}
#endif

void get_content( AcceptRequest_t * ar )
{
    static const int bufsize = 32*1024;
    std::vector<char> buf( bufsize*2 );
    int readpos = 0;
    ar->post_data.clear();
    if ( strcasecmp(ar->method.c_str(), "POST") == 0 )
    {
        //check if i have 'Content-Length' property
        HeaderPeoperties_t::iterator i = ar->properties.find( "Content-Length" );
        if( i != ar->properties.end() )
        {
            //bool multipart = false;
            std::string boundry;
            int len = atoi( i->second.c_str() );                        //len = the content length as apperas in the HTTP POST headers
            //is this a multipart post?
            i = ar->properties.find( "Content-Type" );
            if( i != ar->properties.end() )
            {
                const char * p = i->second.c_str();
                if( strstr( p, "multipart/form-data" ) )
                {
                    //search for the boundry field
                    const char * boundry_txt = "boundary=";
                    const char * b = strstr( p, boundry_txt );
                    if( b )
                    {
                        b += strlen(boundry_txt);
                        while( *b && !ISspace(*b) ) { boundry += *b; b++; };
                        if( DEBUG_LOG ) DoLog( "MULTIPART BOUNDARY='%s'\n", boundry.c_str() );
                        //multipart = true;
                    }
                }
            }
            
            std::string boundry_start = "--";
            boundry_start += boundry;
            boundry_start += "\r\n";
            std::string boundry_end = "--";
            boundry_end += boundry;
            boundry_end += "--";
            
            std::string thefilename;
            int remain = len;
            enum BoundarySearchState_t {
                BSS_FIND_START,
                BSS_GET_CONTENT
            };
            BoundarySearchState_t bss = BSS_FIND_START;
            
            ar->post_data.clear();

            while( remain || readpos )
            {
                int toread = min( bufsize, remain );
                int got = (int)logged_recv( ar, &buf[readpos], toread, 0 );
                if( got < 0 )
                {
                    if( DEBUG_LOG ) perror("recv");
                    if( DEBUG_LOG ) DoLog("readpos = %d toread = %d\n", readpos, toread);
                    break;
                }
                else if( got == 0 && remain )
                {
                    boost::thread::yield();
                }
                else
                {
                    char *data_start = &buf[0];
                    int data_len = got + readpos;
                    int write_len = data_len;
                    //deal with content data
                    if( bss == BSS_FIND_START ) 
                    {
                        thefilename.clear();
                        if( data_len > (int)boundry_start.length() + 4/*\r\n\r\n*/ )
                        {
                            int len = data_len;
                            char * p =  (char *)memmem( &buf[0], len, boundry_start.c_str(), boundry_start.length() );
                            if( p == &buf[0] )
                            {
                                
                                len -= (int)boundry_start.length();
                                p += boundry_start.length();
                                char * p1 = (char *)memmem( p, len, "\r\n\r\n", 4 );
                                if( p1 )
                                {
                                    //found subheader end marker
                                    if( DEBUG_LOG ) DoLog("found subheader end marker\n");
                                    
                                    //check if a filename is present
                                    char * fn = (char *)memmem( p, p1-p, "filename=\"", 10 );
                                    if( fn )
                                    {
                                        fn = fn + 10;
                                        while( *fn  != '"' && fn<p1 ) 
                                        {
                                            thefilename += *fn;
                                            fn++;
                                        }
                                        if( *fn != '"' ) 
                                        {
                                            thefilename.clear();
                                        }
                                        if( !thefilename.empty() )
                                        {
                                            char _filename[512];
                                            urldecode( _filename, sizeof(_filename), thefilename.c_str() );
                                            thefilename = _filename;

                                            if( DEBUG_LOG ) DoLog("filename = %s\n", thefilename.c_str() );
                                            if( ar->serverinfo->fileUploadCallback )
                                            {
                                                if( ar->serverinfo->fileUploadCallback( ar->serverinfo->fileUploadUserdata, 0, thefilename, NULL, 0 ) == false )
                                                {
                                                    if( DEBUG_LOG ) DoLog("writing to filename was disallowed, ignoring request\n");
                                                    thefilename.clear();
                                                }
                                            }
                                        }
                                    }
                                    
                                    data_start = p1 + 4;
                                    write_len = data_len = (int) ( &buf[data_len] - data_start );
                                    bss = BSS_GET_CONTENT;
                                }
                                else 
                                {
                                    if( DEBUG_LOG ) DoLog("not enough data(0)\n");
                                    //not enought data
                                    readpos = data_len;
                                    if( readpos > (int)(buf.size()-bufsize) )
                                    {
                                        assert(!"error... should have got boundry end markerby now...");
                                    }
                                }
                            }
                            else 
                            {
                                //there is no boundry start marker
                                if( got == len ) 
                                {
                                    ar->post_data += std::string(buf.begin(),buf.begin()+len);
                                    if( DEBUG_LOG ) DoLog("POST data:\n%s\n", ar->post_data.c_str());
                                } 
                                else
                                { 
                                    if( DEBUG_LOG ) DoLog("not enough data(1)\n");
                                    //not enough data
                                    readpos = data_len;
                                    if( readpos > (int)(buf.size()-bufsize) )
                                    {
                                        assert(!"error... should have got boundry start by now...");
                                    }
                                }
                            }
                        }
                        else 
                        {
                            if( DEBUG_LOG ) DoLog("not enough data(2)\n");
                            //not enough data
                            readpos = data_len;
                            if( readpos > (int)(buf.size()-bufsize) )
                            {
                                assert(!"error... should have got boundry start by now...");
                            }
                        }
                    }
                    if( bss == BSS_GET_CONTENT)
                    {
                        bool foundmarker = false;
                        int data_remain = 0;
                        int len = data_len;
                        char * p =  (char *)memmem( data_start, len, boundry_start.c_str(), boundry_start.length() );
                        if( p )
                        {
                            //this is a new marker, meaning that up to this marker is the end of the data
                            int valid_data_len = (int) ( p - data_start );
                            data_remain = data_len - valid_data_len;
                            bss = BSS_FIND_START;
                            data_len = valid_data_len;
                            write_len = data_len - 2; //todo: ends with \n\r, this is a bug!!!! since they might have been in the last packet
                            if( DEBUG_LOG ) DoLog("found data end marker ( new boundry after ) write_len=%d\n", write_len );
                            foundmarker = true;
                        }
                        else 
                        {
                            p =  (char *)memmem( data_start, len, boundry_end.c_str(), boundry_end.length() );
                            if( p )
                            {
                                //this is a terminating marker, meaning that up to this marker is the end of data
                                data_len = (int) ( p - data_start ); 
                                write_len = data_len - 2; //todo: ends with \n\r, this is a bug!!!! since they might have been in the last packet
                                if( DEBUG_LOG ) DoLog("found end marker ( end of boundries ) write_len=%d\n", write_len );
                                foundmarker = true;
                                readpos = 0;
                            }
                        }
                        
                        if( !foundmarker )
                        {
                            //todo: if boundry_start and boundry_end dont have the same length there is a potential bug here
                            int maxsearchlen = (int) max( boundry_start.length(), boundry_end.length() );
                            if( data_len >= maxsearchlen ) 
                            {
                                data_len -= maxsearchlen-1;
                                write_len = data_len;
                                data_remain = maxsearchlen-1;
                            }
                            else 
                            {
                                readpos = data_len;
                                write_len = data_len = 0;
                            }
                        }
                        
                        //DoLog("got content with size %d --> %s\n", got, buf );
                        if( thefilename.empty() )
                        {
                            //we only save post data when doing non-file uploads
                            if( write_len ) 
                            {
                                ar->post_data += std::string( data_start, write_len );
                            }
                        }
                        else
                        {
                            if( ar->serverinfo->fileUploadCallback ) 
                            {
                                if( write_len )
                                {
                                    ar->serverinfo->fileUploadCallback( ar->serverinfo->fileUploadUserdata, 1, thefilename, data_start, write_len );
                                }
                                if( foundmarker )
                                {
                                    ar->serverinfo->fileUploadCallback( ar->serverinfo->fileUploadUserdata, 2, thefilename, NULL, 0 );
                                }
                            }
                        }
                        //for( int j=0; j<write_len; j++) putchar( data_start[j] );
                        
                        if( data_remain )
                        {
                            memmove( &buf[0], &data_start[data_len], data_remain );
                            readpos = data_remain;
                            if( foundmarker ) 
                            {
                                bss = BSS_FIND_START;
                            }
                        }
                    }
                    
                    remain -= got;
                }
            }
        }
    }
}

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client */
/**********************************************************************/
static void* accept_request(void* userData)
{
    AcceptRequest_t * ar = (AcceptRequest_t*)userData;
    char path[512];
    struct stat st;
    int cgi = 0;      /* becomes true if server decides this is a CGI
                       * program */
    
    bool abort = false;
    
#ifndef _WINDOWS
    //disable sigpipe
    int set = 1;
    setsockopt( ar->client, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int) );
#endif
    
    parse_headers( ar );
    get_content( ar );

    bool isGet = ( strcasecmp(ar->method.c_str(), "GET") == 0 );
    bool isPost = ( strcasecmp(ar->method.c_str(), "POST") == 0 );
    
    if (isGet == false && isPost == false)
    {
        //we only accept get and post commands
        unimplemented( ar );
        abort = true;
    }
    
    if ( !abort )
    {
        
        ////////////////////////////////////////////
        //convert url from URL encoding to std::string
        
        char tmppath[512];
        sprintf(tmppath, "%s%s", ar->serverinfo->root_folder.c_str(),ar->url.c_str());
        //convert the path to canonical path
#ifdef _WINDOWS
        GetFullPathNameA( tmppath, sizeof(path), path, NULL );
#else
        if( realpath( tmppath, path ) == NULL ) strcpy( path, tmppath );
#endif
        
#if APPEND_INDEX
        //if it ends with '/' append index.html
#ifdef _WINDOWS
        if (path[strlen(path) - 1] == '\\')
#else
        if (path[strlen(path) - 1] == '/')
#endif
            strcat(path, "index.html");
        
        //if this is a folder, append /index.html
        if (stat(path, &st) != -1)
        {
            if ((st.st_mode & S_IFMT) == S_IFDIR)
#ifdef _WINDOWS
                strcat(path, "/index.html");
#else
                strcat(path, "\\index.html");
#endif
        }
#endif //APPEND_INDEX
        
        if( DEBUG_LOG ) DoLog("requesting %s\n", path);
        
        //if the path returned does not prefix root_folder, somebody is trying to screw with us giving us a relative path
        if( strncmp( path, ar->serverinfo->root_folder.c_str(), ar->serverinfo->root_folder.length() ) != 0 )
        {
            if( DEBUG_LOG ) DoLog("somebody is trying to fool the server into giving him a forbidden file\n");
            abort = true;
        }
        
        if( !abort )
        {
            
            MiniHttpDaemon::FileBuffer emulatedResult;
            bool emulated = false;
            if( ar->serverinfo->fileHandlerCallback )
            {
                boost::char_separator<char> sep("&");
                boost::tokenizer< boost::char_separator<char> > tok( ar->query_string, sep );
                boost::tokenizer< boost::char_separator<char> >::iterator toki;
                
                MiniHttpDaemon::QueryParams_t query_params;
                
                for( toki=tok.begin(); toki!=tok.end(); toki++ )
                {
                    if( (*toki).length() )
                    {
                        //for each token, add it to the parameters pair map
                        std::size_t eqpos = (*toki).find( '=' );
                        std::string key;
                        std::string value;
                        if( eqpos == std::string::npos )
                        {
                            //only key
                            key = (*toki);
                        }
                        else 
                        {
                            //key and value
                            key = (*toki).substr( 0, eqpos );
                            value = (*toki).substr( eqpos+1, std::string::npos );
                        }
                        query_params.insert( std::make_pair( key, value ) );
                    }
                }
                
                emulated = ar->serverinfo->fileHandlerCallback( ar->serverinfo->fileHandlerUserdata, isPost, ar->post_data, path, query_params, emulatedResult );
            }
            
            if ( !emulated && stat(path, &st) == -1 ) 
            {
                abort = true;
            }
            else
            {    
                if( !emulated )
                {
#if ENABLE_CGI
                    if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH) )
                        cgi = 1;
#endif
                }
                
                typedef struct 
                {
                    const char * extension;
                    const char * mime_type;
                } ext2mime_t;
                
                static const ext2mime_t binmimetypes[] = 
                {
                    { "htm", "text/html" },
                    { "html", "text/html" },
                    { "htmls", "text/html" },
                    { "htx", "text/html" },
                    { "php", "text/html" },
                    { "xml", "text/xml" },
                    { "txt", "text/plain" },
                    { "text", "text/plain" },
                    
                    { "png", "image/png" },
                    { "gif", "image/gif" },
                    { "jpg", "image/jpeg" },
                    { "jfif", "image/jpeg" },
                    { "jpeg", "image/jpeg" },
                    { "jpe", "image/jpeg" },
                    { "tiff", "image/tiff" },
                    { "tif", "image/tiff" },
                    { "bmp", "image/bmp" },
                    { "ico", "image/x-icon" },
                    { "pic", "image/pict" },
                    { "pict", "image/pict" },
                    { "qif", "image/x-quicktime" },
                    
                    { "aac", "audio/aac" },
                    { "mpa", "audio/mpeg" },
                    { "mp3", "audio/mpeg" },
                    { "m2a", "audio/mpeg" },
                    { "s3m", "audio/s3m" },
                    { "m3u", "audio/x-mpequrl" },
                    { "aif", "audio/aiff" },
                    { "aiff", "audio/aiff" },
                    { "aifc", "audio/aiff" },
                    { "au", "audio/basic" },
                    { "kar", "audio/midi" },
                    { "mid", "audio/midi" },
                    { "midi", "audio/midi" },
                    { "mod", "audio/mod" },
                    { "ra", "audio/x-realaudio" },
                    { "voc", "audio/voc" },
                    { "wav", "audio/wav" },
                    { "xm", "audio/xm" },
                    
                    { "asx", "video/x-ms-asf" },
                    { "asf", "video/x-ms-asf" },
                    { "mp4", "video/mp4" },
                    { "avi", "video/avi" },
                    { "dv", "video/x-dv" },
                    { "mpe", "video/mpeg" },
                    { "mpeg", "video/mpeg" },
                    { "mpg", "video/mpeg" },
                    { "mp2", "video/mpeg" },
                    { "m1v", "video/mpeg" },
                    { "m2v", "video/mpeg" },
                    { "qt", "video/quicktime" },
                    { "mov", "video/quicktime" },
                    { "mjpg", "video/x-motion-jpeg" },
                    { "fli", "video/fli" },
                    
                    { "json", "application/json" },
                    { "doc", "application/msword" },
                    { "dot", "application/msword" },
                    { "pps", "application/mspowerpoint" },
                    { "ppt", "application/mspowerpoint" },
                    { "pdf", "application/pdf" },
                    { "swf", "application/x-shockwave-flash" },
                    { "rt", "text/richtext" },
                    { "rtf", "application/rtf" },
                    { "rtx", "application/rtf" },
                    { "xl", "application/excel" },
                    { "xls", "application/excel" },
                    { "zip", "application/zip" },
                    { "z", "application/x-compress" },
                };
                static const int binmimetypes_len = sizeof(binmimetypes)/sizeof(binmimetypes[0]);
                
                if (emulated || !cgi)
                {
                    int i;
                    bool handled = false;
                    for( i=0; i<binmimetypes_len; i++ )
                    {
                        if( ends_with_ext( path, binmimetypes[i].extension ) )
                        {
                            serve_bin_file( ar, path, binmimetypes[i].mime_type, emulated, emulatedResult );
                            handled = true;
                        }
                    }
                    if( !handled )
                    {
                        serve_bin_file( ar, path, NULL, emulated, emulatedResult );
                    }
                }
                else
                {
#if ENABLE_CGI
                    execute_cgi( ar, path );
#else //ENABLE_CGI
                    if( DEBUG_LOG ) DoLog("cgi not allowed\n");
                    cannot_execute( ar );
#endif //ENABLE_CGI
                }
            }
        }
        
        if( abort)
        {        
            not_found( ar );
        }
        
    }
        
    if( ar->serverinfo->socketHandlerCallback )
    {
        ar->serverinfo->socketHandlerCallback( ar->serverinfo->socketHandlerUserdata, ar->client, false, false );
    }

    {
        boost::mutex::scoped_lock _lk( ar->serverinfo->activeClientsLock );
        RunningClients_t::iterator i = ar->serverinfo->activeClients.find( ar );
        if( i != ar->serverinfo->activeClients.end() )
        {
            ar->serverinfo->activeClients.erase( i );
        }
        else 
        {
            //This is weird!!!
        }
        delete ar;
    }
    
    if( DEBUG_LOG ) DoLog("finished serving client\n");
        
    return NULL;
}


/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
static void bad_request( AcceptRequest_t * ar )
{
    const char * buf =  "HTTP/1.1 400 BAD REQUEST\r\n"
                        "Content-type: text/html\r\n"
                        "\r\n"
                        "<P>Your browser sent a bad request, such as a POST without a Content-Length.\r\n"
                        ;
    logged_send( ar, buf, sizeof(buf), 0 );
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc)
{
    if( DEBUG_LOG )  
    {
        perror(sc);
        exit(1);
    }
}

/**********************************************************************/
/* Execute a CGI script.  Will need to set environment variables as
 * appropriate.
 * Parameters: client socket descriptor
 *             path to the CGI script */
/**********************************************************************/
void execute_cgi(AcceptRequest_t *ar, const char *path)
{
#if ENABLE_CGI
    char buf[1024];
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    char c;
    int content_length = -1;
    
    if (strcasecmp(ar->method.c_str(), "GET") == 0)
    {
    }
    else    /* POST */
    {
        HeaderPeoperties_t::iterator i = ar->properties.find( "Content-Length" );
        if( i != ar->properties.end() )
        {
            content_length = atoi( i->second.c_str() );
        }
        else 
        {
            bad_request(ar);
            return;
        }
    }
    
    sprintf(buf, "HTTP/1.1 200 OK\r\n");
    logged_send( ar, buf, strlen(buf), 0 );
    
    if (pipe(cgi_output) < 0) {
        cannot_execute( ar );
        return;
    }
    if (pipe(cgi_input) < 0) {
        cannot_execute( ar );
        return;
    }
    
    if ( (pid = fork()) < 0 ) {
        cannot_execute( ar );
        return;
    }
    if (pid == 0)  /* child: CGI script */
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];
        
        dup2(cgi_output[1], 1);
        dup2(cgi_input[0], 0);
        close(cgi_output[0]);
        close(cgi_input[1]);
        sprintf(meth_env, "REQUEST_METHOD=%s", ar->method.c_str());
        putenv(meth_env);
        if (strcasecmp(ar->method.c_str(), "GET") == 0) {
            sprintf(query_env, "QUERY_STRING=%s", ar->query_string.c_str());
            putenv(query_env);
        }
        else {   /* POST */
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }
        execl(path, path, NULL);
        exit(0);
    } else {    /* parent */
        close(cgi_output[1]);
        close(cgi_input[0]);
        if (strcasecmp(ar->method.c_str(), "POST") == 0)
            for (int i = 0; i < content_length; i++) {
                logged_recv( ar, &c, 1, 0 );
                write(cgi_input[1], &c, 1);
            }
        while (read(cgi_output[0], &c, 1) > 0)
            logged_send( ar, &c, 1, 0 );
        
        close(cgi_output[0]);
        close(cgi_input[1]);
        waitpid(pid, &status, 0);
    }
#endif //ENABLE_CGI
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/
int get_line( AcceptRequest_t *ar, char *buf, int size )
{
    int i = 0;
    char c = '\0';
    int n;
    
    while ((i < size - 1) && (c != '\n'))
    {
        n = (int)logged_recv( ar, &c, 1, 0 );
        /* if( DEBUG_LOG ) DoLog("%02X\n", c); */
        if (n > 0)
        {
            if (c == '\r')
            {
                n = (int)logged_recv( ar, &c, 1, MSG_PEEK );
                /* if( DEBUG_LOG ) DoLog("%02X\n", c); */
                if ((n > 0) && (c == '\n'))
                    logged_recv( ar, &c, 1, 0 );
                else
                    c = '\n';
            }
            buf[i] = c;
            i++;
        }
        else
            c = '\n';
    }
    buf[i] = '\0';
    
    if( DEBUG_LOG ) DoLog("C->S: %s",buf);
    
    return(i);
}

/**********************************************************************/
/* This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 *             should accept connection from localhost only?
 * Returns: the socket */
/**********************************************************************/
static int startup(int *port, bool local_only)
{
    int httpd = 0;
    struct sockaddr_in name;
    
    //create the server socket
#ifdef _WINDOWS
    httpd = (int)socket(AF_INET, SOCK_STREAM, 0);
#else
    httpd = (int)socket(PF_INET, SOCK_STREAM, 0);
#endif
    if (httpd == -1)
        error_die("socket");
    
    //make the socket reusable
    int on = 1;
    if (setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on)) < 0)
    {
        //some error has occurred
        error_die("setsockopt");
    }
    
    //bind the socket
    
    int retries = 3;
REBIND:
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(*port);
    
    if (local_only)
    {
        //bind to local only: 127.0.0.1
        name.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    else 
    {
        name.sin_addr.s_addr = htonl(INADDR_ANY);
    }
            
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
    {
        if( DEBUG_LOG ) perror("bind");
        if( DEBUG_LOG ) DoLog("alternating port\n");
        *port = 0;
        retries--;
        if( retries ) 
        {
            goto REBIND;
        }
        else 
        {
            error_die("bind");
        }
    }
    
    if (*port == 0)  /* if dynamically allocating a port */
    {
        int namelen = sizeof(name);
                
        if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
            error_die("getsockname");
        *port = ntohs(name.sin_port);
    }

    
    if (listen(httpd, 5) < 0)
        error_die("listen");
    
    return(httpd);
}


/**********************************************************************/

MiniHttpDaemon::FileBuffer::FileBuffer()
{
    data = NULL;
    size = 0;
    cacheable = false;
    modifydate = 0;
}

MiniHttpDaemon::FileBuffer::~FileBuffer() 
{
    Release();
}

void MiniHttpDaemon::FileBuffer::Resize( int len )
{
    Release();
    if( len )
    {
        data = new unsigned char[ len ];
        size = len;
    }
}

void MiniHttpDaemon::FileBuffer::Set( const void * buf, int len )
{
    Resize( len );
    memcpy( data, buf, len );
}

void MiniHttpDaemon::FileBuffer::Release( void )
{
    if( data )
    {
        delete [] data;
    }
    data = NULL;
    size = 0;
}

/**********************************************************************/

MiniHttpDaemon::MiniHttpDaemon()
{
    private_data = new MiniHtmlInfo_t;
    ((MiniHtmlInfo_t*)private_data)->thread_started = false;
    ((MiniHtmlInfo_t*)private_data)->port = 0;
    ((MiniHtmlInfo_t*)private_data)->serve_diskfiles = true;
    ((MiniHtmlInfo_t*)private_data)->fileHandlerCallback = NULL;
    ((MiniHtmlInfo_t*)private_data)->socketHandlerCallback = NULL;
    ((MiniHtmlInfo_t*)private_data)->fileUploadCallback = NULL;
}

MiniHttpDaemon::~MiniHttpDaemon()
{
    Stop();
    delete (MiniHtmlInfo_t*)private_data;
}

void MiniHttpDaemon::SetServeDiskFiles( bool serve )
{
    ((MiniHtmlInfo_t*)private_data)->serve_diskfiles = serve;
}

int MiniHttpDaemon::GetBytesSent()
{
    return ((MiniHtmlInfo_t*)private_data)->bytes_sent;
}

int MiniHttpDaemon::GetBytesReceived()
{
    return ((MiniHtmlInfo_t*)private_data)->bytes_received;
}

int MiniHttpDaemon::GetPort( void )
{
    return ((MiniHtmlInfo_t*)private_data)->port;
}

void MiniHttpDaemon::RegisterFileHandler( FileHandlerCallback_t callback, void * userData )
{
    ((MiniHtmlInfo_t*)private_data)->fileHandlerCallback = callback;
    ((MiniHtmlInfo_t*)private_data)->fileHandlerUserdata = userData;
}

void MiniHttpDaemon::RegisterUploadHandler( FileUploadCallback_t callback, void * userData )
{
    ((MiniHtmlInfo_t*)private_data)->fileUploadCallback = callback;
    ((MiniHtmlInfo_t*)private_data)->fileUploadUserdata = userData;
}

void MiniHttpDaemon::RegisterSocketHandler( SocketHandlerCallback_t callback,  void *userData )
{
    ((MiniHtmlInfo_t*)private_data)->socketHandlerCallback = callback;
    ((MiniHtmlInfo_t*)private_data)->socketHandlerUserdata = userData;
}

void MiniHttpDaemon::RegisterFile( const char * filename )
{
    RegisterIncompleteFile( filename, -1 );
}

void MiniHttpDaemon::RegisterIncompleteFile( const char * _filename, int completeSize )
{
    char filename[512];
#ifdef _WINDOWS
    GetFullPathNameA( _filename, sizeof(filename), filename, NULL );
#else
    if( realpath( _filename, filename ) == NULL ) strcpy( filename, _filename );
#endif
    incomplete2size_t::iterator i = ((MiniHtmlInfo_t*)private_data)->incomplete2size.find( filename );
    if( i == ((MiniHtmlInfo_t*)private_data)->incomplete2size.end() )
    {
        IncompleteInfo_t ii;
        ii.size = completeSize;
        ((MiniHtmlInfo_t*)private_data)->incomplete2size.insert( std::make_pair( filename, ii ) );
    }
    else 
    {
        i->second.size = completeSize;
    }
    //if( DEBUG_LOG ) DoLog("set --> %s complete --> %d\n", filename, completeSize);
}

static void * MiniHttpDaemon_serverthread( void * userData )
{
    MiniHtmlInfo_t * ji = (MiniHtmlInfo_t *)userData;
    ji->thread_started = true;
    ji->server_sock = -1;
    ji->bytes_sent = 0;
    ji->bytes_received = 0;
    int client_sock = -1;
    struct sockaddr_in client_name;
    int client_name_len = sizeof(client_name);
    
    ji->server_sock = startup(&ji->port, ji->local_only);
    if( DEBUG_LOG ) DoLog("httpd running on port %d\n", ji->port);
    
    if( ji->socketHandlerCallback )
    {
        ji->socketHandlerCallback( ji->socketHandlerUserdata, ji->server_sock, true, true );
    }
        
    while ( ji->thread_shouldrun && ji->server_sock != -1 )
    {
        client_sock = (int)accept(ji->server_sock, (struct sockaddr *)&client_name, &client_name_len);
        
        
        if (client_sock == -1)
        {
            if( DEBUG_LOG ) perror("accept");
        }
        else
        {
            if( ji->socketHandlerCallback )
            {
                ji->socketHandlerCallback( ji->socketHandlerUserdata, client_sock, true, false );
            }
            
            AcceptRequest_t * ar = new AcceptRequest_t( client_sock );
            ar->serverinfo = ji;
            ar->thread = boost::thread( accept_request, (void*)ar );
            {
                boost::mutex::scoped_lock _lk( ji->activeClientsLock );
                ji->activeClients.insert( std::make_pair( ar, 0 ) );
            }
        }
        boost::thread::yield();
    }
    
    {
        //disconnect all active clients
        boost::mutex::scoped_lock _lk( ji->activeClientsLock );
        RunningClients_t::iterator i = ji->activeClients.begin();
        while ( i != ji->activeClients.end() ) 
        {
            i->first->Kill();
            i++;
        }
    }
    
    int remainingClients;
    do  
    {
        boost::mutex::scoped_lock _lk( ji->activeClientsLock );
        remainingClients = (int)ji->activeClients.size();
        if( remainingClients ) boost::thread::yield();
        DoLog("remaining clients = %d\n", remainingClients);
    } while( remainingClients );
    
    if( DEBUG_LOG ) DoLog("httpd stopped\n");
    if( ji->server_sock != -1 )
    {
        if( ji->socketHandlerCallback )
        {
            ji->socketHandlerCallback( ji->socketHandlerUserdata, ji->server_sock, false, true );
        }
 
#ifdef _WINDOWS
        closesocket( ji->server_sock );
#else
        close( ji->server_sock );
#endif
        ji->server_sock = -1;
    }
    ji->thread_started = false;

    return 0;
}

void MiniHttpDaemon::Stop( void )
{
    //todo: kill all active clients
    if( ((MiniHtmlInfo_t*)private_data)->thread_started )
    {
        ((MiniHtmlInfo_t*)private_data)->thread_shouldrun = false;
        
        if( DEBUG_LOG ) DoLog("shutting down server\n");
        int ssock = ((MiniHtmlInfo_t*)private_data)->server_sock;
        ((MiniHtmlInfo_t*)private_data)->server_sock = -1;
        if( ((MiniHtmlInfo_t*)private_data)->socketHandlerCallback )
        {
            ((MiniHtmlInfo_t*)private_data)->socketHandlerCallback( ((MiniHtmlInfo_t*)private_data)->socketHandlerUserdata, ssock, false, true );
        }

        shutdown( ssock, SD_BOTH );
#ifdef _WINDOWS
        closesocket( ssock );
#else
        close( ssock );
#endif
        ((MiniHtmlInfo_t*)private_data)->server_thread.join();
        ((MiniHtmlInfo_t*)private_data)->port = 0;
        if( DEBUG_LOG ) DoLog("server thread terminated\n");
    }
}

const char * MiniHttpDaemon::GetRootFolder()
{
    return ((MiniHtmlInfo_t*)private_data)->root_folder.c_str();
}

bool MiniHttpDaemon::Start( const char * _rootFolder, int use_port, bool local_only )
{
    char rootFolder[512];
#ifdef _WINDOWS
    GetFullPathNameA( _rootFolder, sizeof(rootFolder), rootFolder, NULL );
#else
    if( realpath(_rootFolder, rootFolder) == NULL ) strcpy( rootFolder, _rootFolder );
#endif
    ((MiniHtmlInfo_t*)private_data)->root_folder = rootFolder;
    ((MiniHtmlInfo_t*)private_data)->port = use_port;
    ((MiniHtmlInfo_t*)private_data)->local_only = local_only;
    ((MiniHtmlInfo_t*)private_data)->thread_started = false;
    ((MiniHtmlInfo_t*)private_data)->thread_shouldrun = true;

    
    ((MiniHtmlInfo_t*)private_data)->server_thread = boost::thread( MiniHttpDaemon_serverthread, private_data );

    while( ((MiniHtmlInfo_t*)private_data)->thread_started == false )
    {
        boost::thread::yield();
    }
    return true;
}

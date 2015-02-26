
#ifndef __MINIHTTPD_H__
#define __MINIHTTPD_H__

#include <string>
#include <map>

void urldecode(char *pszDecodedOut, size_t nBufferSize, const char *pszEncodedIn);

class MiniHttpDaemon
{
    
public:
    
    class FileBuffer
    {
    public:
        FileBuffer();
        ~FileBuffer();
        void Set( const void * buf, int len );
        void Release( void );
        void Resize( int len );
        unsigned char * data;
        int size;
        bool cacheable;
        time_t modifydate;
    };
    
    typedef std::map< std::string, std::string > QueryParams_t;
    
    //if returns false, request is handled normally, else returns the content of reply (acting as if this file is available)
    typedef bool (*FileHandlerCallback_t)( void * userData, bool isPost, std::string & postData, std::string fileRequested, QueryParams_t & queryParams, FileBuffer & reply );
    //allows you to modify the socket on cretion (isOpening) or closure(!isOpening), it can be a server socket or a client socket
    typedef void (*SocketHandlerCallback_t)( void * userData, int socket, bool isOpening, bool isServer );
    
    //if returns true, file is valid for writing (will proceed with calls)
    // state is 0 = create, 1 = append, 0 = close
    typedef bool (*FileUploadCallback_t)( void * userData, int state, std::string filename, const void * fileData, const int dataLen );
    
public:
    
    //if serve all is false, only registered files are served
    MiniHttpDaemon();
    virtual ~MiniHttpDaemon();

    //local_only - if true accepts connection only from localhost, else from anywhere
    //use_port - 0 for auto-assign, else port wanted, may change still autoassign if port is in use, check with GerPort()
    bool Start( const char * rootFolder, int use_port=0, bool local_only=false );
    const char * GetRootFolder();
    void Stop( void );
    int GetPort();
    int GetBytesSent();
    int GetBytesReceived();
    void SetServeDiskFiles( bool serve );
    void RegisterIncompleteFile( const char * filename, int completeSize );
    void IncompleteLock( const char * filename );
    void IncompleteUnlock( const char * filename );
    void RegisterFile( const char * filename );
    void RegisterFileHandler( FileHandlerCallback_t callback, void * userData );
    void RegisterSocketHandler( SocketHandlerCallback_t callback, void * userData );
    void RegisterUploadHandler( FileUploadCallback_t callback, void * userData );
    
private:
    void * private_data;
    
};

#endif //__MINIHTTPD_H__

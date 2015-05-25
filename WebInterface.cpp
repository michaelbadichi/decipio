#include "stdafx.h"
#include "WebInterface.h"
#include "log.h"
#include "process_util.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/tokenizer.hpp>

DecipioWebInterface::DecipioWebInterface( int listeningPort )
    :m_IsRunning( false )
    ,m_ListeningPort( listeningPort )
{ 
}

DecipioWebInterface::~DecipioWebInterface()
{
    Stop();
}

static CProcess * g_TheProcess = NULL;
typedef struct {
	PVOID base;
	unsigned int offset;
    DWORD value;
    int type;
} Addr_t;
typedef std::vector< Addr_t > AddrList_t;
static AddrList_t g_AddressList;

static void MyAddResultCallback( PVOID base, unsigned int offset, DWORD value, int type, void *userData )
{
    if( g_AddressList.size() < 100 )
	{
		Addr_t a;
		a.base = base;
		a.offset = offset;
        a.value = value;
        a.type = type;
		g_AddressList.push_back(a);
	}
	else
	{
		g_TheProcess->SetAddResultCallback( NULL, NULL );
	}
}

static bool MyFileHandler( void * userData, bool isPost, std::string & postData, std::string fileRequested, MiniHttpDaemon::QueryParams_t & queryParams, MiniHttpDaemon::FileBuffer & reply )
{
    static CRITICAL_SECTION lock;
    static bool isInitialized = false;
    if( !isInitialized ) {
        InitializeCriticalSection( &lock );
        isInitialized = true;
    }
    //strip the path
    MiniHttpDaemon * pDaemon = (MiniHttpDaemon*)userData;
    const char * rf = pDaemon->GetRootFolder();
    if( strncmp( fileRequested.c_str(), rf, strlen( rf ) ) == 0 ) 
    {
        fileRequested = fileRequested.substr( strlen( rf )+1/* include \ */, std::string::npos );
    }
    DoLog("Got %s uri request: %s\n", isPost ? "get" : "post",  fileRequested.c_str());
    boost::char_separator<char> sep("\\");
    boost::tokenizer<boost::char_separator<char>> tokens(fileRequested, sep);
    boost::tokenizer<boost::char_separator<char>>::iterator i;
    i = tokens.begin();
    if( i != tokens.end() ) 
    {
       //extract the uri
        if( (*i).compare( "api" ) == 0 )
        {
            i++;
            if( i != tokens.end() ) 
            {
                if( (*i).compare( "config" ) == 0 )
                {
                    HKEY hKey;
                    const char * REG_KEY_BASE  = "SOFTWARE\\AlienAssembly\\decipio\\";
                    LSTATUS rc = RegOpenKeyExA( HKEY_CURRENT_USER, REG_KEY_BASE, 0, KEY_ALL_ACCESS, &hKey );
                    //if this is a post request - update configuration, else fetch it
                    if( isPost ) {
                        //set config
 	                    if( rc != ERROR_SUCCESS ) {
		                    rc = RegCreateKeyExA( HKEY_CURRENT_USER, REG_KEY_BASE, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hKey, NULL );
	                    }
	                    if( rc == ERROR_SUCCESS ) {
                            RegSetValueExA( hKey, "Config", 0, REG_BINARY, (BYTE*)postData.c_str(), (DWORD)postData.length() );
                        }                    
                    } 
                    else 
                    {
                        //get config
                        if( rc == ERROR_SUCCESS )  {
                            DWORD dataSize = 0;
			                RegQueryValueExA( hKey, "Config", NULL, NULL, NULL, &dataSize );
			                if( dataSize ) {
				                std::vector<char> buf( dataSize );
				                if( RegQueryValueExA( hKey, "Config", NULL, NULL, (LPBYTE)&buf[0], &dataSize ) == ERROR_SUCCESS ) {
                                    reply.Set( &buf[0], dataSize );
                                }
                            }
                        }
                    }
                    if( rc == ERROR_SUCCESS ) {
                        RegCloseKey( hKey );
                    } 
                    else {
                        //error accessing registry...
                    }
                    return true;
                }
                else if( (*i).compare( "processes" ) == 0 ) 
                {
                    std::string ret;
                    ret = "[";
                    ProcessList_t pl;
	                GetAvailableProcesses( pl );
	                ProcessList_t::iterator p;
	                int t=0;
	                for( p=pl.begin(); p!=pl.end(); p++ ) {
                        char b[1024];
                        std::string fn = (*p).filename;
                        boost::replace_all( fn, "\\", "\\\\" );
                        sprintf( b, "%s{\"name\":\"%s\",\"pid\":%d,\"filename\":\"%s\"}", (p==pl.begin()) ? "" : ",", (*p).name.c_str(), (*p).id, fn.c_str() ); 
		                ret += b;
                    }
                    ret += "]";
                    reply.Set( ret.c_str(), (int)ret.size() );
                    return true;
                }
                else if( (*i).compare( "select-process" ) == 0 )
                {
                    //next token is process id
                    i++;
                    if( i != tokens.end() ) 
                    {
                        int pid = atoi( (*i).c_str() );
                        if( g_TheProcess ) {
                            delete g_TheProcess;
                        }
                        DoLog("Opening process id %d\n", pid);
                        g_TheProcess = new CProcess( pid );
                        return true;
                    }
                }
                else if( (*i).compare( "search" ) == 0 )
                {
                    //next token is the mode
                    i++;
                    if( i != tokens.end() )
                    {
                        CProcess::SearchMode_t mode;
                        bool ok = true;
                        if( (*i).compare( "exact" ) == 0 )
                        {
                            mode = CProcess::SM_EXACT;
                        }
                        else if( (*i).compare( "greater" ) == 0 )
                        {
                            mode = CProcess::SM_GREATER;
                        }
                        else if( (*i).compare( "lower" ) == 0 )
                        {
                            mode = CProcess::SM_LOWER;
                        }
                        else if( (*i).compare( "equal" ) == 0 )
                        {
                            mode = CProcess::SM_EQUAL;
                        }
                        else if( (*i).compare( "refresh" ) == 0 )
                        {
                            mode = CProcess::SM_REFRESH;
                        }
                        else ok = false;
                        if( ok )
                        {
                                
                            if( mode == CProcess::SM_EXACT ) 
                            {
                                //next token is the value to search
                                i++;
                            }
                            if( i != tokens.end() ) 
                            {
                                unsigned int val = ( mode == CProcess::SM_EXACT ) ? strtoul( (*i).c_str(), NULL, 10 ) : 0;
                                if( g_TheProcess ) 
                                {
                                    EnterCriticalSection( &lock );                    
                                    if( g_TheProcess->Open() ) 
                                    {
                                        g_AddressList.clear();
                                        g_TheProcess->SetAddResultCallback( MyAddResultCallback, NULL );
                                        unsigned int matches = g_TheProcess->Search( mode, val, CProcess::SS_DWORD, true );
		                                char b[1024];
                                        sprintf(b, "{\"count\":%u,\"match\":%u}",g_TheProcess->GetScanCount(), matches);
                                        reply.Set( b, (int)strlen(b) );
                                        g_TheProcess->Close();
                                        LeaveCriticalSection( &lock );
                                        return true;
                                    }
                                    LeaveCriticalSection( &lock );
                                }
                            }
                        }
                    }
                }
                else if( (*i).compare( "list" ) == 0 )
                {
                    if( g_TheProcess ) 
                    {
                        std::string s = "[";
                        for( AddrList_t::iterator k = g_AddressList.begin(); k != g_AddressList.end(); k++ )
                        {
                            char b[128];
                            sprintf( b, "%s{\"address\":\"%08X::%08X\",\"value\":%u,\"type\":%d}", (k==g_AddressList.begin()) ? "" : ",", (*k).base, (*k).offset, (*k).value, (*k).type );
                            s += b;
                        }
                        s += "]";
                        reply.Set( s.c_str(), (int)s.size() );
                        return true;
                    }
                }
                else if( (*i).compare( "get" ) == 0 )
                {
                    //next token is address base::offset
                    i++;
                    if( i != tokens.end() ) 
                    {
                        if( g_TheProcess )
                        {
                            if( g_TheProcess->Open() ) 
                            {
                                bool retcode = false;
                                unsigned int value;
                                PVOID base = NULL;
                                unsigned int offset = 0;
                                sscanf( (*i).c_str(), "%08X::%08X", &base, &offset );
                                if( g_TheProcess->GetValue( base, offset, CProcess::SS_DWORD, value ) ) 
                                {
                                    char b[128];
                                    sprintf( b, "%u", value );
                                    reply.Set( b, (int)strlen(b) );
                                    retcode = true;
                                }
                                g_TheProcess->Close();
                                return retcode;
                            }
                        }
                    }
                }
                else if( (*i).compare( "set" ) == 0 )
                {
                    //next token is address base::offset
                    i++;
                    if( i != tokens.end() ) 
                    {
                        PVOID base = NULL;
                        unsigned int offset = 0;
                        sscanf( (*i).c_str(), "%08X::%08X", &base, &offset );
                        //next token is value
                        i++;
                        if( i != tokens.end() )
                        {
                            unsigned int value = strtoul( (*i).c_str(), NULL, 10 );
                            if( g_TheProcess )
                            {
                                bool retcode = false;
                                if( g_TheProcess->Open( true ) )
                                {
			                        if( g_TheProcess->SetValue( base, offset, CProcess::SS_DWORD, value ) ) 
                                    {
                                        char b[128];
                                        sprintf( b, "%u", value );
                                        reply.Set( b, (int)strlen(b) );
                                        retcode = true;
			                        } 
                                    g_TheProcess->Close();
                                    return retcode;
                                }                                   
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

bool DecipioWebInterface::Start()
{
    if( !m_IsRunning )
    {
        m_Daemon.RegisterFileHandler( MyFileHandler, &m_Daemon );
        m_Daemon.Start( "www", m_ListeningPort );        
        m_IsRunning = true;
    }
    return m_IsRunning;
}

void DecipioWebInterface::Stop()
{
    if( m_IsRunning ) 
    {
        m_Daemon.Stop();
        m_IsRunning = false;
    }
}
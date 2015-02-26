#include "Afx.h"
#include <Psapi.h>
#include <TlHelp32.h>
#include "process_util.h"
#include "searching.h"

#pragma comment (lib, "psapi.lib")

#define SUSPEND_WHILE_SEARCHING 0

//////////////////////////////////////////////////////////////////////////

void GetAvailableProcesses(ProcessList_t & dest)
{
	dest.clear();
	//i assume no more than 1024 processes would be opened at a single time
	DWORD idProcesses[1024];	
	DWORD cbNeeded;
	EnumProcesses(idProcesses,sizeof(idProcesses),&cbNeeded);
	//get number of processes:
	cbNeeded /= sizeof(DWORD);
	for(DWORD i=0; i<cbNeeded; i++) {
		//add this process into the map
		//get process name
		HANDLE hProcess = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, idProcesses[i] );
		if(hProcess) {
			HMODULE hMod;
			DWORD cbNeeded;
			if ( EnumProcessModules( hProcess, &hMod, sizeof(hMod), &cbNeeded) ) {
				char szProcessName[MAX_PATH]="unknown";
                char szImageName[MAX_PATH]="unknown";
				GetModuleBaseName( hProcess, hMod, szProcessName, sizeof(szProcessName) );
                GetProcessImageFileName( hProcess, szImageName, sizeof(szImageName) );
				ProcessInfo_t pi;
				pi.name = szProcessName;
                pi.filename = szImageName;
				pi.id = idProcesses[i];
				dest.push_back(pi);
			}
			CloseHandle( hProcess );
		}
	}
}

//////////////////////////////////////////////////////////////////////////

CProcess::CProcess( unsigned int ProcessID )
{
	SetProcessID( ProcessID );
}

//////////////////////////////////////////////////////////////////////////

void CProcess::SetProcessID( unsigned int ProcessID )
{
	m_ProcessID = ProcessID;
	m_IsSuspended = false;
	m_IsOpened = false;
	m_AddResultsCB = NULL;
	Reset();
}

//////////////////////////////////////////////////////////////////////////

unsigned int CProcess::GetProcessID( void )
{
	return m_ProcessID;
}

//////////////////////////////////////////////////////////////////////////

unsigned int CProcess::GetScanCount( void )
{
	return m_ScanCount;
}

//////////////////////////////////////////////////////////////////////////

CProcess::~CProcess()
{
	Resume();
	Close();
}

//////////////////////////////////////////////////////////////////////////

void CProcess::Reset( void )
{
	m_ScanCount = 0;
	m_CurrentMatchCount = 0;
}

//////////////////////////////////////////////////////////////////////////

bool CProcess::Suspend( void )
{
	if ( m_IsOpened ) {
		HANDLE hSnapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, m_ProcessID );
		THREADENTRY32 te;
		te.dwSize = sizeof( THREADENTRY32 );
		BOOL ret = Thread32First( hSnapshot, &te );
		while( ret ) {
			if( te.th32OwnerProcessID == m_ProcessID ) {
				HANDLE hThread = OpenThread( THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID );
				if( hThread ) {
					SuspendThread( hThread );
					CloseHandle( hThread );
				}
			}	
			ret = Thread32Next( hSnapshot, &te );
		}
		CloseHandle( hSnapshot );
		m_IsSuspended = true;
		return true;
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////

bool CProcess::Resume( void )
{
	if( m_IsOpened ) {
		if( m_IsSuspended ) {
			HANDLE hSnapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, m_ProcessID );
			THREADENTRY32 te;
			te.dwSize = sizeof(THREADENTRY32);
			BOOL ret = Thread32First( hSnapshot, &te );
			while( ret ) {
				if( te.th32OwnerProcessID == m_ProcessID ) {
					HANDLE hThread = OpenThread( THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID );
					if( hThread ) {
						ResumeThread( hThread );
						CloseHandle( hThread );
					}
				}
				ret = Thread32Next( hSnapshot, &te );
			}
			CloseHandle( hSnapshot );
		}
		m_IsSuspended = false;
		return true;
	}
	return false;
}
//////////////////////////////////////////////////////////////////////////

bool CProcess::Open( bool write )
{
    DWORD access;
    if( write ) {
        access = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    } else {
        access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    }
	m_ProcessHandle = OpenProcess( access, FALSE, m_ProcessID);
	if( m_ProcessHandle ) {
		m_IsOpened = true;
		return true;
	} 
	return false;
}

//////////////////////////////////////////////////////////////////////////

bool CProcess::Close( void )
{
	if( m_IsOpened ) {
		CloseHandle( m_ProcessHandle );
		m_IsOpened = false;
		return true;
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////

bool CProcess::RescanRegions( void )
{
	SIZE_T ret;
    MEMORY_BASIC_INFORMATION mbi;
	char log[1024];
    if( m_IsOpened ) {
		static PVOID kernelBase=0;
		if(!kernelBase) {
            HMODULE kernMod = GetModuleHandle( "KERNEL32.DLL" );
            VirtualQuery( kernMod, &mbi, sizeof( mbi ) );
            kernelBase = mbi.AllocationBase;
            sprintf_s(log, sizeof(log), "Kernel base address is: %p\n", kernelBase);
		    OutputDebugString(log);   
		}
		m_Regions.clear();
		int i = 0;	
		if( m_ProcessHandle ) {
			//query process address space
			PVOID address = 0;
			do {
                ret = VirtualQueryEx( m_ProcessHandle, reinterpret_cast< LPCVOID >( address ), &mbi, sizeof( mbi ) );
                if( ret )
                {
                    switch( mbi.State ) {
					    //case MEM_RESERVE:
					    //case MEM_FREE:
					    //we deal with committed memory only!
					    case MEM_COMMIT:
                            //we only care about writable pages
						    switch( mbi.AllocationProtect ) {
							    //case PAGE_NOACCESS:  
							    //case PAGE_READONLY: 
							    case PAGE_READWRITE:
							    case PAGE_WRITECOPY:    
							    //case PAGE_EXECUTE:     
							    //case PAGE_EXECUTE_READ:     
							    case PAGE_EXECUTE_READWRITE:     
							    case PAGE_EXECUTE_WRITECOPY:     
							    //case PAGE_GUARD:     
							    //case PAGE_NOCACHE:     
							    case PAGE_WRITECOMBINE:     
								    //we only interested in page r/w permitted pages,
								    //this is where the data is (it might be also
								    //in execute pages, but unlikely)
								    //add this region to the list
								    if( mbi.BaseAddress < kernelBase ) {
                                        sprintf_s(log, sizeof(log), "Found rw data region: ALLOC_BASE: %p / BASE: %p (%d bytes)\n", mbi.AllocationBase, mbi.BaseAddress, mbi.RegionSize);
				                        OutputDebugString(log);
									    RegionInfo_t t;
									    t.base_address = mbi.BaseAddress;
									    t.size = mbi.RegionSize;
									    m_Regions.push_back( t );
									    i++;
								    } else {
                                        //we can finish our search
                                        ret = 0;
                                    }
								    break;
							    default:
								    //do nothing
								    break;
						    }
						    break;
					    default:
						    //do nothing
						    break;
				    }
				    address = (PVOID)( (unsigned long)mbi.BaseAddress + (unsigned long)mbi.RegionSize );
                }
			} while ( ret == sizeof( mbi ) );
		}
		return true;
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////

char* CProcess::GetRegionMemory( RegionInfo_t &ri )
{
	char* retcode = NULL;
	if( m_ProcessHandle ) {
		//lets read the process memory...
		SIZE_T readed;
		char *buffer = new char[ ri.size ];
		if( ReadProcessMemory( m_ProcessHandle, reinterpret_cast< LPCVOID >( ri.base_address ), buffer, ri.size, &readed ) ) {
			retcode = buffer;			
		} else {
			DWORD err = GetLastError();
			if( err == 0x12B ) {
				//this region might not exist any longer...
				if( readed ) {
					retcode = buffer;
				} 
			} 
			if( retcode == NULL ) {
				delete buffer;
			}
		}
	} 
	return retcode;
}


//////////////////////////////////////////////////////////////////////////

void CProcess::SetAddResultCallback( AddResultCallback_t callbackFn, void * userData )
{
	m_AddResultsUserData = userData;
	m_AddResultsCB = callbackFn;
}

//////////////////////////////////////////////////////////////////////////

bool CProcess::GetValue( PVOID base, unsigned int offset, SearchSize_t size, unsigned int & value )
{
	SIZE_T readed;
	SIZE_T readsize = 4 >> size;
	value = 0;
	BOOL ret = ReadProcessMemory( m_ProcessHandle, reinterpret_cast<LPCVOID>( (unsigned long)base + offset ), &value, readsize, &readed );
	if( ret && readed == readsize )
	{
		return true;
	}
	else
	{
		return false;
	}
}

//////////////////////////////////////////////////////////////////////////

bool CProcess::SetValue( PVOID base, unsigned int offset, SearchSize_t size, unsigned int value )
{
	SIZE_T written;
	SIZE_T writesize = 4 >> size;
	BOOL ret = WriteProcessMemory( m_ProcessHandle, reinterpret_cast<LPVOID>( (unsigned long)base + offset ), &value, writesize, &written );
	if( ret && written == writesize )
	{
		return true;
	}
	else
	{
		return false;
	}
}

//////////////////////////////////////////////////////////////////////////

template <class Type_V> int CProcess::MemoryExactSearch( void * mem, SIZE_T size, Type_V value, CFile & f, CProcess::Address_t & address, bool isAligned )
{
    char * v = (char*)mem;
    int matchCount = 0;
    int grow = isAligned ? sizeof(Type_V) : 1;
    for(unsigned int t=0; t<=size-sizeof(Type_V); t+=grow) {
		if((*(Type_V*)v)==value) {
			address.offset = t;
			f.Write(&address,sizeof(address));
			if(m_AddResultsCB) m_AddResultsCB( address.base, address.offset, address.value, address.type, m_AddResultsUserData );
			matchCount++;
		}
		v+=grow;
	}
    return matchCount;
}

//////////////////////////////////////////////////////////////////////////

template <class Type_V>
int CProcess::MemoryConditionalSearch( PVOID rgnbase, SIZE_T rgnsize, char * oldp, char * curp, CFile & f, CProcess::SearchMode_t mode, int type, Type_V value, CProcess::SearchSize_t size, bool isAligned )
{
    CProcess::Address_t address;
    address.type = type;
    int matchCount = 0;
    unsigned int offset = 0;
    unsigned int searchsize = 4 >> size;
    unsigned int incsize = isAligned ? searchsize : 1;
    while( (offset+searchsize) <= rgnsize ) {
        Type_V d = *((Type_V*)curp);
        Type_V o = *((Type_V*)oldp);
        if( ( mode == CProcess::SM_EXACT && d == value ) ||
            ( mode == CProcess::SM_GREATER && d > o ) || 
            ( mode == CProcess::SM_LOWER && d < o ) ||
            ( mode == CProcess::SM_EQUAL && d == o ) ||
            ( mode == CProcess::SM_REFRESH ) )
        {
            address.base = rgnbase;
            address.offset = offset;
            address.value = d;
            if( mode != CProcess::SM_REFRESH ) f.Write( &address, sizeof(address) );   //no need to write when we refresh
            //but we report the new value
		    if( m_AddResultsCB ) m_AddResultsCB( address.base, address.offset, address.value, address.type, m_AddResultsUserData );
		    matchCount++;
	    }
        oldp += incsize;
        curp += incsize;
        offset += incsize;
    }
    return matchCount;
}

//////////////////////////////////////////////////////////////////////////

unsigned int CProcess::Search( SearchMode_t mode, unsigned int value, SearchSize_t size, bool isAligned )
{
    unsigned int matchCount = 0;
	//create a temporary file to hold results
	char tempPath[MAX_PATH];
	char tempFilename[MAX_PATH];
	GetTempPath( sizeof(tempPath), tempPath );
	CString tempFilename2;
	tempFilename2 = tempPath;
	tempFilename2 += "decipio.srch.old";
	GetTempFileName( tempPath, "dcp", 0, tempFilename );
	CFile f;
	DeleteFile( tempFilename );
	f.Open( tempFilename, CFile::modeCreate|CFile::modeWrite );
#if SUSPEND_WHILE_SEARCHING
    OutputDebugString("Suspending process...\n");
	Suspend();
#endif //SUSPEND_WHILE_SEARCHING
    OutputDebugString("Scanning regions...\n");
	RescanRegions();
	Address_t address;
	unsigned int searchsize = 4 >> size;
    DWORD andval = 0;
    memset( &andval, 0xFF, searchsize );
    if( m_ScanCount == 0 ) {
        //This is the first scan
        if( mode != SM_EXACT ) {
            //full dump (value unknown)
            RegionsMap_t::iterator i;
            for(i=m_Regions.begin(); i!=m_Regions.end(); i++) {
                //OutputDebugString("Dumping region...\n");
                char *regionData = GetRegionMemory( *i );
                if( regionData ) {
                    f.Write( &(*i).base_address, sizeof(PVOID) );
                    f.Write( &(*i).size, sizeof(SIZE_T) );
                    f.Write( regionData, (UINT)(*i).size );
                    if(m_AddResultsCB) {
                        unsigned long offset = 0;
                        while(m_AddResultsCB && (offset+searchsize) <= (*i).size) {
                            DWORD d = (*((DWORD*)&regionData[offset])) & andval;
							m_AddResultsCB( (*i).base_address, offset, d, 1/*type*/, m_AddResultsUserData );
                            offset += searchsize;
                        }
                    }
                    matchCount += (unsigned int)(*i).size / searchsize;
                    delete [] regionData;
                }
            }
            m_LastScanIsFullDump = true;
        } 
        else {
            //exact value search
            RegionsMap_t::iterator i;
		    for(i=m_Regions.begin(); i!=m_Regions.end(); i++) {
			    //OutputDebugString("Searching region...\n");
		        char *regionData = GetRegionMemory( *i );
			    if( regionData ) {
				    address.base = (*i).base_address;
                    address.value = value;
                    address.type = 1;
				    switch(size) {					
						    case SS_BYTE:
                                matchCount += MemoryExactSearch<unsigned char>( (void*)regionData, (*i).size, value, f, address, false );
							    break;
						    case SS_SHORT:
							    matchCount += MemoryExactSearch<unsigned short>( (void*)regionData, (*i).size, value, f, address, isAligned );
							    break;
						    case SS_DWORD:
							    if( isAligned ) {
								    matchCount += MemoryExactSearch<DWORD>( (void*)regionData, (*i).size, value, f, address, true );
							    } else {
								    DWORD d=0;
                                    unsigned char* v=(unsigned char*)regionData;
								    DWORD buflen = (DWORD)(*i).size;
								    DWORD t = findfast(&v[d],buflen,(unsigned char*)&value,4);
								    while(t <(int)(buflen-3)) {
									    int skip = t+1;
									    address.offset = d+t;
									    f.Write(&address,sizeof(address));
									    if(m_AddResultsCB) m_AddResultsCB( address.base, address.offset, address.value, address.type, m_AddResultsUserData );
									    matchCount++;
									    //found at d location
									    buflen-=skip;
									    d+=skip;
									    t = findfast(&v[d],buflen,(unsigned char*)&value,4);
								    }
 							    }
							    break;
				    }
				    delete [] regionData;
			    }
            }
		    m_LastScanIsFullDump = false;        
        }
	} else {
		//This is not the first scan
		CFile f2;
		f2.Open( tempFilename2, CFile::modeRead );
        if( m_LastScanIsFullDump ) {
            //last scan was full dump
            PVOID rgnbase;
            SIZE_T rgnsize;
            while( f2.Read( &rgnbase, sizeof(PVOID) ) == sizeof(PVOID) )
            {
                bool err = true;
                if( f2.Read( &rgnsize, sizeof(SIZE_T) ) == sizeof(SIZE_T) ) {
                    char * oldmem = new char[rgnsize];
                    if( f2.Read( oldmem, (UINT)rgnsize ) == rgnsize ) {
                        char * curmem = new char[rgnsize];
                        SIZE_T readed;
                        bool doit = false;
                        if( ReadProcessMemory( m_ProcessHandle, reinterpret_cast<LPCVOID>( (unsigned long)rgnbase ), curmem, rgnsize, &readed ) ) {
                            doit = true;
                            address.type = 1;
                        }
                        if( mode == SM_REFRESH ) {
                            if( !doit ) {
                                address.type = -1;
                                doit = true;                          
                            }
                        }
			            if( doit ) 
			            {
                            char * oldp = oldmem;
                            char * curp = ( address.type == -1 ) ? oldmem : curmem;
                            matchCount += MemoryConditionalSearch<DWORD>( rgnbase, rgnsize, oldp, curp, f, mode, address.type, value, size, isAligned );
                        }
                        delete [] curmem;
                        err = false;
                    }
                    delete [] oldmem;
                }
                if( err ) break;        
            }
            m_LastScanIsFullDump = false;
        }
        else 
        {
            //last scan was not full dump
		    while( f2.Read( &address, sizeof(address) ) == sizeof(address) ) 
		    {
			    DWORD d = address.value;
			    SIZE_T readed;
                bool doit = false;
                if( ReadProcessMemory( m_ProcessHandle, reinterpret_cast<LPCVOID>( (unsigned long)address.base + address.offset ), &d, searchsize, &readed ) )
                {
                    address.type = 1;
                    doit = true;
                }              
                if( mode == SM_REFRESH ) 
                {
                    if( !doit ) {
                        address.type = -1;
                        doit = true;
                    }
                }
                
			    if( doit ) 
			    {
				    if( ( mode == SM_EXACT && d == value ) ||
                        ( mode == SM_GREATER && d > address.value ) || 
                        ( mode == SM_LOWER && d < address.value ) ||
                        ( mode == SM_EQUAL && d == address.value ) ||
                        ( mode == SM_REFRESH ) )
                    {
                        address.value = d;  //keep the old value
                        if( mode != SM_REFRESH ) f.Write( &address, sizeof(address) );
                        //report the new value
					    if( m_AddResultsCB ) m_AddResultsCB( address.base, address.offset, address.value, address.type, m_AddResultsUserData );
					    matchCount++;
				    }
			    }
                else
                {
                    //OutputDebugString("Error reading memory\n");
                }
		    }
            m_LastScanIsFullDump = false;
        }
		f2.Close();      
	}
#if SUSPEND_WHILE_SEARCHING
    OutputDebugString("Resuming process...\n");
	Resume();
#endif //SUSPEND_WHILE_SEARCHING
	f.Close();
    if( mode != SM_REFRESH ) 
    {
	    if( matchCount )
	    {
		    DeleteFile( tempFilename2 );
		    MoveFile( tempFilename, tempFilename2 );
		    if( matchCount != m_CurrentMatchCount ) {
			    //add this stage's result to our results list
			    m_CurrentMatchCount = matchCount;
		    }
            m_ScanCount++;        
	    }
	    else
	    {
		    //value not found
		    Reset();
	    }
    }
	return matchCount;
}


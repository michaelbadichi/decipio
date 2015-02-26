#include <vector>
#include <string>
#include <list>

//////////////////////////////////////////////////////////////////////////

typedef struct {
	std::string name;
    std::string filename;
	DWORD id;
} ProcessInfo_t;

typedef std::vector<ProcessInfo_t> ProcessList_t;

//////////////////////////////////////////////////////////////////////////

void GetAvailableProcesses(ProcessList_t & dest);

//////////////////////////////////////////////////////////////////////////

class CProcess {
public:
    enum SearchMode_t {
        SM_EXACT,
        SM_GREATER,
        SM_LOWER,
        SM_EQUAL,
        SM_REFRESH
    };
	enum SearchSize_t {
		SS_DWORD = 0,
		SS_SHORT = 1,
		SS_BYTE = 2
	};
	typedef struct {
		PVOID base;
		unsigned int offset;
        DWORD value;
        int type;
	} Address_t;
	typedef void (*AddResultCallback_t)( PVOID base, unsigned int offset, DWORD value, int type, void *userData );
public:
	CProcess( unsigned int ProcessID );
	void SetProcessID( unsigned int ProcessID );
	unsigned int GetProcessID( void );
	virtual ~CProcess();

	unsigned int GetScanCount( void );
	bool Open( bool write = false );
	bool Close( void );
	void Reset( void );
	unsigned int Search( SearchMode_t mode, unsigned int value, SearchSize_t size, bool isAligned );
	void SetAddResultCallback( AddResultCallback_t callbackFn, void *userData );
	bool GetValue( PVOID base, unsigned int offset, SearchSize_t size, unsigned int & value );
	bool SetValue( PVOID base, unsigned int offset, SearchSize_t size, unsigned int value );

protected:
	bool Suspend( void );
	bool Resume( void );
	bool RescanRegions( void );
private:
	unsigned int m_ScanCount;
    bool m_LastScanIsFullDump;
	typedef struct RegionInfo_t {
		PVOID base_address;
		SIZE_T size;
	} RegionInfo_t;
	typedef std::list<RegionInfo_t> RegionsMap_t;
	RegionsMap_t m_Regions;
	char * GetRegionMemory( RegionInfo_t &ri );
	bool m_IsOpened;
	bool m_IsSuspended;
	unsigned int m_ProcessID;
	HANDLE m_ProcessHandle;
	AddResultCallback_t m_AddResultsCB;
	void * m_AddResultsUserData;
	unsigned int m_CurrentMatchCount;

    template <class Type_V> int MemoryExactSearch( void * mem, SIZE_T size, Type_V value, CFile & f, Address_t & address, bool isAligned );
    template <class Type_V> int MemoryConditionalSearch( PVOID rgnbase, SIZE_T rgnsize, char * oldp, char * curp, CFile & f, SearchMode_t mode, int type, Type_V value, SearchSize_t size, bool isAligned );

};
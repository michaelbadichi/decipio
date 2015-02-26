#include "stdafx.h"
#include "MiniHttpDaemon.h"
#include <string>

class DecipioWebInterface
{
    bool m_IsRunning;
    int m_ListeningPort;
    MiniHttpDaemon m_Daemon;
public:
    DecipioWebInterface( int listeningPort );
    virtual ~DecipioWebInterface();

    bool Start();

    void Stop();
};


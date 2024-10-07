///////////////////////////////////////////////////////////////////////////////
// JP UPDATE
// Name:        tests/net/ipc.cpp
// Purpose:     IPC classes unit tests
// Author:      Vadim Zeitlin
// Copyright:   (c) 2008 Vadim Zeitlin
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx/wx.h".
// and "wx/cppunit.h"
#include "testprec.h"

#include <iostream>  // JPDELETE  for testlogging

#if wxUSE_THREADS

// for all others, include the necessary headers
#ifndef WX_PRECOMP
    #include "wx/app.h"
#endif

#define wxUSE_SOCKETS_FOR_IPC 1
#define wxUSE_DDE_FOR_IPC     0

#include <wx/ipc.h>
#include <wx/thread.h>

#define MAX_MSG_BUFFERS 2048

namespace
{

const char *IPC_TEST_PORT = "4242";
const char *IPC_TEST_TOPIC = "IPC TEST";

} // anonymous namespace

// ----------------------------------------------------------------------------
// test connection class used by IPCTestServer
// ----------------------------------------------------------------------------

class IPCTestConnection : public wxConnection
{
public:
    IPCTestConnection()
    {
        for (int i = 0; i < MAX_MSG_BUFFERS; i++)
            m_bufferList[i] = nullptr;

        m_nextAvailable = 0;
    }

    ~IPCTestConnection()
    {
        for (int i = 0; i < MAX_MSG_BUFFERS; i++)
            if (m_bufferList[i])
                delete[] m_bufferList[i];
    }

    virtual bool OnExec(const wxString& topic, const wxString& data)
    {
        if ( topic != IPC_TEST_TOPIC )
            return false;

        return data == "Date";
    }

    virtual const void* OnRequest(const wxString& topic,
                                  const wxString& item,
                                  size_t* size,
                                  wxIPCFormat format);
private:

    char* GetBufPtr(size_t size)
    {
        wxCRIT_SECT_LOCKER(lock, m_cs_assign_buffer);

        if (m_bufferList[m_nextAvailable] != nullptr)
        {
            // Free the memory from the last use
            delete[] m_bufferList[m_nextAvailable];
        }

        char* ptr = new char[size];

        m_bufferList[m_nextAvailable] = ptr;
        m_nextAvailable = (m_nextAvailable + 1) % MAX_MSG_BUFFERS;

        return ptr;
    }

    wxCRIT_SECT_DECLARE_MEMBER(m_cs_assign_buffer);

    char* m_bufferList[MAX_MSG_BUFFERS];
    int m_nextAvailable;


    wxDECLARE_NO_COPY_CLASS(IPCTestConnection);
};

const void* IPCTestConnection::OnRequest(const wxString& topic,
                                         const wxString& item,
                                         size_t* size,
                                         wxIPCFormat format)
{
    *size = 0;

    if ( topic != IPC_TEST_TOPIC )
        return nullptr;

    wxString s;

    if (item == "ping")
    {
        if (format != wxIPC_PRIVATE)
            return nullptr;

        s = "pong";
    }

    *size = strlen(s.mb_str()) + 1;
    char* ret = GetBufPtr(*size);
    strncpy(ret, s.mb_str(), *size);
    // wxLogError(s);
    return ret;
}



// ----------------------------------------------------------------------------
// test server class
// ----------------------------------------------------------------------------

class IPCTestServer : public wxServer
{
public:
    IPCTestServer()
    {
        m_conn = nullptr;
    }

    virtual ~IPCTestServer()
    {
        if (m_conn)
            delete m_conn;
    }

    virtual wxConnectionBase *OnAcceptConnection(const wxString& topic)
    {
        if ( topic != IPC_TEST_TOPIC )
            return nullptr;

        m_conn = new IPCTestConnection;
        return m_conn;
    }

private:
    IPCTestConnection *m_conn;

    wxDECLARE_NO_COPY_CLASS(IPCTestServer);
};


// Define a new application
class MyApp : public wxApp
{
public:
    virtual bool OnInit() override;
    virtual int OnExit() override;

protected:
    IPCTestServer m_server;
};

wxDECLARE_APP(MyApp);

// ============================================================================
// implementation
// ============================================================================

wxIMPLEMENT_APP_CONSOLE(MyApp);

// ----------------------------------------------------------------------------
// MyApp
// ----------------------------------------------------------------------------

bool MyApp::OnInit()
{
    if ( !wxApp::OnInit() )
        return false;

#if wxUSE_SOCKETS_FOR_IPC
        // we must call this from the main thread
        wxSocketBase::Initialize();
#endif // wxUSE_SOCKETS_FOR_IPC

    if ( !m_server.Create(IPC_TEST_PORT) )
    {
        std::cout << "Failed to create server. Make sure nothing is running on port " << IPC_TEST_PORT << std::flush;
        return false;
    }
    return true;
}

int MyApp::OnExit()
{

#if wxUSE_SOCKETS_FOR_IPC
    wxSocketBase::Shutdown();
#endif // wxUSE_SOCKETS_FOR_IPC

    return wxApp::OnExit();
}


#endif // wxUSE_THREADS

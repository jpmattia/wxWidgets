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

// #define wxUSE_SOCKETS_FOR_IPC (!wxUSE_DDE_FOR_IPC)

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
    IPCTestConnection() { }

    virtual bool OnExec(const wxString& topic, const wxString& data)
    {
        if ( topic != IPC_TEST_TOPIC )
            return false;

        return data == "Date";
    }

    virtual bool OnRequest(const wxString& topic, const wxString& data)
    {
        if ( topic != IPC_TEST_TOPIC )
            return false;

        return data == "Date";
    }


private:
    wxDECLARE_NO_COPY_CLASS(IPCTestConnection);
};

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
        wxTheApp->ExitMainLoop();
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

    return m_server.Create(IPC_TEST_PORT);
}

int MyApp::OnExit()
{

#if wxUSE_SOCKETS_FOR_IPC
    wxSocketBase::Shutdown();
#endif // wxUSE_SOCKETS_FOR_IPC

    return wxApp::OnExit();
}


#endif // wxUSE_THREADS

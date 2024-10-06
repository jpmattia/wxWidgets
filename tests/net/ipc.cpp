///////////////////////////////////////////////////////////////////////////////
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

// FIXME: this tests currently sometimes hangs in Connect() for unknown reason
//        and this prevents buildbot builds from working so disabling it, but
//        the real problem needs to be fixed, of course
#if 1

// this test needs threads as it runs the test server in a secondary thread
#if wxUSE_THREADS

// for all others, include the necessary headers
#ifndef WX_PRECOMP
    #include "wx/app.h"
#endif

#define wxUSE_SOCKETS_FOR_IPC 1
#define wxUSE_DDE_FOR_IPC     0

#include "wx/ipc.h"
#include "wx/thread.h"
#include "wx/process.h"

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

private:
    wxDECLARE_NO_COPY_CLASS(IPCTestConnection);
};

// ----------------------------------------------------------------------------
// event dispatching thread class
// ----------------------------------------------------------------------------

class EventThread : public wxThread
{
public:
    EventThread()
        : wxThread(wxTHREAD_JOINABLE)
    {
#if wxUSE_SOCKETS_FOR_IPC
        // we must call this from the main thread
        wxSocketBase::Initialize();
#endif // wxUSE_SOCKETS_FOR_IPC

        Create();
        Run();
    }

protected:
    virtual void *Entry();

    wxDECLARE_NO_COPY_CLASS(EventThread);
};

// ----------------------------------------------------------------------------
// test client class
// ----------------------------------------------------------------------------

class IPCTestClient : public wxClient
{
public:
    IPCTestClient()
    {
        m_conn = nullptr;
    }

    virtual ~IPCTestClient()
    {
        Disconnect();
    }

    bool
    Connect(const wxString& host, const wxString& service, const wxString& topic)
    {
        m_conn = MakeConnection(host, service, topic);

        return m_conn != nullptr;
    }

    void Disconnect()
    {
        if ( m_conn )
        {
            delete m_conn;
            m_conn = nullptr;
        }
    }

    wxConnectionBase& GetConn() const
    {
        CHECK( m_conn );

        return *m_conn;
    }

private:
    wxConnectionBase *m_conn;

    wxDECLARE_NO_COPY_CLASS(IPCTestClient);
};

static IPCTestClient *gs_client = nullptr;


// ----------------------------------------------------------------------------
// IPCServerProcess
// ----------------------------------------------------------------------------

//
class IPCServerProcess : public wxProcess
{
public:
    IPCServerProcess()
        {
            Redirect();
            m_finished = false;
        }

    virtual void OnTerminate(int pid, int status) override
    {
        wxString output =
            wxString::Format("Process %u terminated with exit code %d.",
                             pid, status);
        std::cout << output;
        m_finished = true;
    }

    bool m_finished;
};



// ----------------------------------------------------------------------------
// EventThread implementation
// ----------------------------------------------------------------------------

void *EventThread::Entry()
{
//    gs_server = new IPCTestServer;

    wxTheApp->MainLoop();

//     delete gs_server;
    return nullptr;
}


// ----------------------------------------------------------------------------
// the test code itself
// ----------------------------------------------------------------------------

TEST_CASE("JP", "[TEST_IPC][.]")
{
    IPCServerProcess * const process = new IPCServerProcess;
    wxString cmd = "echo hi; exit", out;

    long wxExecuteReturnCode = wxExecute(cmd, wxEXEC_ASYNC, process);

    if ( !wxExecuteReturnCode )
    {
        out = wxString::Format("Execution of '%s' failed.");
        delete process;
    }
    else
    {
        out = wxString::Format("Process %ld (%s) launched.\n",
                               wxExecuteReturnCode, cmd);
    }

    std::cout << out;
    CHECK( wxExecuteReturnCode != 0 );
    wxSleep(2);

    if (process->m_finished)
        std::cout << "Process finished before end of test";
    else
        std::cout << "Process did not finish before end of test";


    std::cout << '\n';
}


TEST_CASE("TEST_IPC_Connect", "[TEST_IPC][Connect][WrongPort]")
{
    gs_client = new IPCTestClient;

    // connecting to the wrong port should fail
    CHECK( !gs_client->Connect("localhost", "2424", IPC_TEST_TOPIC) );

    CHECK( !gs_client->Connect("localhost", IPC_TEST_PORT, "VCP GRFG") );

    // connecting to the right port on the right topic should succeed
    CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    delete gs_client;
}

TEST_CASE("TEST_IPC_Execute", "[TEST_IPC][Execute]")
{
    gs_client = new IPCTestClient;

    CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    wxConnectionBase& conn = gs_client->GetConn();

    const wxString s("Date");
    CHECK( conn.Execute(s) );
    CHECK( conn.Execute(s.mb_str(), s.length() + 1) );

    char bytes[] = { 1, 2, 3 };
    CHECK( conn.Execute(bytes, WXSIZEOF(bytes)) );

    delete gs_client;

}

TEST_CASE("TEST_IPC_Disconnect", "[TEST_IPC][Disconnect]")
{
    if ( gs_client )
    {
        gs_client->Disconnect();
        delete gs_client;
        gs_client = nullptr;
    }
}


// JPDELETE
TEST_CASE("testme", "[TEST_IPC][testme]")
 {
    std::cout << 'A';
    SECTION("A") {
        std::cout << 'A';
    }
    SECTION("B") {
        std::cout << 'B';
    }
    SECTION("C") {
        std::cout << 'C';
    }
    std::cout << " JP";
    std::cout << '\n';
}


#endif // wxUSE_THREADS

#endif // !__WINDOWS__

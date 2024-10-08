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

// this test needs threads as it runs the test server in a secondary thread
#if wxUSE_THREADS

// for all others, include the necessary headers
#ifndef WX_PRECOMP
    #include "wx/app.h"
#endif

#define wxUSE_SOCKETS_FOR_IPC 1
#define wxUSE_DDE_FOR_IPC     0

#include <wx/ipc.h>
#include <wx/thread.h>
#include <wx/process.h>
#include <wx/timer.h>
#include <wx/txtstrm.h>
#include <wx/sstream.h>

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

// ----------------------------------------------------------------------------
// IPCServerProcess
// ----------------------------------------------------------------------------

// The server is run in an external process, which is necessary when TCP
// sockets are in use.
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
            wxString::Format("Process %u terminated with exit code %d.\n",
                             pid, status);
        std::cout << output << std::flush;
        m_finished = true;
    }

    void PrintStreams()
    {
        wxString textout="stdout:\n", texterr="stderr:\n";

        if ( !GetInputStream() )
        {
            textout += "closed.";
        }
        else
        {
            wxStringOutputStream procOutput;
            GetInputStream()->Read(procOutput);
            textout += procOutput.GetString();
        }

        if ( !GetErrorStream() )
            texterr += "closed.";
        else
        {
            wxStringOutputStream procError;
            GetErrorStream()->Read(procError);
            textout += procError.GetString();
        }

        std::cout << '\n' << textout << '\n' << texterr << '\n' << std::flush;
    }

    bool m_finished;
};

// ----------------------------------------------------------------------------
// event dispatching thread class
// ----------------------------------------------------------------------------

class EventThread : public wxTimer
{
public:
    EventThread() {}

    ~EventThread() {}

    long DoExecute()
    {
        // Trigger the timer to go off inside the event loop
        // so that we can run wxExecute there.
        StartOnce(10);

        // Run the event loop.
        wxEventLoop loop;
        loop.Run();

        return m_pid;
    }

    void Notify() override
    {
        std::cout << "async::Notify() start\n" << std::flush;

        wxString command = "test_sckipc_server";

        // Run wxExecute inside the event loop.
        m_pid = wxExecute(command, wxEXEC_ASYNC, &m_process);

        REQUIRE( m_pid != 0);

        std::cout << "async::Notify() ending loop...\n" << std::flush;

        wxEventLoop::GetActive()->Exit();

        std::cout << "async::Notify() done\n" << std::flush;
    }

    bool EndProcess()
    {
        // return m_pid == 0
        //     || m_process.m_finished
        //     || 0 == wxKill(m_pid, wxSIGTERM)
        //     || wxKill(m_pid, wxSIGKILL) == 0;

         return m_pid == 0
             || 0 == wxKill(m_pid, wxSIGTERM)
             || wxKill(m_pid, wxSIGKILL) == 0;
    }

    long m_pid;
    IPCServerProcess m_process;

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
        REQUIRE( m_conn );

        return *m_conn;
    }

private:
    wxConnectionBase *m_conn;

    wxDECLARE_NO_COPY_CLASS(IPCTestClient);
};

static IPCTestClient *gs_client = nullptr;


// ----------------------------------------------------------------------------
// the test code itself
// ----------------------------------------------------------------------------

TEST_CASE("JP", "[TEST_IPC][.]")
{

#if wxUSE_SOCKETS_FOR_IPC
        // we must call this from the main thread
        wxSocketBase::Initialize();
#endif // wxUSE_SOCKETS_FOR_IPC

    EventThread event_thread;

    long pid = event_thread.DoExecute();

    // Allow time for the server to bind the port
    wxMilliSleep(300);

    REQUIRE( pid != 0);

    gs_client = new IPCTestClient;

    SECTION("Connect")
    {
        // connecting to the wrong port should fail
        CHECK( !gs_client->Connect("localhost", "2424", IPC_TEST_TOPIC) );

        CHECK( !gs_client->Connect("localhost", IPC_TEST_PORT, "VCP GRFG") );

        // connecting to the right port on the right topic should succeed
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        gs_client->Disconnect();
    }


    SECTION("Execute")
    {
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        wxConnectionBase& conn = gs_client->GetConn();

        const wxString s("Date");
        CHECK( conn.Execute(s) );
        CHECK( conn.Execute(s.mb_str(), s.length() + 1) );

        char bytes[] = { 1, 2, 3 };
        CHECK( conn.Execute(bytes, WXSIZEOF(bytes)) );
    }

    SECTION("SimpleRequest")
    {
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        wxConnectionBase& conn = gs_client->GetConn();

        const wxString s("ping");
        size_t size=0;
        const char* data = (char*) conn.Request(s, &size, wxIPC_PRIVATE);

        CHECK( wxString(data) == "pong"  );
    }

    // ensure we are connected, and then send the shutdown signal to the sckipc server.
    CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    wxConnectionBase& conn = gs_client->GetConn();
    const wxString s("shutdown");
    CHECK( conn.Execute(s) );



    // event_thread.m_process.PrintStreams();

    // Make sure the server process exits.
    CHECK( event_thread.EndProcess() );

    // Allow time for the server to unbind the port
    wxMilliSleep(50);

    gs_client->Disconnect();
    delete gs_client;

#if wxUSE_SOCKETS_FOR_IPC
        wxSocketBase::Shutdown();
#endif // wxUSE_SOCKETS_FOR_IPC

    std::cout << '\n' << std::flush;
}

#endif // wxUSE_THREADS

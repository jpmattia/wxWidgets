///////////////////////////////////////////////////////////////////////////////
// Name:        tests/net/ipc.cpp
// Purpose:     IPC classes unit tests
// Author:      Vadim Zeitlin
// Copyright:   (c) 2008 Vadim Zeitlin
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// single Poke
// single Advise
// 100 Advise


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
#define MESSAGE_ITERATIONS 100
#define MESSAGE_ITERATIONS_STRING  wxString::Format("%d",MESSAGE_ITERATIONS)

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
// ExecAsyncWrapper starts a process with wxExecute, which must be done in the
// main thread.
// ----------------------------------------------------------------------------

class ExecAsyncWrapper : public wxTimer
{
public:
    ExecAsyncWrapper() {}

    ~ExecAsyncWrapper() {}

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
        wxString command = "test_sckipc_server";

        // Run wxExecute inside the event loop.
        m_pid = wxExecute(command, wxEXEC_ASYNC, &m_process);

        REQUIRE( m_pid != 0);

        wxEventLoop::GetActive()->Exit();
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

    wxDECLARE_NO_COPY_CLASS(ExecAsyncWrapper);
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
            m_conn->Disconnect();
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
// MultiRequestThread
// ----------------------------------------------------------------------------

// Send repeated Request()'s, each with a serial number to verify that
// multiple repeated messages are sent and received correctly and in order.

class MultiRequestThread : public wxThread
{
public:

    // label: A header to be put on the string sent to the server.
    // It should be of the form "MultiRequest thread N", where N
    // is "1", "2", or "3".
    MultiRequestThread(const wxString& label )
        : wxThread(wxTHREAD_JOINABLE)
    {
        m_label = label;

        Create();
    }

protected:
    virtual void *Entry()
    {
        wxConnectionBase& conn = gs_client->GetConn();

        for (size_t n=1; n < MESSAGE_ITERATIONS + 1; n++)
        {
            wxString s = m_label + wxString::Format(" %zu", n);
            size_t size=0;
            const char* data = (char*) conn.Request(s, &size, wxIPC_PRIVATE);

            CHECK( wxString(data) == "OK: " + s );
        }

        return nullptr;
    }

    wxString m_label;

    wxDECLARE_NO_COPY_CLASS(MultiRequestThread);
};

// ----------------------------------------------------------------------------
// the test code itself
// ----------------------------------------------------------------------------

TEST_CASE("JP", "[TEST_IPC][.]")
{

#if wxUSE_SOCKETS_FOR_IPC
        // we must call this from the main thread
        wxSocketBase::Initialize();
#endif // wxUSE_SOCKETS_FOR_IPC

    ExecAsyncWrapper exec_wrapper;

    long pid = exec_wrapper.DoExecute();

    // Allow time for the server to bind the port
    wxMilliSleep(300);

    REQUIRE( pid != 0);

    gs_client = new IPCTestClient;


    SECTION("Connect")
    {
        // connecting to the wrong port should fail
        CHECK( !gs_client->Connect("localhost", "2424", IPC_TEST_TOPIC) );

        CHECK( !gs_client->Connect("localhost", IPC_TEST_PORT, "VCP GRFG") );

        // Connecting to the right port on the right topic should succeed.
        // If Connect() doesn't work, then nothing below works
        REQUIRE( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );
    }


    SECTION("SimpleRequest")
    {
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        wxConnectionBase& conn = gs_client->GetConn();

        const wxString s("ping");
        size_t size=0;
        const char* data = (char*) conn.Request(s, &size, wxIPC_PRIVATE);

        // Make sure that Request() works, because we use it to probe the
        // state of the server for the remaining tests.
        REQUIRE( wxString(data) == "pong"  );
    }

    SECTION("Execute")
    {
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        wxConnectionBase& conn = gs_client->GetConn();

        wxString s("Date");
        CHECK( conn.Execute(s) );

        // Get the last execute from the server side.
        size_t size=0;
        const wxString last_execute_query("last_execute");

        char* data = (char*) conn.Request(last_execute_query, &size, wxIPC_PRIVATE);
        CHECK( wxString(data) == s );


        s = "another execution command!";
        CHECK( conn.Execute(s.mb_str(), s.length() + 1) );

        data = (char*) conn.Request(last_execute_query, &size, wxIPC_PRIVATE);
        CHECK( wxString(data) == s );
    }

    SECTION("SingleThreadOfRequests")
    {
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        MultiRequestThread thread1("MultiRequest thread 1");
        thread1.Run();
        thread1.Wait();

        // Make sure the server got all the requests in the correct order.
        wxConnectionBase& conn = gs_client->GetConn();

        size_t size=0;
        wxString query("get_thread1_request_counter");

        char* data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);
        CHECK( wxString(data) == MESSAGE_ITERATIONS_STRING );

        size=0;
        query = "get_error_string";
        data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);

        INFO( wxString(data) );
        CHECK( wxString(data).IsEmpty() );
    }

    SECTION("MultipleThreadsOfRequests")
    {
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

        MultiRequestThread thread1("MultiRequest thread 1");
        MultiRequestThread thread2("MultiRequest thread 2");
        MultiRequestThread thread3("MultiRequest thread 3");

        thread1.Run();
        thread2.Run();
        thread3.Run();

        thread1.Wait();
        thread2.Wait();
        thread3.Wait();

        // Make sure the server got all the requests in the correct order.
        wxConnectionBase& conn = gs_client->GetConn();

        size_t size=0;
        wxString query = "get_thread1_request_counter";

        char* data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);
        CHECK( wxString(data) == MESSAGE_ITERATIONS_STRING );

        size=0;
        query = "get_thread2_request_counter";

        data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);
        CHECK( wxString(data) == MESSAGE_ITERATIONS_STRING );

        size=0;
        query = "get_thread3_request_counter";

        data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);
        CHECK( wxString(data) == MESSAGE_ITERATIONS_STRING );

        size=0;
        query = "get_error_string";
        data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);

        INFO( wxString(data) );
        CHECK( wxString(data).IsEmpty() );
    }



    wxConnectionBase& conn = gs_client->GetConn();
    const wxString s("shutdown");
    CHECK( conn.Execute(s) );

    // Make sure the server process exits.
    CHECK( exec_wrapper.EndProcess() );

    // Allow time for the server to unbind the port
    wxMilliSleep(100);

    gs_client->Disconnect();
    delete gs_client;

#if wxUSE_SOCKETS_FOR_IPC
        wxSocketBase::Shutdown();
#endif // wxUSE_SOCKETS_FOR_IPC

    std::cout << '\n' << std::flush;
}

#endif // wxUSE_THREADS

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
#include <wx/timer.h>
#include <wx/txtstrm.h>

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

        wxEventLoop::GetActive()->ScheduleExit();


        wxString textout="stdout:\n", texterr="stderr:\n";

        wxTextInputStream strm_stdout( *GetInputStream() );
        while (!strm_stdout.GetInputStream().Eof())
        {
            textout += strm_stdout.ReadLine() + "\n";
        }

        wxTextInputStream strm_stderr( *GetErrorStream() );
        while (!strm_stderr.GetInputStream().Eof())
        {
            texterr += strm_stderr.ReadLine() + "\n";
        }

        std::cout << '\n' << textout << '\n' << texterr << '\n' << std::flush;
    }

    bool m_finished;
};


// This class is used as a helper in order to run wxExecute(ASYNC)
// inside of an event loop.

class AsyncInEventLoop : public wxTimer
{
public:
    AsyncInEventLoop()
    {
        m_loop = nullptr;
        m_pid = 0;
    }

    ~AsyncInEventLoop()
    {
        if (m_loop)
            wxEventLoop::GetActive()->Exit();
    }

    long DoExecute(const wxString& command,
                   IPCServerProcess* callback = nullptr)
    {
        m_command = command;
        m_callback = callback;

        wxEventLoop* m_loop = new wxEventLoop;

        // Trigger the timer to go off inside the event loop
        // so that we can run wxExecute there.
        StartOnce(10);

        // Run the event loop.
        m_loop->Run();

        return m_pid;
    }

    void Notify() override
    {
        // Run wxExecute inside the event loop.

        std::cout << "async::Notify()\n" << std::flush;

        m_pid = wxExecute(m_command, wxEXEC_ASYNC, m_callback);

        std::cout << "async::Notify() done\n" << std::flush;
    }

    bool EndProcess()
    {
        return m_pid == 0
            || ( m_callback && m_callback->m_finished )
            || 0 == wxKill(m_pid, wxSIGTERM)
            || wxKill(m_pid, wxSIGKILL) == 0;
    }


private:
    wxString m_command;
    IPCServerProcess* m_callback;
    wxEventLoop* m_loop ;
    long m_pid;
};

// ----------------------------------------------------------------------------
// the test code itself
// ----------------------------------------------------------------------------

TEST_CASE("JP", "[TEST_IPC][.]")
{
    IPCServerProcess * const process = new IPCServerProcess;
    wxString cmd = "test_sckipc_server", out;

    AsyncInEventLoop exec_wrapper;
    long pid = exec_wrapper.DoExecute(cmd, process);

    REQUIRE( pid != 0);

    wxMilliSleep(50); // let the process start

    gs_client = new IPCTestClient;

    SECTION("Connect")
    {
        // connecting to the wrong port should fail
        CHECK( !gs_client->Connect("localhost", "2424", IPC_TEST_TOPIC) );

        CHECK( !gs_client->Connect("localhost", IPC_TEST_PORT, "VCP GRFG") );

        // connecting to the right port on the right topic should succeed
        CHECK( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );
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

    gs_client->Disconnect();
    delete gs_client;

    CHECK( exec_wrapper.EndProcess() );

    std::cout << '\n' << std::flush;
}

#endif // wxUSE_THREADS

#endif // !__WINDOWS__

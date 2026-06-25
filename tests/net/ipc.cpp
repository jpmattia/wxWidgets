///////////////////////////////////////////////////////////////////////////////
// Name:        tests/net/ipc.cpp
// Purpose:     IPC classes unit tests
// Author:      Vadim Zeitlin
// Copyright:   (c) 2008 Vadim Zeitlin
// Modified by: JP Mattia, 2024
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

#include "testprec.h"

// This test requires wxUSE_THREADS==1 since it runs the test server
// concurrently with the client.
//
// This test is deliberately excluded from wxMSW monolithic builds,
// where wxIPC itself is broken by wxWidgets#24909. In that issue, we
// noted that in a monolithic build, a GUI-only component inserts
// itself into the wxAppConsole server. The bug can be demonstrated by
// compile the IPC sample in a monolithic build, where it will be seen
// that the base server sample stops receiving data. 
//
// Running this test there fails for reasons unrelated to what it is
// meant to check, so we skip it rather than report a spurious
// failure. The bug is wxMSW-specific -- a GTK monolithic build runs
// these tests cleanly -- so the guard keys off wxMONOLITHIC, which is
// defined (to 1) only in wxMSW monolithic builds. (For the guard to
// engage, the MSVC monolithic test build must define wxMONOLITHIC=1;
// it currently selects the monolithic library via the makefile's
// $(MONOLITHIC) but does not pass it to the compiler as a -D.)
#if wxUSE_THREADS && (!defined(wxMONOLITHIC) || wxMONOLITHIC == 0)

#ifndef WX_PRECOMP
    #include "wx/app.h"
#endif // WX_PRECOMP

#include "ipc_setup_test.h"
#include "ipc_test_server.h"

#include <wx/ipc.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <wx/evtloop.h>
#include <wx/stopwatch.h>

// forward decl
class IPCTestClient;

// The IPC tests use a single test binary: the server is started by re-executing
// the same test program with WX_IPC_TEST_SERVER set (see ipc_test_server.cpp).
// The client runs in the main Catch2 process. Catch2 cannot run checks in the
// server process, so the client queries the server for state and verifies it
// here.

// When g_show_message_timing is set to true, Advise() and RequestReply()
// messages will be printed when they arrive. This shows how the IPC messages
// arrive and whether they interleave.
bool g_show_message_timing = false;

// Output for g_show_message_timing uses std::cout, so we can get a sense of the
// raw arrival times.
#include <atomic>
#include <iostream>
#include <memory>

// Test connection class used by the client.
class IPCTestConnection : public wxConnection
{
public:
    IPCTestConnection(IPCTestClient* client)
    {
        m_client = client;
        ResetThreadTrackers();
    }

    virtual ~IPCTestConnection() {}

    virtual bool OnExec(const wxString& topic, const wxString& data) override
    {
        if ( topic != IPC_TEST_TOPIC )
            return false;

        return data == "Date";
    }

    virtual bool OnAdvise(const wxString& topic,
                          const wxString& item,
                          const void* data,
                          size_t size,
                          wxIPCFormat format) override
    {
        if ( topic != IPC_TEST_TOPIC )
            return false;

        CHECK( format == wxIPC_TEXT );

        wxString s(static_cast<const char*>(data), size);

        if (item == "SimpleAdvise test")
        {
            if ( s == "OK SimpleAdvise" )
                m_advise_complete = true;
            else
                m_general_error << "SimpleAdvise: unexpected payload: " << s << '\n';
        }

        else if (item == "MultiAdvise test" ||
                 item == "MultiAdvise MultiThread test" ||
                 item == "MultiAdvise MultiThread test with simultaneous Requests")
        {
            HandleThreadAdviseCounting(s);

            if (m_thread1_advise_lastval == MESSAGE_ITERATIONS &&
                m_thread2_advise_lastval == MESSAGE_ITERATIONS &&
                m_thread3_advise_lastval == MESSAGE_ITERATIONS)
            {
                m_advise_complete = true;
            }
        }

        else
        {
            m_general_error << "Unknown Advise item: " << item << wxString('\n');
        }

        return true;
    }


    virtual bool OnDisconnect() override;

private:

    void ResetThreadTrackers()
    {
        m_general_error = "";

        m_advise_complete = false;

        m_thread1_advise_lastval = m_thread2_advise_lastval =
            m_thread3_advise_lastval = 0;
    }

    void HandleThreadAdviseCounting(const wxString& advise_string);

    wxCRIT_SECT_DECLARE_MEMBER(m_cs_assign_buffer);

    IPCTestClient* m_client;

public:
    bool  m_advise_complete;

    int m_thread1_advise_lastval;
    int m_thread2_advise_lastval;
    int m_thread3_advise_lastval;

    wxString m_general_error;

    wxDECLARE_NO_COPY_CLASS(IPCTestConnection);
};

// Helper for the MultiAdvise thread tests. Repeated Advise's of the form
// "MultiAdvise thread <thread_number N> <serial_number>" are received during
// the test. Track the serial number in the appropriate
// m_threadN_advise_lastval member variables for CHECKing at the end of the
// test.
void IPCTestConnection::HandleThreadAdviseCounting(const wxString& advise_string)
{
    wxCRIT_SECT_LOCKER(lock, m_cs_assign_buffer);

    wxString info;
    advise_string.StartsWith("MultiAdvise thread", &info);

    int thread_number = wxAtoi(info.Left(2));
    int counter_value = wxAtoi(info.Mid(3));
    int lastval = INT_MIN; // default to causing an error below

    bool err = false;
    wxString err_string;

    if ( g_show_message_timing )
        std::cout << advise_string << '\n' << std::flush;


    switch (thread_number)
    {
    case 0:
        err_string =
            "Error: MultiAdvise thread number could not be converted.\n";
        err = true;
        break;

    case 1:
        lastval = m_thread1_advise_lastval;
        m_thread1_advise_lastval = counter_value;
        break;

    case 2:
        lastval = m_thread2_advise_lastval;
        m_thread2_advise_lastval = counter_value;
        break;

    case 3:
        lastval = m_thread3_advise_lastval;
        m_thread3_advise_lastval = counter_value;
        break;

    default:
        err_string =
            "Error: MultiAdvise thread number must be 1, 2, or3.\n";
        err = true;
    }

    if (lastval !=  counter_value -1)
    {
        // Concatenate to any other error:
        err_string +=
            "Error: Misordered count in thread " +
            wxString::Format("%d - expected %d, received %d\n",
                             thread_number, lastval + 1, counter_value);
        err = true;
    }

    if (err)
    {
        m_general_error += err_string;
    }
}

// The actual client is pretty thin, most of the work is done in the
// connection class.
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
        m_conn = (IPCTestConnection*) MakeConnection(host, service, topic);

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

    wxConnectionBase* OnMakeConnection() override
    {
        return new IPCTestConnection(this);
    }

    IPCTestConnection& GetConn() const
    {
        REQUIRE( m_conn );

        return *m_conn;
    }

    IPCTestConnection *m_conn;

    wxDECLARE_NO_COPY_CLASS(IPCTestClient);
};

static IPCTestClient *gs_client = nullptr;
static wxEventLoop *gs_clientLoop = nullptr;

static bool PumpConnect(const wxString& host,
                        const wxString& service,
                        const wxString& topic)
{
    return gs_client->Connect(host, service, topic);
}

void IPCClientDispatch(unsigned long timeoutMs)
{
    if ( !gs_clientLoop )
        return;

    // The client loop is already active for the lifetime of IPCFixture, so do
    // NOT re-activate it per call: that writes ms_activeLoop and races the
    // worker threads reading it via CallAfter() -> WakeUpIdle().

    // Run any queued CallAfter() work first: worker threads marshal their IPC
    // socket I/O to the main thread via wxTCPEventHandler::RunOnMainThread(),
    // which posts async method-call events. DispatchTimeout() only services FD
    // (socket) events, so without this the marshaled jobs would never run.
    if ( wxTheApp )
        wxTheApp->ProcessPendingEvents();

    gs_clientLoop->DispatchTimeout(timeoutMs);
}

static void PumpDispatch()
{
    IPCClientDispatch(10);
}

static void DrainPendingIPCEvents()
{
    if ( gs_clientLoop )
    {
        // No per-call activation: the client loop is already active via IPCFixture
        // (and DispatchTimeout()/Pending() act on the loop object directly).
        for ( int i = 0; i < 100; ++i )
        {
            if ( !gs_clientLoop->Pending() )
                break;

            gs_clientLoop->DispatchTimeout(10);
        }
    }

    if ( wxTheApp )
    {
        for ( int i = 0; i < 100; ++i )
        {
            if ( !wxTheApp->Pending() )
                break;

            wxTheApp->ProcessPendingEvents();
        }
    }
}

bool IPCTestConnection::OnDisconnect()
{
    m_client->m_conn = nullptr;
    return wxConnection::OnDisconnect();
}

// MultiRequestThread sends repeated Request()'s, each with a serial number,
// so that we can verify the repeated messages are sent and received correctly
// and in order.
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

        // Resolve the connection here, on the main thread (the test constructs
        // us before calling Run()). GetConn() uses REQUIRE(), a Catch2 macro
        // that is not thread-safe, so it must not run on the worker thread in
        // Entry(). The connection is stable for our lifetime, so caching the
        // pointer is safe.
        m_conn = &gs_client->GetConn();

        Create();
    }

protected:
    virtual void *Entry() override
    {
        IPCTestConnection& conn = *m_conn;

        for (size_t n=1; n < MESSAGE_ITERATIONS + 1; n++)
        {
            wxString s = m_label + wxString::Format(" %zu", n);
            size_t size=0;
            const char* data = (const char*) conn.Request(s, &size, wxIPC_PRIVATE);

            // Catch2 macros are not thread safe, so we check explicitly and
            // store any deviation from the expected result.
            if ( wxString(data) != "OK: " + s )
            {
                m_error += "MultiRequestThread error: expected \"OK: " + s;
                m_error += ", received " + wxString(data);
                m_error += '\n';
            }

            if ( g_show_message_timing )
                std::cout << wxString(data) << '\n' << std::flush;

            // Space out the requests, to test any race conditions with
            // incoming messages, like Advise()
            wxMilliSleep(50);
        }

        return nullptr;
    }

public:
    wxString m_label;
    wxString m_error;
    IPCTestConnection* m_conn = nullptr;

    wxDECLARE_NO_COPY_CLASS(MultiRequestThread);
};

// IPCFixture starts the in-process server and the client.
class IPCFixture
{
    std::unique_ptr<wxEventLoop> m_clientLoop{new wxEventLoop};
    std::unique_ptr<wxEventLoopActivator> m_loopActivator;
    IPCServerThread m_server;

public:
    IPCFixture()
    {
#if wxUSE_SOCKETS_FOR_IPC
        wxSocketBase::Initialize();
#endif // wxUSE_SOCKETS_FOR_IPC

        DrainPendingIPCEvents();

        gs_clientLoop = m_clientLoop.get();

        // Activate the client loop once for the lifetime of this fixture so its
        // worker threads see a stable wxEventLoopBase::ms_activeLoop. Activating
        // per IPCClientDispatch() call would write ms_activeLoop and race the
        // workers' CallAfter() -> WakeUpIdle() -> GetActive() reads.
        m_loopActivator.reset(new wxEventLoopActivator(m_clientLoop.get()));

        gs_client = new IPCTestClient;

        REQUIRE( m_server.Start() );

        // Wait for the server to actually be ready to accept connections rather
        // than sleeping a fixed amount: the re-exec'd server process can take a
        // while to come up -- well over a second under sanitizers, on a loaded CI
        // runner, or as a GUI (test_gui) process doing full toolkit init -- and a
        // fixed delay races that startup (every later PumpConnect() then fails).
        // Poll with a throwaway connection until one succeeds, then drop it so
        // each test starts from a clean state. The bound is wall-clock based, not
        // iteration based: in a GUI event loop IPCClientDispatch() returns at once
        // (idle events), so a fixed iteration count would expire in a fraction of
        // a second, before a GUI server is listening.
        bool serverReady = false;
        wxStopWatch sw;
        while ( !serverReady && sw.Time() < 30000 )   // up to 30s
        {
            if ( gs_client->Connect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) )
            {
                gs_client->Disconnect();
                serverReady = true;
            }
            else
            {
                IPCClientDispatch(50);
            }
        }
        REQUIRE( serverReady );
    }

    ~IPCFixture()
    {
        if ( gs_client )
        {
            if ( !gs_client->m_conn )
                PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC);

            if ( gs_client->m_conn )
            {
                const wxString s("shutdown");
                gs_client->GetConn().Execute(s);
                wxMilliSleep(100);
            }
        }

        m_server.WaitForExit();

        DrainPendingIPCEvents();

        if ( gs_client )
            gs_client->Disconnect();

        DrainPendingIPCEvents();

        m_loopActivator.reset(); // restore the previously-active loop, once
        gs_clientLoop = nullptr;
        m_clientLoop.reset();

        delete gs_client;
        gs_client = nullptr;

#if wxUSE_SOCKETS_FOR_IPC
        wxSocketBase::Shutdown();
#endif // wxUSE_SOCKETS_FOR_IPC

        if ( g_show_message_timing )
            std::cout << "teardown complete\n" << std::flush;
    }
};

// Test the basics of Connect()
TEST_CASE_METHOD(IPCFixture,
                 "IPC::Connect", "[net][ipc][single_command]")
{
    if ( g_show_message_timing )
        std::cout << "Running test Connect\n" << std::flush;

    // connecting to the wrong port should fail
    CHECK( !PumpConnect("localhost", "2424", IPC_TEST_TOPIC) );

    // connecting with the wrong topic should fail
    CHECK( !PumpConnect("localhost", IPC_TEST_PORT, "VCP GRFG") );

    // Connecting to the right port on the right topic should succeed.
    REQUIRE( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );
}

// Test the basics of Request(): A Request() goes out and it should result in
// a reply from the server.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::SingleRequest", "[net][ipc][single_command]")
{
    if ( g_show_message_timing )
        std::cout << "Running test SingleRequest\n" << std::flush;

    // Use REQUIRE: if the connection itself failed there is no point in
    // probing the server, and it distinguishes a connect failure from a
    // Request() failure below.
    REQUIRE( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    IPCTestConnection& conn = gs_client->GetConn();

    const wxString s("ping");
    size_t size=0;
    const char* data = (char*) conn.Request( s, &size, wxIPC_PRIVATE);

    // Guard against a null return before constructing a wxString from it:
    // a failed Request() must report cleanly instead of dereferencing null
    // (this was an information-free SIGSEGV on wxMSW). size is logged to help
    // diagnose why the very first post-connect Request would fail.
    INFO( "Request() returned size=" << size );
    REQUIRE( data != nullptr );

    // Make sure that Request() works, because we use it to probe the
    // state of the server for the remaining tests.
    REQUIRE( wxString(data) == "pong"  );
}

// Test the basics of Execute(). The Execute() goes out: Note that a return
// value of "true" means simply that the message was transmitted. We follow
// the Execute with a Request() to verify that the server received the Execute
// correctly.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::SingleExecute", "[net][ipc][single_command]")
{
    if ( g_show_message_timing )
        std::cout << "Running test Execute\n" << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    IPCTestConnection& conn = gs_client->GetConn();

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

// Send multiple requests to the server. Each request has a serial number, and
// this test verifies that the replies have the correct serial in the reply
// message. After the serial requests are done, the client queries the server
// and verifies the server received the requests error-free.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::RequestThread", "[net][ipc][multi_command]")
{
    if ( g_show_message_timing )
        std::cout << "Running test Single Thread Of Requests\n" << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    MultiRequestThread thread1("MultiRequest thread 1");
    thread1.Run();
    WaitForThreadWithDispatch(thread1);

    INFO( thread1.m_error );
    CHECK( thread1.m_error.IsEmpty() );

    // Make sure the server got all the requests in the correct order.
    IPCTestConnection& conn = gs_client->GetConn();

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

// Send multiple requests to the server via three concurrent threads. Each
// reply is verified to make sure that the request corresponds to the correct
// thread and has the correctly ordered serial number. After the request
// threads are finished, the client queries the server and verifies the server
// received the requests error-free.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::RequestMultiThread", "[net][ipc][multi_thread]")
{
    if ( g_show_message_timing )
        std::cout << "Running test Requests with Multiple Threads\n"
                  << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    MultiRequestThread thread1("MultiRequest thread 1");
    MultiRequestThread thread2("MultiRequest thread 2");
    MultiRequestThread thread3("MultiRequest thread 3");

    thread1.Run();
    thread2.Run();
    thread3.Run();

    WaitForThreadWithDispatch(thread1);
    WaitForThreadWithDispatch(thread2);
    WaitForThreadWithDispatch(thread3);

    INFO( thread1.m_error );
    CHECK( thread1.m_error.IsEmpty() );

    INFO( thread2.m_error );
    CHECK( thread2.m_error.IsEmpty() );

    INFO( thread2.m_error );
    CHECK( thread2.m_error.IsEmpty() );

    // Make sure the server got all the requests in the correct order.
    IPCTestConnection& conn = gs_client->GetConn();

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

// Test the basics of Advise(). First, send a StartAdvise(), then wait for the
// server to send a single Advise(). When that is received, StopAdvise() is
// sent.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::SingleAdvise", "[net][ipc][single_command]")
{
    if ( g_show_message_timing )
        std::cout << "Running test Advise as single command\n" << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    IPCTestConnection& conn = gs_client->GetConn();
    wxString item = "SimpleAdvise test";

    CHECK( conn.StartAdvise(item) );

    // Wait a maximum of 2 seconds for completion. The bound is wall-clock based,
    // not iteration based: under a GUI event loop PumpDispatch() returns at once
    // (idle events), so a fixed iteration count would expire almost immediately,
    // before the server's advise arrives.
    wxStopWatch sw;
    while ( sw.Time() < 2000 && !conn.m_advise_complete )
    {
        PumpDispatch();
    }

    CHECK( conn.StopAdvise(item) );
    CHECK( conn.m_advise_complete );

    // Make sure the server didn't record an error
    wxString query = "get_error_string";
    size_t size=0;

    char* data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);

    INFO( wxString(data) );
    CHECK( wxString(data).IsEmpty() );
}

// Instruct the server to send a series of Advise() items to the client. Each
// Advise() is verified to make sure that the Advise() arrives in order by
// checking its serial number. Also verifies that the server encountered no
// errors during the Advise() calls.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::AdviseThread", "[net][ipc][multi_command]")
{
    if ( g_show_message_timing )
        std::cout << "Running test Single Thread Of Advise()'s\n" << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    IPCTestConnection& conn = gs_client->GetConn();
    wxString item = "MultiAdvise test";

    CHECK( conn.StartAdvise(item) );

    // Wait a maximum of 20 seconds for completion (wall-clock bounded; see the
    // note in IPC::SingleAdvise about GUI event loops and PumpDispatch()).
    wxStopWatch sw;
    while ( sw.Time() < 20000 &&
            conn.m_thread1_advise_lastval != MESSAGE_ITERATIONS )
    {
        PumpDispatch();
    }

    CHECK( conn.StopAdvise(item) );

    // Verify the results of the test.
    CHECK( conn.m_thread1_advise_lastval == MESSAGE_ITERATIONS );

    INFO( conn.m_general_error );
    CHECK( conn.m_general_error.IsEmpty() );

    // Make sure the server didn't record an error
    wxString query = "get_error_string";
    size_t size=0;

    char* data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);

    INFO( wxString(data) );
    CHECK( wxString(data).IsEmpty() );
}

// Instruct the server to send a series of Advise() items to the client via
// three concurrent threads. Each Advise() is verified to make sure that the
// Advise() arrives in order within its thread number. Also verifies that
// the server encountered no errors during the Advise() calls.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::AdviseMultiThread", "[net][ipc][multi_thread]")
{
    if ( g_show_message_timing )
        std::cout << "Running test MultipleThreadsOfMultiAdvise\n" << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );

    IPCTestConnection& conn = gs_client->GetConn();
    wxString item = "MultiAdvise MultiThread test";

    CHECK( conn.StartAdvise(item) );

    // Wait a maximum of 20 seconds for completion (wall-clock bounded; see the
    // note in IPC::SingleAdvise about GUI event loops and PumpDispatch()).
    wxStopWatch sw;
    while ( sw.Time() < 20000 )
    {
        PumpDispatch();

        if ( conn.m_thread1_advise_lastval == MESSAGE_ITERATIONS &&
             conn.m_thread2_advise_lastval == MESSAGE_ITERATIONS &&
             conn.m_thread3_advise_lastval == MESSAGE_ITERATIONS)
        {
            break;
        }
    }

    CHECK( conn.StopAdvise(item) );

    CHECK( conn.m_thread1_advise_lastval == MESSAGE_ITERATIONS );
    CHECK( conn.m_thread2_advise_lastval == MESSAGE_ITERATIONS );
    CHECK( conn.m_thread3_advise_lastval == MESSAGE_ITERATIONS );

    INFO( conn.m_general_error );
    CHECK( conn.m_general_error.IsEmpty() );

    // Make sure the server didn't record an error
    wxString query = "get_error_string";
    size_t size=0;

    char* data = (char*) conn.Request(query, &size, wxIPC_PRIVATE);

    INFO( wxString(data) );
    CHECK( wxString(data).IsEmpty() );
}

// Run three concurrent threads in the client sending Requests() to the
// server, and simultaneously run three concurrent threads in the server
// sending Advise() information to the client. Verify that all messages are
// serial and correspond to the correct thread. Lastly, verify that the server
// encountered no errors during this test.
//
// By setting g_show_message_timing to "true", the ordering of the Requests
// and Advise's can be seen. Different systems may need to change the delay
// wxMilliSleep in the client and server threads to make the interleave happen
// properly, which is a stringent test of race conditions that might be present
// in wxIPC.
// Concurrent simultaneous Advise and Request IPC stress test.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::AdviseAndRequestMultiThread", "[net][ipc][multi_thread]")
{
    if ( g_show_message_timing )
        std::cout << "Running test MultiAdvise MultiThreads test with simultaneous MultiRequests MultiThreads\n" << std::flush;

    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );
    IPCTestConnection& conn = gs_client->GetConn();

    MultiRequestThread thread1("MultiRequest thread 1");
    MultiRequestThread thread2("MultiRequest thread 2");
    MultiRequestThread thread3("MultiRequest thread 3");

    // start local and remote threads as close to simultaneously as possible
    wxString item = "MultiAdvise MultiThread test with simultaneous Requests";

    CHECK( conn.StartAdvise(item) ); // starts 3 advise threads on the server

    thread1.Run();
    thread2.Run();
    thread3.Run();

    // Phase 1: Request() threads process interleaved Advise() via FindMessage().
    // Do not PumpDispatch() here: main-thread dispatch races worker Request().
    WaitForThreadWithDispatch(thread1);
    WaitForThreadWithDispatch(thread2);
    WaitForThreadWithDispatch(thread3);

    // Phase 2: dispatch any remaining Advise() notifications on the main thread.
    // Wall-clock bounded; see the note in IPC::SingleAdvise about GUI event loops.
    wxStopWatch sw;
    while ( sw.Time() < 20000 )
    {
        PumpDispatch();

        if ( conn.m_thread1_advise_lastval == MESSAGE_ITERATIONS &&
             conn.m_thread2_advise_lastval == MESSAGE_ITERATIONS &&
             conn.m_thread3_advise_lastval == MESSAGE_ITERATIONS )
        {
            break;
        }
    }

    CHECK( conn.StopAdvise(item) );

    // Everything is done, check that all the advise messages were
    // correctly received.

    CHECK( conn.m_thread1_advise_lastval == MESSAGE_ITERATIONS );
    CHECK( conn.m_thread2_advise_lastval == MESSAGE_ITERATIONS );
    CHECK( conn.m_thread3_advise_lastval == MESSAGE_ITERATIONS );

    INFO( conn.m_general_error );
    CHECK( conn.m_general_error.IsEmpty() );


    // Also make sure all the request messages were correctly received on
    // the server side. The client side was already validated in the
    // MultiRequestThread.
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

// A deadlock cannot be detected from the main thread, because the main thread
// is precisely what gets stuck. This watchdog runs on its own thread and aborts
// the process with a diagnostic if the test does not signal completion in time,
// turning an otherwise indefinite hang into a clear, bounded failure.
class DeadlockWatchdog : public wxThread
{
public:
    explicit DeadlockWatchdog(int timeoutMs)
        : wxThread(wxTHREAD_JOINABLE), m_timeoutMs(timeoutMs) {}

    // Called by the test once it has completed normally.
    void Done() { m_done.store(true); }

protected:
    void* Entry() override
    {
        const int step = 50;
        for ( int waited = 0; waited < m_timeoutMs; waited += step )
        {
            if ( m_done.load() )
                return nullptr;
            wxMilliSleep(step);
        }

        std::cerr << "\nDEADLOCK: concurrent main-thread + worker-thread "
                     "Request() on the same connection did not complete within "
                  << m_timeoutMs << " ms.\n" << std::flush;
        abort();
    }

    const int m_timeoutMs;
    std::atomic<bool> m_done{false};

    wxDECLARE_NO_COPY_CLASS(DeadlockWatchdog);
};

// Exercises the case where a Request() is issued on the main thread while a
// worker thread is also issuing Request()s on the same connection.
//
// A main-thread Request() goes through SendAndGetReply_MainThread(), which
// blocks acquiring m_cs_process_msgs. A worker thread holds that critical
// section for the whole of its exchange, including while it marshals its socket
// write to the main thread (RunOnMainThread) and blocks waiting for the main
// thread to run it. So if the main thread blocks on m_cs_process_msgs at that
// moment, it stops pumping the event loop, the worker's marshalled write never
// runs, and both threads are stuck.
//
// The watchdog bounds the failure; the test should complete near-instantly once
// main-thread and worker-thread Request()s are properly serialized.
TEST_CASE_METHOD(IPCFixture,
                 "IPC::ConcurrentMainAndWorkerRequest", "[net][ipc][multi_command]")
{
    CHECK( PumpConnect("localhost", IPC_TEST_PORT, IPC_TEST_TOPIC) );
    IPCTestConnection& conn = gs_client->GetConn();

    // Generous timeout: the test completes in ~1-2s when healthy, so the
    // watchdog only ever fires on a *permanent* deadlock (which never recovers).
    // A large value avoids spurious aborts on slow/loaded CI runners or under
    // sanitizers, at no cost to the passing case.
    DeadlockWatchdog watchdog(30000);
    watchdog.Run();

    MultiRequestThread worker("MultiRequest thread 1");
    worker.Run();

    // Hammer the connection from the main thread while the worker does the same
    // from its thread. Pump between requests so that, absent the deadlock, the
    // worker's marshalled socket I/O can run on the main thread.
    while ( worker.IsRunning() )
    {
        size_t size = 0;
        const char* pong = (const char*) conn.Request("ping", &size, wxIPC_PRIVATE);

        CHECK( pong != nullptr );
        if ( pong )
            CHECK( wxString(pong) == "pong" );

        IPCClientDispatch(5);
    }

    worker.Wait();
    watchdog.Done();
    watchdog.Wait();

    INFO( worker.m_error );
    CHECK( worker.m_error.IsEmpty() );
}

#endif // wxUSE_THREADS && (!defined(wxMONOLITHIC) || wxMONOLITHIC == 0)

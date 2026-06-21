///////////////////////////////////////////////////////////////////////////////
// Name:        tests/net/ipc.cpp
// Purpose:     IPC classes unit tests
// Author:      Vadim Zeitlin
// Copyright:   (c) 2008 Vadim Zeitlin
// Modified by: JP Mattia, 2024
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

#include "testprec.h"

// this test needs threads as it runs the test server concurrently with the client
#if wxUSE_THREADS

#ifndef WX_PRECOMP
    #include "wx/app.h"
#endif // WX_PRECOMP

#include "ipc_setup_test.h"
#include "ipc_test_server.h"

#include <wx/ipc.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <wx/evtloop.h>

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

    wxEventLoopActivator activate(gs_clientLoop);

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
        wxEventLoopActivator activate(gs_clientLoop);

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

        Create();
    }

protected:
    virtual void *Entry() override
    {
        IPCTestConnection& conn = gs_client->GetConn();

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

    wxDECLARE_NO_COPY_CLASS(MultiRequestThread);
};

// IPCFixture starts the in-process server and the client.
class IPCFixture
{
    std::unique_ptr<wxEventLoop> m_clientLoop{new wxEventLoop};
    IPCServerThread m_server;

public:
    IPCFixture()
    {
#if wxUSE_SOCKETS_FOR_IPC
        wxSocketBase::Initialize();
#endif // wxUSE_SOCKETS_FOR_IPC

        DrainPendingIPCEvents();

        gs_clientLoop = m_clientLoop.get();
        gs_client = new IPCTestClient;

        REQUIRE( m_server.Start() );

        wxMilliSleep(200);
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

    // wait a maximum of 2 seconds for completion.
    int cnt = 0;
    while ( cnt++ < 200 && !conn.m_advise_complete )
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

    // wait a maximum of 20 seconds for completion.
    int cnt = 0;
    while ( cnt++ < 2000 &&
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

    // wait a maximum of 20 seconds for completion.
    int cnt = 0;
    while ( cnt++ < 2000 )
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
    int cnt = 0;
    while ( cnt++ < 20000 )
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

#endif // wxUSE_THREADS

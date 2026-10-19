/////////////////////////////////////////////////////////////////////////////
// Name:        src/common/sckipc.cpp
// Purpose:     Interprocess communication implementation (wxSocket version)
// Author:      Julian Smart
// Modified by: Guilhem Lavaux (big rewrite) May 1997, 1998
//              Guillermo Rodriguez (updated for wxSocket v2) Jan 2000
//                                  (callbacks deprecated)    Mar 2000
//              Vadim Zeitlin (added support for Unix sockets) Apr 2002
//                            (use buffering, many fixes/cleanup) Oct 2008
//              JP Mattia (use messages, fix reporting of errors
//                         and use in multithreading) Sep 2024
// Created:     1993
// Copyright:   (c) Julian Smart 1993
//              (c) Guilhem Lavaux 1997, 1998
//              (c) 2000 Guillermo Rodriguez <guille@iies.es>
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// ==========================================================================
// declarations
// ==========================================================================

// --------------------------------------------------------------------------
// headers
// --------------------------------------------------------------------------

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"


#if wxUSE_SOCKETS && wxUSE_IPC

#include "wx/sckipc.h"

#ifndef WX_PRECOMP
    #include "wx/log.h"
    #include "wx/event.h"
    #include "wx/module.h"
    #include "wx/app.h"
#endif

#include "wx/evtloop.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "wx/socket.h"
#include "wx/stopwatch.h"
#include "wx/thread.h"

#include "wx/private/make_unique.h"

class wxIPCMessageBase;
class wxIPCMessageReader;

// --------------------------------------------------------------------------
// Global variables
// --------------------------------------------------------------------------

wxCRIT_SECT_DECLARE_MEMBER(gs_critical_io);

// --------------------------------------------------------------------------
// macros and constants
// --------------------------------------------------------------------------

namespace
{

// Message codes (don't change them to avoid breaking the existing code using
// wxIPC protocol!)
enum IPCCode
{
    IPC_NULL            = 0,
    IPC_EXECUTE         = 1,
    IPC_REQUEST         = 2,
    IPC_POKE            = 3,
    IPC_ADVISE_START    = 4,
    IPC_ADVISE_REQUEST  = 5, // not used
    IPC_ADVISE          = 6,
    IPC_ADVISE_STOP     = 7,
    IPC_REQUEST_REPLY   = 8,
    IPC_FAIL            = 9,
    IPC_CONNECT         = 10,
    IPC_DISCONNECT      = 11,
    IPC_MAX
};

// A random header, which is used to detect a loss-of-sync on the IPC
// data stream. The header is 24-bits, and the IPCCode above is sent in the
// last 8 bits.
constexpr wxUint32 IPCCodeHeader = 0x439d9600;

} // anonymous namespace

// headers needed for umask()
#ifdef __UNIX_LIKE__
    #include <sys/types.h>
    #include <sys/stat.h>
#endif // __UNIX_LIKE__

// headers needed for IPPROTO_TCP and TCP_NODELAY, used by DisableNagle()
#ifdef __WINDOWS__
    #include "wx/msw/wrapwin.h"
#else
    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

constexpr auto wxNO_RETURN_MESSAGE = nullptr;

constexpr long wxIPCTimeout = 5; // socket timeout, in seconds

// Upper bound on the main-thread requests that can be waiting for their
// replies at the same time, i.e. nested inside each other: a request issued
// from an event handler that runs while another request waits for its reply
// (see SendAndGetReply_MainThread()) queues behind that one, and a handler
// that does so on every timer tick would otherwise grow the stack without
// bound while the peer is slow.
constexpr int wxIPCMaxNestedRequests = 8;

// Size of the reads used to drain a connection socket, see
// wxTCPEventHandler::Drain().
constexpr size_t wxIPCReadChunk = 64 * 1024;


// ----------------------------------------------------------------------------
// private functions
// ----------------------------------------------------------------------------

// Disable Nagle's algorithm on a connection socket. IPC messages are small
// and each one is sent as more than one short write (the code word, then the
// payload), so with Nagle enabled every request/reply round trip stalls for
// a delayed-ACK period (tens of milliseconds), even on loopback. The call
// fails harmlessly on sockets where the option does not apply, such as the
// Unix domain sockets used when the server name is a path.
static void DisableNagle(wxSocketBase* socket)
{
    int nodelay = 1;
    socket->SetOption(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

// get the address object for the given server name, the caller must delete it
static std::unique_ptr<wxSockAddress>
GetAddressFromName(const wxString& serverName,
                   const wxString& host = wxEmptyString)
{
    // we always use INET sockets under non-Unix systems
#if defined(__UNIX__) && !defined(__WINDOWS__) && !defined(__WINE__)
    // under Unix, if the server name looks like a path, create a AF_UNIX
    // socket instead of AF_INET one
    if ( serverName.Find(wxT('/')) != wxNOT_FOUND )
    {
        auto addr = std::make_unique<wxUNIXaddress>();
        addr->Filename(serverName);

        // The explicit conversion is needed by g++ 4.8, which can't
        // implicitly move a returned local to the base class pointer, while
        // "return std::move(addr)" triggers -Wredundant-move in C++20 mode
        // with newer compilers.
        return std::unique_ptr<wxSockAddress>(std::move(addr));
    }
#endif // Unix/!Unix

    auto addr = std::make_unique<wxIPV4address>();
    addr->Service(serverName);
    if ( !host.empty() )
    {
        addr->Hostname(host);
    }

    // See above.
    return std::unique_ptr<wxSockAddress>(std::move(addr));
}

// --------------------------------------------------------------------------
// wxTCPEventHandler declaration (private class)
// --------------------------------------------------------------------------

// Reads and writes on behalf of every IPC connection in the process.
//
// Reading never waits. Every byte available on a connection socket is moved
// into a per-connection receive buffer as soon as the socket signals input (or
// the main thread needs a reply, see below) and complete messages are parsed
// from that buffer and dispatched one at a time. Nothing in the event loop can
// therefore run "inside a read": a handler that issues a request from within
// an advise or a timer runs at a point where the buffer holds whole messages
// only, and its request simply queues behind whatever reply is already
// expected on that connection.
//
// Writing is unchanged: a message is written in one go, waiting for the socket
// if it has to.
class wxTCPEventHandler : public wxEvtHandler
{
public:
    wxTCPEventHandler() = default;

    void OnSocketInput(wxSocketEvent& event);
    void OnSocketConnection(wxSocketEvent& event);

    bool ProcessMessage(wxIPCMessageBase* msg, wxSocketBase *socket);

    bool SendAndGetReply(wxIPCMessageBase& send_msg,
                         IPCCode expected_code,
                         wxSocketBase* socket,
                         wxIPCMessageBase** return_msgptr);

    void SendFailMessage(const wxString& reason, wxSocketBase* socket);
    void HandleDisconnect(wxTCPConnection *connection);

    // Read one message from a socket which is not a connection socket (yet),
    // waiting up to the socket timeout for it to arrive: this is only used
    // for the CONNECT handshake. Returns wxIPCMessageNull on failure, the
    // caller owns the returned message.
    wxIPCMessageBase* ReadMessageBlocking(wxSocketBase* socket);
    bool WriteMessageToSocket(wxIPCMessageBase& msg);

    // This event handler is a single process-wide object shared by every IPC
    // connection (client and server) and it lives until the program exits.
    // Socket events are queued on it and may still be pending after the socket
    // they refer to has been destroyed: Dispatching a stale event would
    // dereference a dangling wxSocketBase pointer. To guard against this we keep
    // the set of sockets that currently have a live connection and ignore events
    // for any socket which is not in it. The check compares only pointer values,
    // so it is safe even if the socket has already been freed.
    void RegisterConnectionSocket(wxSocketBase* socket);
    void UnregisterConnectionSocket(wxSocketBase* socket);
    bool IsConnectionSocket(wxSocketBase* socket);

    // Runs fn on the main thread, blocking the caller until completion (on the
    // main thread fn runs directly). All IPC socket I/O must happen on the main
    // thread: the wxFDIODispatcher used by the main event loop in Unix+Mac is not
    // safe to mutate from worker threads, so worker-thread socket usage is
    // funneled through this method.
    void RunOnMainThread(const std::function<void()>& fn);

    wxIPCMessageBase* GetIPCMessageFromCode(IPCCode code, wxSocketBase* socket);

private:
    // A reply-expecting message (request, start/stop advise) that has been
    // written to a connection and whose reply has not been consumed yet.
    //
    // The entry is shared between the thread waiting for the reply and the
    // connection's queue of expected replies (ReceiveState::pending). Its
    // fields are protected by m_replyMutex, as the waiter may be a worker
    // thread while replies are always delivered by the main thread.
    struct PendingReply
    {
        PendingReply(IPCCode expected_, bool mainThread_)
            : expected(expected_), mainThread(mainThread_)
        {
        }

        ~PendingReply();    // deletes the reply, defined once its type is complete

        const IPCCode expected;     // IPC code of the reply we wait for
        const bool mainThread;      // waiter is the main thread (else a worker)

        wxIPCMessageBase* reply = nullptr;  // the reply, owned, once done
        bool done = false;          // reply received, failed, or connection gone
        bool failed = false;
        bool abandoned = false;     // the waiter timed out: drop the reply

        wxDECLARE_NO_COPY_CLASS(PendingReply);
    };

    using PendingReplyPtr = std::shared_ptr<PendingReply>;

    // Everything received on a connection socket but not dispatched yet, plus
    // the replies expected on it. Only ever touched by the main thread.
    struct ReceiveState
    {
        // Bytes [rd, wr) of buf have been read from the socket and not parsed.
        std::vector<char> buf;
        size_t rd = 0;
        size_t wr = 0;

        size_t Available() const { return wr - rd; }
        const char* Data() const { return buf.data() + rd; }
        void Consume(size_t n) { rd += n; }
        void Compact();

        // Replies expected on this connection, in the order the corresponding
        // messages were written, which is the order the peer answers them in:
        // the reply which arrives next always belongs to the front entry.
        std::deque<PendingReplyPtr> pending;
    };

    wxTCPConnection* GetConnection(wxSocketBase* socket);
    ReceiveState* GetReceiveState(wxSocketBase* socket);

    // Move everything the socket has to offer right now into the buffer,
    // without waiting. Returns false if the peer has closed the connection or
    // the socket failed (what was read before that is still in the buffer).
    bool Drain(wxSocketBase* socket, ReceiveState& state);

    enum ParseResult
    {
        Parse_Complete,     // msg holds the next message, removed from the buffer
        Parse_NeedMore,     // the next message has not fully arrived yet
        Parse_Corrupt       // the buffer does not start with a message header
    };

    ParseResult ParseMessage(ReceiveState& state,
                             wxSocketBase* socket,
                             wxIPCMessageBase*& msg);

    // Drain the socket, then dispatch every complete message in the buffer.
    void DrainAndDispatch(wxSocketBase* socket);

    // If msg is the reply the front pending entry waits for, hand it over
    // (taking ownership) and return true.
    bool DeliverReply(wxSocketBase* socket,
                      ReceiveState& state,
                      wxIPCMessageBase* msg);

    // Fail every reply still expected on the connection: it is going away.
    void FailPendingReplies(wxSocketBase* socket, ReceiveState& state);

    // Queue the entry on the connection and write the message, as one step.
    bool SendAndRegister(wxIPCMessageBase& send_msg,
                         wxSocketBase* socket,
                         const PendingReplyPtr& entry);

    // Take the reply out of a finished entry, or mark the entry abandoned if
    // it is not finished (the waiter timed out).
    bool TakeReply(const PendingReplyPtr& entry,
                   IPCCode expected_code,
                   wxIPCMessageBase** return_msgptr);

    bool SendAndGetReply_MainThread(wxIPCMessageBase& send_msg,
                                    IPCCode expected_code,
                                    wxSocketBase* socket,
                                    wxIPCMessageBase** return_msgptr);
    bool SendAndGetReply_WorkerThread(wxIPCMessageBase& send_msg,
                                      IPCCode expected_code,
                                      wxSocketBase* socket,
                                      wxIPCMessageBase** return_msgptr);

    // Protects the fields of every PendingReply; m_replyCond wakes the worker
    // threads waiting for a reply.
    wxMutex m_replyMutex;
    wxCondition m_replyCond{m_replyMutex};

    // Number of SendAndGetReply_MainThread() calls on the stack.
    int m_mainThreadRequests = 0;

    wxCRIT_SECT_DECLARE_MEMBER(m_cs_connection_sockets);
    std::unordered_map<wxSocketBase*, std::unique_ptr<ReceiveState>> m_connections;

    wxDECLARE_EVENT_TABLE();
    wxDECLARE_NO_COPY_CLASS(wxTCPEventHandler);
};

enum
{
    _SOCKET_INPUT_ID = 1000,
    _SERVER_SOCKET_CONNECTION_ID
};

// --------------------------------------------------------------------------
// wxTCPEventHandlerModule (private class)
// --------------------------------------------------------------------------

class wxTCPEventHandlerModule : public wxModule
{
public:
    wxTCPEventHandlerModule() : wxModule() { }

    // get the global wxTCPEventHandler creating it if necessary
    static wxTCPEventHandler& GetHandler()
    {
        if ( !ms_handler )
            ms_handler = new wxTCPEventHandler;

        return *ms_handler;
    }

    // as ms_handler is initialized on demand, don't do anything in OnInit()
    virtual bool OnInit() override { return true; }
    virtual void OnExit() override { wxDELETE(ms_handler); }

private:
    static wxTCPEventHandler *ms_handler;

    wxDECLARE_DYNAMIC_CLASS(wxTCPEventHandlerModule);
    wxDECLARE_NO_COPY_CLASS(wxTCPEventHandlerModule);
};

wxIMPLEMENT_DYNAMIC_CLASS(wxTCPEventHandlerModule, wxModule);

wxTCPEventHandler *wxTCPEventHandlerModule::ms_handler = nullptr;

// --------------------------------------------------------------------------
// wxIPCMessageBase
// --------------------------------------------------------------------------

// Cursor over the unparsed bytes of a connection's receive buffer, from which
// the messages are parsed. Every Read*() either reads its whole field and
// advances past it, or reads nothing and returns false, meaning that the field
// has not fully arrived yet: the caller then abandons the message and parses
// it again from its start once more bytes are in. Only the data block of a
// message can be large and it is only copied once, when it is complete.
class wxIPCMessageReader
{
public:
    wxIPCMessageReader(const char* data, size_t len)
        : m_data(data), m_len(len), m_pos(0)
    {
    }

    size_t GetConsumed() const { return m_pos; }

    bool ReadBytes(void* dst, size_t n)
    {
        if ( m_len - m_pos < n )
            return false;

        memcpy(dst, m_data + m_pos, n);
        m_pos += n;
        return true;
    }

    bool Read32(wxUint32& word)
    {
        return ReadBytes(&word, 4);
    }

    bool ReadByte(wxUint8& byte)
    {
        return ReadBytes(&byte, 1);
    }

    // A 32-bit length followed by that many bytes.
    bool ReadBlock(const char*& block, wxUint32& len)
    {
        const size_t start = m_pos;
        if ( !Read32(len) || m_len - m_pos < len )
        {
            m_pos = start;
            return false;
        }

        block = m_data + m_pos;
        m_pos += len;
        return true;
    }

private:
    const char* const m_data;
    const size_t m_len;
    size_t m_pos;
};

// This class and its derivatives manage the writing of messages to the socket
// and their parsing from the bytes received from it.
class wxIPCMessageBase : public wxObject
{
public:
    wxIPCMessageBase(wxSocketBase* socket = nullptr)
        : m_writeData(nullptr)
    {
        Init(socket);
    }

    wxIPCMessageBase(wxSocketBase* socket, const void* data)
        : m_writeData(data)
    {
        Init(socket);
    }

    bool IsOk() const { return m_ipcCode != IPC_NULL; }

    // Accessors for the base object
    IPCCode GetIPCCode() const { return m_ipcCode; }
    void SetIPCCode(IPCCode ipc_code) { m_ipcCode = ipc_code; }

    wxSocketBase* GetSocket() const { return m_socket; }
    void SetSocket(wxSocketBase* socket) { m_socket = socket; }

    wxSocketError GetError() const { return m_error; }
    void SetError(wxSocketError error) { m_error = error; }

    // These accessors are here to avoid repetition in the derived objects.
    wxIPCFormat GetIPCFormat() const { return m_ipcFormat; }
    void SetIPCFormat(wxIPCFormat ipc_format) { m_ipcFormat = ipc_format; }

    const void* GetReadData() const { return m_readData; }
    void SetReadData(void *data) { m_readData = data; }

    // Relinquish ownership of the buffer allocated by ReadSizeAndData() to
    // the caller, which becomes responsible for delete[]-ing it.
    char* ReleaseReadData()
    {
        m_readData = nullptr;
        return m_readDataBuf.release();
    }

    size_t GetSize() const { return m_size; }
    void SetSize(size_t size) { m_size = size; }

    wxString GetItem() const { return m_item; }
    void SetItem(const wxString& item) { m_item = item; }

protected:
    void Init(wxSocketBase* socket)
    {
        SetIPCCode(IPC_NULL);
        SetSocket(socket);
        SetError(wxSOCKET_NOERROR);

        SetIPCFormat(wxIPC_INVALID);
        SetReadData(nullptr);
        SetSize(0);
    }

    // Parse the fields following the IPC code from the given bytes. Returns
    // false if they have not all arrived yet.
    virtual bool ReadFrom(wxIPCMessageReader& in) = 0;

    virtual bool DataToSocket() = 0;

    // primitives for parsing a message from memory

    bool ReadIPCFormat(wxIPCMessageReader& in)
    {
        wxUint8 format;
        if ( !in.ReadByte(format) )
            return false;

        m_ipcFormat = static_cast<wxIPCFormat>(format);
        return true;
    }

    bool ReadString(wxIPCMessageReader& in, wxString& str);
    bool ReadSizeAndData(wxIPCMessageReader& in);

    // primitives for writing to the socket

    bool VerifyValidSocket()
    {
        if (m_socket && m_socket->IsOk() && m_socket->IsConnected() )
            return true;

        SetError(wxSOCKET_INVSOCK);
        return false;
    }

    bool Write32(wxUint32 word)
    {
        if ( !VerifyValidSocket() )
            return false;

        m_socket->Write(reinterpret_cast<char *>(&word), 4);

        return VerifyLastWriteCount(4);
    }

    bool WriteData(const void* data, wxUint32 nbytes)
    {
        if ( !VerifyValidSocket() )
            return false;

        m_socket->Write(data, nbytes);

        return VerifyLastWriteCount(nbytes);
    }

    bool WriteSizeAndData()
    {
        if (!Write32(m_size))
            return false;

        return WriteData(m_writeData, m_size);
    }

    bool WriteIPCCode()
    {
        wxUint32 code_with_header = IPCCodeHeader | GetIPCCode();
        return Write32(code_with_header);
    }

    bool WriteIPCFormat()
    {
        if ( !VerifyValidSocket() )
            return false;

        const wxUint8 format = static_cast<wxUint8>(m_ipcFormat);
        m_socket->Write(&format, 1);

        return VerifyLastWriteCount(1);
    }

    bool WriteString(const wxString& str);

    bool VerifyLastWriteCount(wxUint32 nbytes)
    {
        if ( !VerifyValidSocket() )
            return false;

        if (m_socket->Error())
        {
            SetError(m_socket->LastError());
            return false;
        }

        if (m_socket->LastWriteCount() != nbytes)
        {
            SetError(wxSOCKET_IOERR);
            return false;
        }
        return true;
    }

    IPCCode m_ipcCode;

    wxSocketBase* m_socket;
    wxSocketError m_error;

    // Members used in most of the derived messages
    wxUint32 m_size;
    wxIPCFormat m_ipcFormat;
    wxString m_item;

    // immutable pointer to data from connection object
    const void* m_writeData;

    // pointer to data read from socket
    void* m_readData;

    // owns the buffer m_readData points to when it was allocated by
    // ReadSizeAndData(), so that the data read from the socket is freed with
    // the message unless ReleaseReadData() has passed it on
    std::unique_ptr<char[]> m_readDataBuf;

    friend wxTCPEventHandler;

    wxDECLARE_NO_COPY_CLASS(wxIPCMessageBase);
    wxDECLARE_CLASS(wxIPCMessageBase);
};

// Parses a 32-bit word giving the size of the data block, then copies the
// block into a buffer of that size owned by this message: it is freed with the
// message unless ReleaseReadData() is used to take it over.
bool wxIPCMessageBase::ReadSizeAndData(wxIPCMessageReader& in)
{
    const char* block = nullptr;
    wxUint32 size = 0;
    if ( !in.ReadBlock(block, size) )
        return false;

    m_readDataBuf.reset(new char[size]);
    memcpy(m_readDataBuf.get(), block, size);
    m_readData = m_readDataBuf.get();
    m_size = size;
    return true;
}

bool wxIPCMessageBase::ReadString(wxIPCMessageReader& in, wxString& str)
{
    const char* block = nullptr;
    wxUint32 len = 0;
    if ( !in.ReadBlock(block, len) )
        return false;

    if ( len > 0 )
        str = wxString::FromUTF8(block, len);

    return true;
}

bool wxIPCMessageBase::WriteString(const wxString& str)
{
    const wxScopedCharBuffer& buf = str.utf8_str();
    size_t len = buf.length();

    if (len > 0)
        return Write32(len) && WriteData(buf, len);
    else
        return Write32(len);
}


// ==========================================================================
// wxIPCMessages
// ==========================================================================

class wxIPCMessageExecute : public wxIPCMessageBase
{
public:
    wxIPCMessageExecute(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_EXECUTE);
    }

    wxIPCMessageExecute(wxSocketBase* socket,
                        const void* data,
                        size_t size,
                        wxIPCFormat format)
        : wxIPCMessageBase(socket, data)
    {
        SetIPCCode(IPC_EXECUTE);
        SetSize((wxUint32) size);
        SetIPCFormat(format);
    }

protected:
    bool DataToSocket() override
    {
        return WriteIPCFormat() && WriteSizeAndData();
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadIPCFormat(in) && ReadSizeAndData(in);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageExecute);
};

class wxIPCMessageRequest : public wxIPCMessageBase
{
public:
    wxIPCMessageRequest(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_REQUEST);
        SetIPCFormat(wxIPC_INVALID);
    }

    wxIPCMessageRequest(wxSocketBase* socket,
                        const wxString& item,
                        wxIPCFormat format)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_REQUEST);
        SetItem(item);
        SetIPCFormat(format);
    }

protected:
    bool DataToSocket() override
    {
        return WriteIPCFormat() && WriteString(m_item);
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadIPCFormat(in) && ReadString(in, m_item);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageRequest);
};

class wxIPCMessageRequestReply : public wxIPCMessageBase
{
public:
    wxIPCMessageRequestReply(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_REQUEST_REPLY);
    }

    wxIPCMessageRequestReply(wxSocketBase* socket,
                             const void* user_data,
                             size_t user_size,
                             const wxString& item,
                             wxIPCFormat format)
        : wxIPCMessageBase(socket, user_data)
    {
        SetIPCCode(IPC_REQUEST_REPLY);
        SetItem(item);
        SetIPCFormat(format);

        wxUint32 len = user_size;
        if ( user_size == wxNO_LEN )
        {
            switch ( format )
            {
            case wxIPC_TEXT:
            case wxIPC_UTF8TEXT:
                len = strlen((const char *)user_data) + 1;  // includes final NUL
                break;
            case wxIPC_UNICODETEXT:
                len = (wcslen((const wchar_t *)user_data) + 1) * sizeof(wchar_t);  // includes final NUL
                break;
            default:
                len = 0;
            }
        }
        SetSize(len);
    }

protected:
    bool DataToSocket() override
    {
        return WriteIPCFormat() && WriteString(m_item) && WriteSizeAndData();
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadIPCFormat(in) && ReadString(in, m_item) && ReadSizeAndData(in);
    }
    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageRequestReply);
};

class wxIPCMessagePoke : public wxIPCMessageBase
{
public:
    wxIPCMessagePoke(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_POKE);
    }

    wxIPCMessagePoke(wxSocketBase* socket,
                     const wxString& item,
                     const void* data,
                     size_t size,
                     wxIPCFormat format)
        : wxIPCMessageBase(socket, data)
    {
        SetIPCCode(IPC_POKE);
        SetItem(item);
        SetIPCFormat(format);
        SetSize(size);
    }

protected:
    bool DataToSocket() override
    {
        return WriteIPCFormat() && WriteString(m_item) && WriteSizeAndData();
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadIPCFormat(in) && ReadString(in, m_item) && ReadSizeAndData(in);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessagePoke);
};

class wxIPCMessageAdviseStart : public wxIPCMessageBase
{
public:
    wxIPCMessageAdviseStart(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_ADVISE_START);
    }

    wxIPCMessageAdviseStart(wxSocketBase* socket, const wxString& item)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_ADVISE_START);
        SetItem(item);
    }

protected:
    bool DataToSocket() override
    {
        return WriteString(m_item);
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadString(in, m_item);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageAdviseStart);
};

class wxIPCMessageAdviseStop : public wxIPCMessageBase
{
public:
    wxIPCMessageAdviseStop(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_ADVISE_STOP);
    }

    wxIPCMessageAdviseStop(wxSocketBase* socket, const wxString& item)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_ADVISE_STOP);
        SetItem(item);
    }

protected:
    bool DataToSocket() override
    {
        return WriteString(m_item);
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadString(in, m_item);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageAdviseStop);
};

class wxIPCMessageAdvise : public wxIPCMessageBase
{
public:
    wxIPCMessageAdvise(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_ADVISE);
    }

    wxIPCMessageAdvise(wxSocketBase* socket,
                       const wxString& item,
                       const void* data,
                       size_t size,
                       wxIPCFormat format)
        : wxIPCMessageBase(socket, data)
    {
        SetIPCCode(IPC_ADVISE);
        SetItem(item);
        SetIPCFormat(format);
        SetSize(size);
    }

protected:
    bool DataToSocket() override
    {
        return WriteIPCFormat() && WriteString(m_item) && WriteSizeAndData();
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadIPCFormat(in) && ReadString(in, m_item) && ReadSizeAndData(in);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageAdvise);
};

// m_item to be used for failure reason (for debug)
class wxIPCMessageFail : public wxIPCMessageBase
{
public:
    wxIPCMessageFail(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_FAIL);
    }

    wxIPCMessageFail(wxSocketBase* socket, const wxString& item)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_FAIL);
        SetItem(item);
    }

protected:
    bool DataToSocket() override
    {
        return WriteString(m_item);
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadString(in, m_item);
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageFail);
};

// Message returned when socket fails to read an wxIPCMessage
class wxIPCMessageNull : public wxIPCMessageBase
{
public:
    wxIPCMessageNull(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_NULL);
    }

protected:
    bool DataToSocket() override
    {
        return false;
    }

    bool ReadFrom(wxIPCMessageReader& WXUNUSED(in)) override
    {
        return false;
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageNull);
};

class wxIPCMessageConnect : public wxIPCMessageBase
{
public:
    wxIPCMessageConnect(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_CONNECT);
    }

    wxIPCMessageConnect(wxSocketBase* socket, const wxString& topic)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_CONNECT);
        SetTopic(topic);
    }

    wxString GetTopic() const { return m_topic; }
    void SetTopic(const wxString& topic) { m_topic = topic; }

protected:
    bool DataToSocket() override
    {
        return WriteString(m_topic);
    }

    bool ReadFrom(wxIPCMessageReader& in) override
    {
        return ReadString(in, m_topic);
    }

    wxString m_topic;

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageConnect);
};


class wxIPCMessageDisconnect : public wxIPCMessageBase
{
public:
    wxIPCMessageDisconnect(wxSocketBase* socket = nullptr)
        : wxIPCMessageBase(socket)
    {
        SetIPCCode(IPC_DISCONNECT);
    }

protected:
    bool DataToSocket() override
    {
        return true;
    }

    bool ReadFrom(wxIPCMessageReader& WXUNUSED(in)) override
    {
        return true;
    }

    wxDECLARE_DYNAMIC_CLASS(wxIPCMessageDisconnect);
};

// Utility to ensure deletion of wxIPCMessageBase after use
class wxIPCMessageBaseLocker
{
public:
    wxIPCMessageBaseLocker(wxIPCMessageBase* msg)
    {
        m_msg = msg;
    }

    ~wxIPCMessageBaseLocker()
    {
        if (m_msg) delete m_msg;
    }

    wxIPCMessageBase* m_msg;
};

namespace
{

/*
    Per-thread holder for the last data buffer returned to user code by
    Request(): wxWidgets docs say the user must not free that pointer, so we
    keep it alive here until the same thread makes its next Request() call,
    which replaces (and frees) it. Each thread has its own slot, so requests
    made concurrently from several threads cannot invalidate each other's
    returned data.

    The first version of this class works around a long-standing MinGW bug in
    its thread_local implementation (the object's memory is deallocated before
    its destructor runs, see https://github.com/msys2/MINGW-packages/issues/2519
    and the comment above UntranslatedStringHolder in translation.cpp): the
    buffers are kept in a global map and the destructor only touches globals.

    Those globals are allocated on first use and never destroyed. The main
    thread's thread_local destructor is run by the MinGW CRT's TLS terminator,
    after the atexit table, i.e. after any static object (including a
    function-local one) has been destroyed; entering a destroyed critical
    section there crashed the process at exit. Leaking one critical section and
    one map per process is the only ordering that is always safe.
 */
#if defined(__MINGW32__) && \
    (!defined(__MINGW64_VERSION_MAJOR) || __MINGW64_VERSION_MAJOR < 15)

class IPCRequestBufferHolder
{
private:
    static wxCriticalSection& CriticalSection()
    {
        static wxCriticalSection* const cs = new wxCriticalSection;
        return *cs;
    }
    static std::map<wxThreadIdType, char*>& BuffersMap()
    {
        static std::map<wxThreadIdType, char*>* const m =
            new std::map<wxThreadIdType, char*>;
        return *m;
    }

public:
    IPCRequestBufferHolder() = default;

    const void* assign(char* buf)
    {
        wxCriticalSectionLocker locker(CriticalSection());
        char*& slot = BuffersMap()[wxThread::GetCurrentId()];
        delete[] slot;
        slot = buf;
        return buf;
    }

    ~IPCRequestBufferHolder()
    {
        // This code is run after this object memory has been deallocated so we
        // cannot access any member variables, but we can access global ones.
        wxCriticalSectionLocker locker(CriticalSection());
        const auto it = BuffersMap().find(wxThread::GetCurrentId());
        if ( it != BuffersMap().end() )
        {
            delete[] it->second;
            BuffersMap().erase(it);
        }
    }

    wxDECLARE_NO_COPY_CLASS(IPCRequestBufferHolder);
};

#else // !__MINGW32__

class IPCRequestBufferHolder
{
private:
    std::unique_ptr<char[]> m_buf;

public:
    IPCRequestBufferHolder() = default;

    const void* assign(char* buf)
    {
        m_buf.reset(buf);
        return buf;
    }

    wxDECLARE_NO_COPY_CLASS(IPCRequestBufferHolder);
};

#endif // __MINGW32__/!__MINGW32__

// Transfer ownership of the message's read buffer to the calling thread and
// return it: the buffer stays valid until the next call to this function on
// the same thread, or until the thread ends.
const void* TakeReadDataForThisThread(wxIPCMessageBase* msg)
{
    thread_local IPCRequestBufferHolder wxPerThreadIPCBuffer;
    return wxPerThreadIPCBuffer.assign(msg->ReleaseReadData());
}

} // anonymous namespace

// ==========================================================================
// implementation
// ==========================================================================

wxIMPLEMENT_DYNAMIC_CLASS(wxTCPServer, wxServerBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxTCPClient, wxClientBase);
wxIMPLEMENT_CLASS(wxTCPConnection, wxConnectionBase);

wxIMPLEMENT_CLASS(wxIPCMessageBase,wxObject);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageExecute,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageRequest,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageRequestReply,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessagePoke,wxIPCMessageBase);

wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageAdviseStart,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageAdviseStop,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageAdvise,wxIPCMessageBase);

wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageConnect,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageDisconnect,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageFail,wxIPCMessageBase);
wxIMPLEMENT_DYNAMIC_CLASS(wxIPCMessageNull,wxIPCMessageBase);

namespace
{

// Cast a message read from the socket to its concrete type after its IPC code
// has been checked.
//
// The concrete type of a message is uniquely determined by its IPC code (see
// GetIPCMessageFromCode(), the only place where the messages read from the
// socket are created), so the cast cannot fail and a static_cast is enough;
// still verify the invariant in debug builds.
template <typename T>
T* wxIPCMessageCast(wxIPCMessageBase* msg)
{
    wxASSERT_MSG( wxDynamicCast(msg, T),
                  "IPC message type doesn't correspond to its IPC code" );

    return static_cast<T*>(msg);
}

} // anonymous namespace

// --------------------------------------------------------------------------
// wxTCPClient
// --------------------------------------------------------------------------

wxTCPClient::wxTCPClient()
    : wxClientBase()
{
}

bool wxTCPClient::ValidHost(const wxString& host)
{
    wxIPV4address addr;

    return addr.Hostname(host);
}

wxConnectionBase *wxTCPClient::MakeConnection(const wxString& host,
                                              const wxString& serverName,
                                              const wxString& topic)
{
    auto addr = GetAddressFromName(serverName, host);
    if ( !addr )
        return nullptr;

    wxSocketClient * const client = new wxSocketClient(wxSOCKET_WAITALL);

    // Bound the connection attempt (the TCP connect and the topic handshake
    // that follows) by the IPC timeout instead of leaving it at the socket's
    // long default: without this, connecting to a listener that is not yet
    // ready to complete the handshake blocks for the default timeout (ten
    // minutes) rather than failing promptly so the caller can retry. The
    // per-connection socket timeout is set to the same value once connected.
    client->SetTimeout(wxIPCTimeout);

    bool ok = client->Connect(*addr);


    if ( ok )
    {
        DisableNagle(client);

        wxTCPEventHandler* handler = &wxTCPEventHandlerModule::GetHandler();

        // Send topic name, and enquire whether this has succeeded
        wxIPCMessageConnect msg(client, topic);
        if ( !handler->WriteMessageToSocket(msg) )
        {
            client->Destroy();
            return nullptr;
        }

        wxIPCMessageBase* msg_reply = handler->ReadMessageBlocking(client);
        wxIPCMessageBaseLocker lock(msg_reply);

        // OK! Confirmation.
        if ( msg_reply->GetIPCCode() == IPC_CONNECT )
        {
            auto* const connectionBase = OnMakeConnection ();

            if (auto* const connection = wxDynamicCast(connectionBase, wxTCPConnection))
            {
                connection->m_sock = client;
                connection->m_sock->SetTimeout(wxIPCTimeout);
                connection->m_handler = handler;
                connection->m_topic = topic;

                client->SetEventHandler(*handler, _SOCKET_INPUT_ID);
                client->SetClientData(connection);
                handler->RegisterConnectionSocket(client);
                client->SetNotify(wxSOCKET_INPUT_FLAG | wxSOCKET_LOST_FLAG);
                client->Notify(true);
                return connection;
            }
            else
            {
                delete connectionBase;
                // and fall through to delete everything else
            }
        }
        else if ( msg_reply->GetIPCCode() == IPC_FAIL )
        {
            // The cast is done inside the macro argument so that no unused
            // variable is left behind when wxLogDebug() expands to nothing.
            wxLogDebug("FailMessage received: " +
                       wxIPCMessageCast<wxIPCMessageFail>(msg_reply)->GetItem());
        }
    }

    // Something went wrong
    client->Destroy();

    return nullptr;
}

wxConnectionBase *wxTCPClient::OnMakeConnection()
{
    return new wxTCPConnection();
}

// --------------------------------------------------------------------------
// wxTCPServer
// --------------------------------------------------------------------------

wxTCPServer::wxTCPServer() = default;

bool wxTCPServer::Create(const wxString& serverName)
{
    // Destroy previous server, if any
    if (m_server)
    {
        m_server->SetClientData(nullptr);
        m_server->Destroy();
        m_server = nullptr;
    }

    auto addr = GetAddressFromName(serverName);
    if ( !addr )
        return false;

#ifdef __UNIX_LIKE__
    mode_t umaskOld;
    if ( addr->Type() == wxSockAddress::UNIX )
    {
        // ensure that the file doesn't exist as otherwise calling socket()
        // would fail
        int rc = remove(serverName.fn_str());
        if ( rc < 0 && errno != ENOENT )
        {
            return false;
        }

        // also set the umask to prevent the others from reading our file
        umaskOld = umask(077);
    }
    else
    {
        // unused anyhow but shut down the compiler warnings
        umaskOld = 0;
    }
#endif // __UNIX_LIKE__

    // Create a socket listening on the specified port (reusing it to allow
    // restarting the server listening on the same port as was used by the
    // previous instance of this server)
    m_server = new wxSocketServer(*addr, wxSOCKET_WAITALL | wxSOCKET_REUSEADDR);

#ifdef __UNIX_LIKE__
    if ( addr->Type() == wxSockAddress::UNIX )
    {
        // restore the umask
        umask(umaskOld);

        // save the file name to remove it later
        m_filename = serverName;
    }
#endif // __UNIX_LIKE__

    if (!m_server->IsOk())
    {
        m_server->Destroy();
        m_server = nullptr;

        return false;
    }

    m_server->SetEventHandler(wxTCPEventHandlerModule::GetHandler(),
                              _SERVER_SOCKET_CONNECTION_ID);
    m_server->SetClientData(this);
    m_server->SetNotify(wxSOCKET_CONNECTION_FLAG);
    m_server->Notify(true);

    return true;
}

wxTCPServer::~wxTCPServer()
{
    if ( m_server )
    {
        m_server->SetClientData(nullptr);
        m_server->Destroy();
    }

#ifdef __UNIX_LIKE__
    if ( !m_filename.empty() )
    {
        if ( remove(m_filename.fn_str()) != 0 )
        {
            wxLogDebug(wxT("Stale AF_UNIX file '%s' left."), m_filename);
        }
    }
#endif // __UNIX_LIKE__
}

wxConnectionBase *
wxTCPServer::OnAcceptConnection(const wxString& WXUNUSED(topic))
{
    return new wxTCPConnection();
}

// --------------------------------------------------------------------------
// wxTCPConnection
// --------------------------------------------------------------------------

void wxTCPConnection::Init()
{
    m_sock = nullptr;
    m_handler = nullptr;
}

wxTCPConnection::~wxTCPConnection()
{
    Disconnect();

    if ( m_sock )
    {
        if ( m_handler )
            m_handler->UnregisterConnectionSocket(m_sock);
        m_sock->SetClientData(nullptr);
        m_sock->Destroy();
    }
}

void wxTCPConnection::Compress(bool WXUNUSED(on))
{
    // TODO
}

// Calls that CLIENT can make.
bool wxTCPConnection::Disconnect()
{
    if ( !GetConnected() )
        return true;

    if ( m_sock && m_handler)
    {
        // Send the disconnect message to the peer.
        wxIPCMessageDisconnect msg(m_sock);
        if ( !m_handler->WriteMessageToSocket(msg) )
            wxLogDebug("Failed to send IPC_DISCONNECT message");

        m_sock->Notify(false);
        m_sock->Close();
    }

    SetConnected(false);

    return true;
}

bool wxTCPConnection::DoExecute(const void *data,
                                size_t size,
                                wxIPCFormat format)
{
    if ( !m_handler )
        return false;

    wxIPCMessageExecute msg(m_sock, data, size, format);
    return m_handler->WriteMessageToSocket(msg);
}

const void *wxTCPConnection::Request(const wxString& item,
                                     size_t *size,
                                     wxIPCFormat format)
{
    if ( !m_handler )
        return nullptr;

    wxIPCMessageRequest msg(m_sock, item, format);
    wxIPCMessageBase* msg_reply = nullptr;

    if ( !m_handler->SendAndGetReply(msg, IPC_REQUEST_REPLY, m_sock, &msg_reply) )
        return nullptr;

    wxIPCMessageBaseLocker lockmsg(msg_reply);
    if ( !msg_reply || !msg_reply->IsOk() )
        return nullptr;

    if (size)
      *size = msg_reply->GetSize();

    // The reply message is deleted when this function returns, so hand its
    // buffer over to the calling thread, which keeps it valid until the next
    // Request() on this thread.
    return TakeReadDataForThisThread(msg_reply);
}

bool wxTCPConnection::DoPoke(const wxString& item,
                             const void *data,
                             size_t size,
                             wxIPCFormat format)
{
    if ( !m_handler )
        return false;

    wxIPCMessagePoke msg(m_sock, item, data, size, format);
    return m_handler->WriteMessageToSocket(msg);
}

bool wxTCPConnection::StartAdvise(const wxString& item)
{
    if ( !m_handler )
        return false;

    wxIPCMessageAdviseStart msg(m_sock, item);
    return m_handler->SendAndGetReply(msg, IPC_ADVISE_START, m_sock, wxNO_RETURN_MESSAGE);
}

bool wxTCPConnection::StopAdvise (const wxString& item)
{
    if ( !m_handler )
        return false;

    wxIPCMessageAdviseStop msg(m_sock, item);
    return m_handler->SendAndGetReply(msg, IPC_ADVISE_STOP, m_sock, wxNO_RETURN_MESSAGE);
}


// Calls that Server can make
bool wxTCPConnection::DoAdvise(const wxString& item,
                               const void *data,
                               size_t size,
                               wxIPCFormat format)
{
    if ( !m_handler )
        return false;

    wxIPCMessageAdvise msg(m_sock, item, data, size, format);
    return m_handler->WriteMessageToSocket(msg);
}

// --------------------------------------------------------------------------
// wxTCPEventHandler implementation (private class)
// --------------------------------------------------------------------------

wxBEGIN_EVENT_TABLE(wxTCPEventHandler, wxEvtHandler)
    EVT_SOCKET(_SOCKET_INPUT_ID, wxTCPEventHandler::OnSocketInput)
    EVT_SOCKET(_SERVER_SOCKET_CONNECTION_ID, wxTCPEventHandler::OnSocketConnection)
wxEND_EVENT_TABLE()

// Function that gets called when wxSocket receives info. It always
// runs on wxThread::Main
void wxTCPEventHandler::OnSocketInput(wxSocketEvent &event)
{
    // The handler may receive a socket event that was queued before its socket
    // was destroyed (e.g. a wxSOCKET_LOST generated as a connection is being torn
    // down). Such an event must be ignored without dereferencing the socket,
    // since it now points to freed memory. Even obtaining the typed pointer via
    // GetSocket() performs a checked downcast (wxObject* -> wxSocketBase*), which
    // is undefined behavior on a freed object and trips UBSAN's vptr check, so
    // use reinterpret_cast (not vptr-instrumented) purely for the registry
    // pointer-value lookup below. IsConnectionSocket() only compares the pointer
    // value and never dereferences it, so it is safe to call here.
    wxSocketBase *sock =
        reinterpret_cast<wxSocketBase *>(event.GetEventObject());
    if ( !IsConnectionSocket(sock) )
        return;

    wxTCPConnection * connection = GetConnection(sock);

    // This socket is being deleted
    if ( !connection )
    {
        if ( ReceiveState* const state = GetReceiveState(sock) )
            FailPendingReplies(sock, *state);
        return;
    }

    if ( event.GetSocketEvent() == wxSOCKET_LOST )
    {
        HandleDisconnect(connection);
        return;
    }

    DrainAndDispatch(sock);
}

// Process a wxSOCKET_CONNECTION event for wxServer. It always runs on
// wxThread::Main
void wxTCPEventHandler::OnSocketConnection(wxSocketEvent &event)
{
    wxSocketServer *server = (wxSocketServer *) event.GetSocket();
    if (!server)
        return;
    wxTCPServer *ipcserv = (wxTCPServer *) server->GetClientData();

    // This socket is being deleted; skip this event
    if (!ipcserv)
        return;

    if ( event.GetSocketEvent() != wxSOCKET_CONNECTION )
        return;

    // Accept the connection, getting a new socket
    wxSocketBase *sock = server->Accept();
    if (!sock)
        return;

    if ( !sock->IsOk() )
    {
        sock->Destroy();
        return;
    }

    DisableNagle(sock);

    wxIPCMessageBase* msg = ReadMessageBlocking(sock);
    wxIPCMessageBaseLocker lock(msg);

    if ( msg->GetIPCCode() == IPC_CONNECT )
    {
        wxIPCMessageConnect* msg_conn = wxIPCMessageCast<wxIPCMessageConnect>(msg);

        wxString topic = msg_conn->GetTopic();

        auto* const connectionBase = ipcserv->OnAcceptConnection(topic);

        if ( auto* const new_connection = wxDynamicCast(connectionBase, wxTCPConnection) )
        {
            // Acknowledge success
            wxIPCMessageConnect msg_reply(sock, topic);

            if ( WriteMessageToSocket(msg_reply) )
            {
                new_connection->m_topic = topic;
                new_connection->m_sock = sock;
                new_connection->m_handler = &wxTCPEventHandlerModule::GetHandler();

                sock->SetTimeout(wxIPCTimeout);
                sock->SetEventHandler(wxTCPEventHandlerModule::GetHandler(),
                                      _SOCKET_INPUT_ID);
                sock->SetClientData(new_connection);
                RegisterConnectionSocket(sock);
                sock->SetNotify(wxSOCKET_INPUT_FLAG | wxSOCKET_LOST_FLAG);
                sock->Notify(true);
                return;
            }
        }

        delete connectionBase;
    }

    SendFailMessage("IPC CONNECT failed to create valid connection", sock);
    sock->Destroy();
}

// Process a single wxIPCMessage from the socket. Returns true if processing
// can continue, or false if processing should stop, usually because a
// disconnect was received.
bool wxTCPEventHandler::ProcessMessage(wxIPCMessageBase* msg, wxSocketBase *socket)
{
    if ( !msg || !msg->IsOk() )
        return false;

    wxTCPConnection * const connection = GetConnection(socket);
    if ( !connection || !connection->GetConnected() )
        return false; // socket is being deleted

    wxString errmsg;

    switch ( msg->GetIPCCode() )
    {
    case IPC_EXECUTE:
    {
        wxIPCMessageExecute* msg_execute =
            wxIPCMessageCast<wxIPCMessageExecute>(msg);

        connection->OnExecute(connection->m_topic,
                              msg_execute->GetReadData(),
                              msg_execute->GetSize(),
                              msg_execute->GetIPCFormat());
    }
    break;

    case IPC_ADVISE:
    {
        wxIPCMessageAdvise* msg_advise =
            wxIPCMessageCast<wxIPCMessageAdvise>(msg);

        connection->OnAdvise(connection->m_topic,
                             msg_advise->GetItem(),
                             msg_advise->GetReadData(),
                             msg_advise->GetSize(),
                             msg_advise->GetIPCFormat());
    }
    break;

    case IPC_ADVISE_START:
    {
        wxIPCMessageAdviseStart* msg_advise_start =
            wxIPCMessageCast<wxIPCMessageAdviseStart>(msg);

        if ( connection->OnStartAdvise(connection->m_topic,
                                       msg_advise_start->GetItem()) )
        {
            // reply to confirm
            wxIPCMessageAdviseStart reply(socket,
                                          msg_advise_start->GetItem());
            if ( !WriteMessageToSocket(reply) )
                errmsg = "Failed to confirm IPC StartAdvise";
        }
        else
        {
            errmsg = "IPC StartAdvise refused by peer";
        }

    }
    break;

    case IPC_ADVISE_STOP:
    {
        wxIPCMessageAdviseStop* msg_advise_stop =
            wxIPCMessageCast<wxIPCMessageAdviseStop>(msg);

        if ( connection->OnStopAdvise(connection->m_topic, msg_advise_stop->GetItem()) )
        {
            // reply to confirm
            wxIPCMessageAdviseStop reply(socket, msg_advise_stop->GetItem());
            if ( !WriteMessageToSocket(reply) )
                errmsg = "Failed to confirm IPC StopAdvise";
        }
        else
        {
            errmsg = "IPC StopAdvise refused by peer";
        }

    }
    break;

    case IPC_POKE:
    {
        wxIPCMessagePoke* msg_poke =
            wxIPCMessageCast<wxIPCMessagePoke>(msg);

        connection->OnPoke(connection->m_topic,
                           msg_poke->GetItem(),
                           msg_poke->GetReadData(),
                           msg_poke->GetSize(),
                           msg_poke->GetIPCFormat());
    }
    break;

    case IPC_REQUEST:
    {
        size_t user_size = wxNO_LEN;
        const void *user_data = connection->OnRequest(connection->m_topic,
                                                      msg->GetItem(),
                                                      &user_size,
                                                      msg->GetIPCFormat());
        if ( !user_data )
        {
            SendFailMessage("No reply data for request.", socket);
            break;
        }

        wxIPCMessageRequestReply msg_reply(socket,
                                           user_data,
                                           user_size,
                                           msg->GetItem(),
                                           msg->GetIPCFormat());

        if ( !WriteMessageToSocket(msg_reply) )
            errmsg = "Reply failed for IPC Request";
    }
    break;

    case IPC_DISCONNECT:
        HandleDisconnect(connection);
        return false;

    case IPC_FAIL:
    {
        // The cast is done inside the macro argument so that no unused
        // variable is left behind when wxLogDebug() expands to nothing.
        wxLogDebug("Unexpected IPC_FAIL received: " +
                   wxIPCMessageCast<wxIPCMessageFail>(msg)->GetItem());

        return false;
    }

    // Silence unused-enum warnings from the compiler. These should never be
    // received in this method.
    case IPC_ADVISE_REQUEST:
    case IPC_REQUEST_REPLY:
    case IPC_CONNECT:
    case IPC_MAX:
        wxLogDebug("Invalid IPC code %d received", msg->GetIPCCode());
        return false;

    default:
        wxLogDebug("Unknown message code %d received. IPC data may be misaligned",
                   msg);
        return false;
    }

    if ( !errmsg.IsEmpty() )
    {
        wxLogDebug(errmsg);
        SendFailMessage(errmsg, socket);
    }

    return true;
}

//Runs fn on the main thread, blocking the caller until completion. On the
// main thread, fn runs directly.
void wxTCPEventHandler::RunOnMainThread(const std::function<void()>& fn)
{
    if ( wxThread::IsMain() )
    {
        fn();
        return;
    }

    // Marshal to the main thread and block until it has run the work. CallAfter()
    // posts an async method-call event that the main event loop dispatches; the
    // worker waits on the semaphore meanwhile, so the by-reference captures stay
    // valid. The wait is bounded by wxIPCTimeout.
    wxSemaphore done;
    CallAfter([&fn, &done] {
        fn();
        done.Post();
    });
    done.Wait();
}

// Messages that are sent to the peer with an expected reply are handled here.
//
// Replies carry no identifier, so they are matched to the messages by order:
// the peer handles the messages it receives on a connection one after the
// other and answers each before looking at the next, so the replies come back
// in the order the messages were written. Each message expecting a reply is
// queued on its connection (ReceiveState::pending) in the same step as it is
// written, and the next reply-class message parsed from that connection is the
// reply to the front of the queue.
//
// This is what lets a request be issued from inside another one: a handler
// that runs while a main-thread request pumps the event loop waiting for its
// reply (a timer, an advise, on wxMSW any CallAfter()) can issue its own
// request, which queues behind the outer one; the inner wait then reads both
// replies from the socket, delivering the outer one to its entry, and when
// the outer wait regains control it finds its reply already there.
bool wxTCPEventHandler::SendAndGetReply(wxIPCMessageBase& send_msg,
                                        IPCCode expected_code,
                                        wxSocketBase* socket,
                                        wxIPCMessageBase** return_msgptr)
{
    if (wxThread::IsMain())
        return SendAndGetReply_MainThread(send_msg, expected_code, socket, return_msgptr);
    else
        return SendAndGetReply_WorkerThread(send_msg, expected_code, socket, return_msgptr);
}

bool wxTCPEventHandler::SendAndRegister(wxIPCMessageBase& send_msg,
                                        wxSocketBase* socket,
                                        const PendingReplyPtr& entry)
{
    bool ok = false;
    RunOnMainThread( [&] {
        // Queue the entry and write the message under the same lock, so that
        // the order of the queue is the order of the messages on the wire
        // whichever threads write to this connection.
        wxCRIT_SECT_LOCKER(lock, gs_critical_io);

        ReceiveState* const state = GetReceiveState(socket);
        if ( !state )
            return;     // not a connection socket (any more)

        state->pending.push_back(entry);

        ok = send_msg.WriteIPCCode() && send_msg.DataToSocket();
        if ( !ok )
        {
            // No (complete) message went out, so no reply will come back. The
            // write may have pumped the event loop, so look the state up again
            // rather than trusting the pointer from before it.
            if ( ReceiveState* const st = GetReceiveState(socket) )
            {
                auto& pending = st->pending;
                pending.erase(std::remove(pending.begin(), pending.end(), entry),
                              pending.end());
            }
        }
    });

    return ok;
}

wxTCPEventHandler::PendingReply::~PendingReply()
{
    delete reply;
}

bool wxTCPEventHandler::TakeReply(const PendingReplyPtr& entry,
                                  IPCCode expected_code,
                                  wxIPCMessageBase** return_msgptr)
{
    wxIPCMessageBase* reply = nullptr;
    {
        wxMutexLocker lock(m_replyMutex);

        if ( !entry->done )
        {
            // Timed out. The entry stays queued on the connection so that the
            // reply, if it still comes, is consumed and dropped rather than
            // taken for the reply to the next message.
            entry->abandoned = true;
            return false;
        }

        reply = entry->reply;
        entry->reply = nullptr;
    }

    if ( !reply )
        return false;   // failed (IPC_FAIL received or connection lost)

    if ( reply->GetIPCCode() != expected_code )
    {
        // Cannot happen, DeliverReply() checked it; don't hand back garbage.
        delete reply;
        return false;
    }

    if ( return_msgptr )
        *return_msgptr = reply;
    else
        delete reply;

    return true;
}

// Write the message, then read and dispatch everything that arrives on the
// connection until our reply has been dispatched to us or the timeout expires.
//
// Waiting for the socket pumps the event loop, so other handlers run inside
// this call, including ones which may call this function again for the same
// connection (see SendAndGetReply()). That is fine: the bytes are read into
// the buffer without waiting and dispatched a whole message at a time, so a
// nested call finds the connection in a consistent state and so do we when
// it returns, with our reply delivered to our entry if it was among the bytes
// the nested call read.
bool wxTCPEventHandler::SendAndGetReply_MainThread(wxIPCMessageBase& send_msg,
                                                   IPCCode expected_code,
                                                   wxSocketBase* socket,
                                                   wxIPCMessageBase** return_msgptr)
{
    if ( m_mainThreadRequests >= wxIPCMaxNestedRequests )
    {
        wxFAIL_MSG("IPC requests nested too deeply: a handler issues a request "
                   "every time it runs while an earlier one waits for its reply?");
        return false;
    }

    class DepthGuard
    {
    public:
        explicit DepthGuard(int& depth) : m_depth(depth) { m_depth++; }
        ~DepthGuard() { m_depth--; }
    private:
        int& m_depth;
    } depthGuard(m_mainThreadRequests);

    const PendingReplyPtr entry =
        std::make_shared<PendingReply>(expected_code, true /* main thread */);

    if ( !SendAndRegister(send_msg, socket, entry) )
        return false;

    wxStopWatch sw;
    for ( ;; )
    {
        // Read and dispatch what has arrived: the reply if it is there, and
        // anything in front of it (other replies, advises, ...) in order.
        DrainAndDispatch(socket);

        {
            wxMutexLocker lock(m_replyMutex);
            if ( entry->done )
                break;
        }

        const long remaining = wxIPCTimeout*1000 - sw.Time();
        if ( remaining <= 0 )
            break;

        wxTCPConnection* const connection = GetConnection(socket);
        if ( !connection || !connection->GetConnected() )
            break;

        // Wait for more bytes, pumping the event loop meanwhile. The wait
        // returns early when the socket signals input or when DeliverReply()
        // interrupts it because a nested dispatch completed a main-thread
        // entry, possibly ours; it is also kept short in case that wake-up is
        // missed, e.g. because the nested dispatch happened inside a wait of
        // its own which reset this socket's event flags. Going round the loop
        // one more time costs nothing.
        socket->WaitForRead(0, std::min(remaining, 100L));
    }

    return TakeReply(entry, expected_code, return_msgptr);
}

// worker thread
bool wxTCPEventHandler::SendAndGetReply_WorkerThread(wxIPCMessageBase& send_msg,
                                                     IPCCode expected_code,
                                                     wxSocketBase* socket,
                                                     wxIPCMessageBase** return_msgptr)
{
    const PendingReplyPtr entry =
        std::make_shared<PendingReply>(expected_code, false /* worker */);

    // The write is marshalled to the main thread, which also queues the entry
    // (and must not be blocked by us holding m_replyMutex meanwhile: it needs
    // it to deliver replies).
    if ( !SendAndRegister(send_msg, socket, entry) )
        return false;

    {
        wxMutexLocker lock(m_replyMutex);

        // wxCondition requires its mutex be held around WaitTimeout() (the wait
        // releases it while blocked and re-acquires it before returning). The
        // predicate guards against a reply delivered before we got here and
        // against wake-ups meant for other waiters: m_replyCond is shared.
        wxStopWatch sw;
        while ( !entry->done )
        {
            const long remaining = wxIPCTimeout*1000 - sw.Time();
            if ( remaining <= 0 )
                break;

            m_replyCond.WaitTimeout(remaining);
        }
    }

    return TakeReply(entry, expected_code, return_msgptr);
}

bool wxTCPEventHandler::DeliverReply(wxSocketBase* socket,
                                     ReceiveState& state,
                                     wxIPCMessageBase* msg)
{
    if ( state.pending.empty() )
        return false;

    const IPCCode code = msg->GetIPCCode();
    const PendingReplyPtr head = state.pending.front();

    // The peer answers a message with a message of the same code, or with
    // IPC_FAIL. Anything else is not a reply (an advise arriving while a
    // request is pending, a request from the peer, ...).
    if ( code != head->expected && code != IPC_FAIL )
        return false;

    state.pending.pop_front();

    if ( code == IPC_FAIL )
    {
        wxLogDebug("IPC_FAIL received in reply: " +
                   wxIPCMessageCast<wxIPCMessageFail>(msg)->GetItem());
    }

    {
        wxMutexLocker lock(m_replyMutex);

        head->done = true;
        if ( code == IPC_FAIL )
        {
            head->failed = true;
            delete msg;
        }
        else if ( head->abandoned )
        {
            delete msg;     // the waiter gave up on it
        }
        else
        {
            head->reply = msg;
        }
    }

    if ( head->mainThread )
    {
        // The main thread waits in WaitForRead(): make that return so it sees
        // its entry is done, even if the bytes were consumed by a nested
        // dispatch rather than by its own.
        socket->InterruptWait();
    }
    else
    {
        m_replyCond.Broadcast();
    }

    return true;
}

void wxTCPEventHandler::FailPendingReplies(wxSocketBase* socket,
                                           ReceiveState& state)
{
    if ( state.pending.empty() )
        return;

    {
        wxMutexLocker lock(m_replyMutex);
        for ( const PendingReplyPtr& entry : state.pending )
        {
            entry->done = true;
            entry->failed = true;
        }
    }

    state.pending.clear();

    m_replyCond.Broadcast();
    socket->InterruptWait();
}

// Create a wxIPCMessage object from the given code. Caller is responsible for
// deleting the message when done.
wxIPCMessageBase* wxTCPEventHandler::GetIPCMessageFromCode(IPCCode code, wxSocketBase* socket)
{
    switch ( code )
    {
    case IPC_EXECUTE:
        return new wxIPCMessageExecute(socket);

    case IPC_REQUEST:
        return new wxIPCMessageRequest(socket);

    case IPC_POKE:
        return new wxIPCMessagePoke(socket);

    case IPC_ADVISE_START:
        return new wxIPCMessageAdviseStart(socket);

    case IPC_ADVISE:
        return new wxIPCMessageAdvise(socket);

    case IPC_ADVISE_STOP:
        return new wxIPCMessageAdviseStop(socket);

    case IPC_REQUEST_REPLY:
        return new wxIPCMessageRequestReply(socket);

    case IPC_FAIL:
        return new wxIPCMessageFail(socket);

    case IPC_CONNECT:
        return new wxIPCMessageConnect(socket);

    case IPC_DISCONNECT:
        return new wxIPCMessageDisconnect(socket);

    default:
        return new wxIPCMessageNull(socket);
    }
}

void wxTCPEventHandler::SendFailMessage(const wxString& reason, wxSocketBase* socket)
{
    wxIPCMessageFail msg(socket, reason);

    if (!WriteMessageToSocket(msg) )
        wxLogDebug("Failed to send IPC_FAIL message: " + reason);
}

void wxTCPEventHandler::HandleDisconnect(wxTCPConnection *connection)
{
    // Connection was closed (either gracefully or not): destroy everything.
    // Unregistering fails the replies still expected on it, if any.
    UnregisterConnectionSocket(connection->m_sock);
    connection->m_sock->Notify(false);
    connection->m_sock->Close();

    // Don't leave references to this soon-to-be-dangling connection in the
    // socket as it won't be destroyed immediately as its destruction will be
    // delayed in case there are more events pending for it
    connection->m_sock->SetClientData(nullptr);

    connection->SetConnected(false);
    connection->OnDisconnect();
}

// Reads a single message from a socket that is not a connection socket yet,
// waiting for it. Returns wxIPCMessageNull if none arrived within the socket's
// timeout. The returned message must be freed by the caller.
wxIPCMessageBase* wxTCPEventHandler::ReadMessageBlocking(wxSocketBase* socket)
{
    wxIPCMessageBase* msg = nullptr;

    RunOnMainThread( [&] {
        ReceiveState state;
        wxStopWatch sw;

        for ( ;; )
        {
            const bool lost = !Drain(socket, state);

            if ( ParseMessage(state, socket, msg) != Parse_NeedMore )
                break;

            if ( lost )
                break;

            const long remaining = socket->GetTimeout()*1000 - sw.Time();
            if ( remaining <= 0 )
                break;

            socket->WaitForRead(0, remaining);
        }

        // Give back what may have arrived after the message, so that it is
        // read again once the socket is a connection socket.
        if ( state.Available() )
            socket->Unread(state.Data(), state.Available());
    });

    if ( msg )
        return msg;

    return new wxIPCMessageNull(socket);
};

// Writes this message object to the socket.
bool wxTCPEventHandler::WriteMessageToSocket(wxIPCMessageBase& msg)
{
    bool ok_write = false;
    RunOnMainThread( [&] {
        // Serialize all socket I/O: wxSocketBase is not safe for concurrent use
        // from multiple threads on the same connection. The lock is taken here,
        // inside the marshalled work, so it is only ever held on the thread that
        // actually performs the I/O.
        wxCRIT_SECT_LOCKER(lock, gs_critical_io);
        ok_write = msg.WriteIPCCode() && msg.DataToSocket();
    });

    return ok_write;
};

void wxTCPEventHandler::ReceiveState::Compact()
{
    if ( rd == wr )
    {
        rd = wr = 0;

        // Don't keep a buffer sized for the biggest message ever received.
        if ( buf.size() > 16*wxIPCReadChunk )
            std::vector<char>().swap(buf);
    }
    else if ( rd >= buf.size()/2 )
    {
        memmove(buf.data(), buf.data() + rd, wr - rd);
        wr -= rd;
        rd = 0;
    }
}

bool wxTCPEventHandler::Drain(wxSocketBase* socket, ReceiveState& state)
{
    wxASSERT_MSG( wxThread::IsMain(), "IPC sockets are read on the main thread" );

    // See WriteMessageToSocket().
    wxCRIT_SECT_LOCKER(lock, gs_critical_io);

    if ( !socket->IsOk() )
        return false;

    // Read with wxSOCKET_NOWAIT_READ: each Read() returns at once with what
    // the socket had, and returns nothing when it would have to wait. Reading
    // until that point is also what keeps the input notifications coming on
    // wxMSW, where the next one is only generated once everything already
    // received has been read.
    const wxSocketFlags flags = socket->GetFlags();
    socket->SetFlags((flags & ~wxSOCKET_WAITALL_READ) | wxSOCKET_NOWAIT_READ);

    bool ok = true;
    for ( ;; )
    {
        state.Compact();

        if ( state.buf.size() - state.wr < wxIPCReadChunk )
            state.buf.resize(std::max(2*state.buf.size(), state.wr + wxIPCReadChunk));

        const size_t room = state.buf.size() - state.wr;
        socket->Read(state.buf.data() + state.wr, room);

        const wxUint32 n = socket->LastReadCount();
        state.wr += n;

        if ( n == 0 )
        {
            // Either nothing is available right now, or the peer closed the
            // connection (which Read() reports as an error) or it failed.
            if ( socket->Error() || socket->IsClosed() )
                ok = false;
            break;
        }

        if ( n < room )
            break;      // that was all there is for now
    }

    socket->SetFlags(flags);

    return ok;
}

wxTCPEventHandler::ParseResult
wxTCPEventHandler::ParseMessage(ReceiveState& state,
                                wxSocketBase* socket,
                                wxIPCMessageBase*& msg)
{
    msg = nullptr;

    wxIPCMessageReader in(state.Data(), state.Available());

    wxUint32 code_with_header;
    if ( !in.Read32(code_with_header) )
        return Parse_NeedMore;

    if ( (code_with_header & 0xFFFFFF00) != IPCCodeHeader )
        return Parse_Corrupt;

    const IPCCode code = static_cast<IPCCode>(code_with_header & 0xFF);

    std::unique_ptr<wxIPCMessageBase> m(GetIPCMessageFromCode(code, socket));
    if ( !m->IsOk() )
        return Parse_Corrupt;   // not a code the peer could have sent

    if ( !m->ReadFrom(in) )
        return Parse_NeedMore;

    state.Consume(in.GetConsumed());
    msg = m.release();
    return Parse_Complete;
}

// Read whatever the socket has and dispatch the complete messages in the
// buffer, in order.
//
// This may be re-entered for the same connection: a handler run from
// ProcessMessage() may pump the event loop (a dialog, a request of its own,
// see SendAndGetReply_MainThread()), and the event loop can come back here.
// That is safe because every message is removed from the buffer before its
// handler runs and nothing here keeps a pointer into the buffer, or to the
// connection, across a handler call: the inner call just carries on from where
// the outer one was, and the outer one carries on from where the inner one
// stopped, or finds that the connection is gone.
void wxTCPEventHandler::DrainAndDispatch(wxSocketBase* socket)
{
    ReceiveState* state = GetReceiveState(socket);
    if ( !state )
        return;

    wxTCPConnection* connection = GetConnection(socket);
    if ( !connection )
    {
        // The connection object is being destroyed.
        FailPendingReplies(socket, *state);
        return;
    }

    if ( !connection->GetConnected() )
        return;     // disconnected by our side, the socket is closed

    const bool lost = !Drain(socket, *state);

    bool corrupt = false;
    for ( ;; )
    {
        wxIPCMessageBase* msg = nullptr;
        const ParseResult res = ParseMessage(*state, socket, msg);
        if ( res == Parse_NeedMore )
            break;

        if ( res == Parse_Corrupt )
        {
            corrupt = true;
            break;
        }

        if ( DeliverReply(socket, *state, msg) )
            continue;   // msg ownership handed to the waiting entry

        wxIPCMessageBaseLocker lock_msg(msg);
        ProcessMessage(msg, socket);

        // The handler may have done anything, including deleting the
        // connection: start over from the registry.
        state = GetReceiveState(socket);
        if ( !state )
            return;

        connection = GetConnection(socket);
        if ( !connection || !connection->GetConnected() )
            return;
    }

    if ( corrupt )
    {
        // The stream is misaligned, nothing can be made of it any more.
        wxLogDebug("IPC data misaligned (no message header where one was "
                   "expected), closing the connection");
        HandleDisconnect(connection);
    }
    else if ( lost )
    {
        HandleDisconnect(connection);
    }
}

wxTCPConnection* wxTCPEventHandler::GetConnection(wxSocketBase* socket)
{
    if ( !socket || !socket->IsOk() )
        return nullptr;

    return static_cast<wxTCPConnection *>(socket->GetClientData());
}

wxTCPEventHandler::ReceiveState*
wxTCPEventHandler::GetReceiveState(wxSocketBase* socket)
{
    wxCRIT_SECT_LOCKER(lock, m_cs_connection_sockets);
    const auto it = m_connections.find(socket);
    return it == m_connections.end() ? nullptr : it->second.get();
}

void wxTCPEventHandler::RegisterConnectionSocket(wxSocketBase* socket)
{
    wxCRIT_SECT_LOCKER(lock, m_cs_connection_sockets);
    m_connections[socket] = std::make_unique<ReceiveState>();
}

void wxTCPEventHandler::UnregisterConnectionSocket(wxSocketBase* socket)
{
    wxCRIT_SECT_LOCKER(lock, m_cs_connection_sockets);
    const auto it = m_connections.find(socket);
    if ( it == m_connections.end() )
        return;

    // Whoever still waits for a reply on this connection is not getting one.
    FailPendingReplies(socket, *it->second);

    m_connections.erase(it);
}

bool wxTCPEventHandler::IsConnectionSocket(wxSocketBase* socket)
{
    wxCRIT_SECT_LOCKER(lock, m_cs_connection_sockets);
    return m_connections.find(socket) != m_connections.end();
}

#endif // wxUSE_SOCKETS && wxUSE_IPC

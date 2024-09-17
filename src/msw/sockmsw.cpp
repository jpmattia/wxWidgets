/////////////////////////////////////////////////////////////////////////////
// Name:       src/msw/sockmsw.cpp
// Purpose:    MSW-specific socket code
// Authors:    Guilhem Lavaux, Guillermo Rodriguez Garcia
// Created:    April 1997
// Copyright:  (C) 1999-1997, Guilhem Lavaux
//             (C) 1999-2000, Guillermo Rodriguez Garcia
//             (C) 2008 Vadim Zeitlin
// Licence:    wxWindows licence
/////////////////////////////////////////////////////////////////////////////


// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"


#if wxUSE_SOCKETS

/* including rasasync.h (included from windows.h itself included from
 * wx/setup.h and/or winsock.h results in this warning for
 * RPCNOTIFICATION_ROUTINE
 */
#ifdef _MSC_VER
#   pragma warning(disable:4115) /* named type definition in parentheses */
#endif

#include "wx/private/socket.h"
#include "wx/msw/private.h"     // for wxGetInstance()
#include "wx/private/fd.h"
#include "wx/apptrait.h"
#include "wx/thread.h"
#include "wx/link.h"

#ifdef _MSC_VER
#  pragma warning(default:4115) /* named type definition in parentheses */
#endif

#include "wx/msw/private/hiddenwin.h"

#define CLASSNAME  TEXT("_wxSocket_Internal_Window_Class")

/* Maximum number of different wxSocket objects at a given time.
 * This value can be modified at will, but it CANNOT be greater
 * than (0x7FFF - WM_USER + 1)
 */
#define MAXSOCKETS 1024

#if (MAXSOCKETS > (0x7FFF - WM_USER + 1))
#error "MAXSOCKETS is too big!"
#endif

LRESULT CALLBACK wxSocket_Internal_WinProc(HWND, UINT, WPARAM, LPARAM);

/* Global variables */

static HWND hWin;
wxCRIT_SECT_DECLARE_MEMBER(gs_critical);
static wxSocketImplMSW *socketList[MAXSOCKETS];
static int firstAvailable;

// ----------------------------------------------------------------------------
// MSW implementation of wxSocketManager
// ----------------------------------------------------------------------------

class wxSocketMSWManager : public wxSocketManager
{
public:
    virtual bool OnInit() override;
    virtual void OnExit() override;

    virtual wxSocketImpl *CreateSocket(wxSocketBase& wxsocket) override
    {
        return new wxSocketImplMSW(wxsocket);
    }
    virtual void Install_Callback(wxSocketImpl *socket,
                                  wxSocketNotify event = wxSOCKET_LOST) override;
    virtual void Uninstall_Callback(wxSocketImpl *socket,
                                    wxSocketNotify event = wxSOCKET_LOST) override;
};

bool wxSocketMSWManager::OnInit()
{
  LPCTSTR pclassname = nullptr;
  int i;

  /* Create internal window for event notifications */
  hWin = wxCreateHiddenWindow(&pclassname, CLASSNAME, wxSocket_Internal_WinProc);
  if (!hWin)
      return false;

  /* Initialize socket list */
  for (i = 0; i < MAXSOCKETS; i++)
  {
    socketList[i] = nullptr;
  }
  firstAvailable = 0;

  // finally initialize WinSock
  WSADATA wsaData;
  return WSAStartup((1 << 8) | 1, &wsaData) == 0;
}

void wxSocketMSWManager::OnExit()
{
  /* Destroy internal window */
  DestroyWindow(hWin);
  UnregisterClass(CLASSNAME, wxGetInstance());

  WSACleanup();
}

/* Per-socket GUI initialization / cleanup */

wxSocketImplMSW::wxSocketImplMSW(wxSocketBase& wxsocket)
    : wxSocketImpl(wxsocket)
{
  /* Allocate a new message number for this socket */
  wxCRIT_SECT_LOCKER(lock, gs_critical);

  int i = firstAvailable;
  while (socketList[i] != nullptr)
  {
    i = (i + 1) % MAXSOCKETS;

    if (i == firstAvailable)    /* abort! */
    {
      m_msgnumber = 0; // invalid
      return;
    }
  }
  socketList[i] = this;
  firstAvailable = (i + 1) % MAXSOCKETS;
  m_msgnumber = (i + WM_USER);
}

wxSocketImplMSW::~wxSocketImplMSW()
{
  /* Remove the socket from the list */
  wxCRIT_SECT_LOCKER(lock, gs_critical);

  if ( m_msgnumber )
  {
      // we need to remove any pending messages for this socket to avoid having
      // them sent to a new socket which could reuse the same message number as
      // soon as we destroy this one
      MSG msg;
      while ( ::PeekMessage(&msg, hWin, m_msgnumber, m_msgnumber, PM_REMOVE) )
          ;

      socketList[m_msgnumber - WM_USER] = nullptr;
  }
  //else: the socket has never been created successfully
}

/* Windows proc for asynchronous event handling */

LRESULT CALLBACK wxSocket_Internal_WinProc(HWND hWnd,
                                           UINT uMsg,
                                           WPARAM wParam,
                                           LPARAM lParam)
{
    // wxLogMessage("wxSocket_Internal_WinProc start");

    if ( uMsg < WM_USER || uMsg > (WM_USER + MAXSOCKETS - 1))
    {
        // wxLogMessage("wxSocket_Internal_WinProc DefWindowProc"); // how?
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    wxSocketImplMSW *socket;
    wxSocketNotify event = (wxSocketNotify)-1;
    {
        wxCRIT_SECT_LOCKER(lock, gs_critical);

        socket = socketList[(uMsg - WM_USER)];
        if ( !socket )
        {
            wxLogMessage("wxSocket_Internal_WinProc no socket!");
            return 0;
        }

        // the socket may be already closed but we could still receive
        // notifications for it sent (asynchronously) before it got closed
        if ( socket->m_fd == INVALID_SOCKET )
        {
            wxLogMessage("wxSocket_Internal_WinProc invalid socket!");
            return 0;
        }

        wxASSERT_MSG( socket->m_fd == (SOCKET)wParam,
                      "mismatch between message and socket?" );

        int event_type = WSAGETSELECTEVENT(lParam);
        int cnt=0;

        if (event_type & FD_READ) cnt++;
        if (event_type & FD_WRITE) cnt++;
        if (event_type & FD_ACCEPT) cnt++;
        if (event_type & FD_CONNECT) cnt++;
        if (event_type & FD_CLOSE) cnt++;

        if (event_type & FD_OOB) {
            cnt++;
            wxLogMessage("FD_OOB received");
        }

        if (event_type & FD_QOS) {
            cnt++;
            wxLogMessage("FD_OOB received");
        }

        if (event_type & FD_GROUP_QOS) {
            cnt++;
            wxLogMessage("FD_GROUP_OOB received");
        }

        if (event_type & FD_ROUTING_INTERFACE_CHANGE) {
            cnt++;
            wxLogMessage("FD_GROUP_QOSFD_ROUTING_INTERFACE_CHANGE received");
        }

        if (event_type & FD_ADDRESS_LIST_CHANGE) {
            cnt++;
            wxLogMessage("FD_ADDRESS_LIST_CHANGE received");
        }

        if (cnt > 1)
            wxLogMessage("Notification count = %d", cnt);


        switch ( event_type )
        {
            case FD_READ:
                // We may get a FD_READ notification even when there is no data
                // to read on the socket, in particular this happens on socket
                // creation when we seem to always get FD_CONNECT, FD_WRITE and
                // FD_READ notifications all at once (but it doesn't happen
                // only then). Ignore such dummy notifications.
                {
                    fd_set fds;
                    wxTimeVal_t tv = { 0, 0 };

                    wxFD_ZERO(&fds);
                    wxFD_SET(socket->m_fd, &fds);

                    if ( 0 &&  select(socket->m_fd + 1, &fds, nullptr, nullptr, &tv) != 1 )
                    {
                        wxLogMessage("wxSocket_Internal_WinProc no data condition");
                        return 0;
                    }
                }

                event = wxSOCKET_INPUT;
                break;

            case FD_WRITE:
                event = wxSOCKET_OUTPUT;
                break;

            case FD_ACCEPT:
                event = wxSOCKET_CONNECTION;
                break;

            case FD_CONNECT:
                event = WSAGETSELECTERROR(lParam) ? wxSOCKET_LOST
                                                  : wxSOCKET_CONNECTION;
                break;

            case FD_CLOSE:
                event = wxSOCKET_LOST;
                break;

            default:
                wxFAIL_MSG( "unexpected socket notification" );
                return 0;
        }
    } // unlock gs_critical

    socket->NotifyOnStateChange(event);

    return 0;
}

/*
 *  Enable all event notifications; we need to be notified of all
 *  events for internal processing, but we will only notify users
 *  when an appropriate callback function has been installed.
 */
void wxSocketMSWManager::Install_Callback(wxSocketImpl *socket_,
                                         wxSocketNotify WXUNUSED(event))
{
    wxSocketImplMSW * const socket = static_cast<wxSocketImplMSW *>(socket_);

    /* We could probably just subscribe to all events regardless
     * of the socket type, but MS recommends to do it this way.
     */
    long lEvent = socket->m_server?
                  FD_ACCEPT : (FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
    WSAAsyncSelect(socket->m_fd, hWin, socket->m_msgnumber, lEvent);
}

/*
 *  Disable event notifications (used when shutting down the socket)
 */
void wxSocketMSWManager::Uninstall_Callback(wxSocketImpl *socket_,
                                            wxSocketNotify WXUNUSED(event))
{
    wxSocketImplMSW * const socket = static_cast<wxSocketImplMSW *>(socket_);

    WSAAsyncSelect(socket->m_fd, hWin, socket->m_msgnumber, 0);
}

// set the wxBase variable to point to our wxSocketManager implementation
//
// see comments in wx/apptrait.h for the explanation of why do we do it
// like this
static struct ManagerSetter
{
    ManagerSetter()
    {
        static wxSocketMSWManager s_manager;
        wxAppTraits::SetDefaultSocketManager(&s_manager);
    }
} gs_managerSetter;

// see the relative linker macro in socket.cpp
wxFORCE_LINK_THIS_MODULE( mswsocket );

// ============================================================================
// wxSocketImpl implementation
// ============================================================================

void wxSocketImplMSW::DoClose()
{
    wxSocketManager::Get()->Uninstall_Callback(this);

    wxCloseSocket(m_fd);
}

wxSocketError wxSocketImplMSW::GetLastError() const
{
    int errnum = WSAGetLastError();

    // wxLogMessage("wxSocketImplMSW::GetLastError errnum = %d", errnum);

    switch ( errnum )
    {
        case 0:
            return wxSOCKET_NOERROR;

        case WSAENOTSOCK:
            return wxSOCKET_INVSOCK;

        case 5:
        case 10053:
        case WSAEWOULDBLOCK:
            return wxSOCKET_WOULDBLOCK;

        default:
            return wxSOCKET_IOERR;
    }
}

#endif  // wxUSE_SOCKETS

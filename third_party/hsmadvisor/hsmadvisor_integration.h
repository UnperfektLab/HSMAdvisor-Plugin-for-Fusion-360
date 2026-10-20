/*
 * hsmadvisor_integration.h - native client for the HSMAdvisor integration server.
 *
 * HSMAdvisorIntegration.dll lets C, C++ and other native applications use the installed
 * HSMAdvisor as a speeds and feeds server, without .NET. It is the native counterpart of
 * HSMAdvisor.Integration.dll (HSMAdvisorClient): it finds HSMAdvisor.exe, starts it in
 * integration mode when needed, checks its version and exchanges one JSON message per request
 * over the per-user named pipe. See docs/cam-integration-protocol.md for the messages.
 *
 * Conventions:
 *   - Every string is UTF-8 and NUL-terminated.
 *   - Strings returned by the DLL are allocated by it; release them with hsma_free().
 *   - Payloads are JSON text exactly as on the wire (e.g. {"calculation":{...}}).
 *   - hsma_request() blocks; call it from a worker thread when your UI must stay responsive.
 *     A client may be used from several threads at once.
 *
 * Most hosts load the DLL with hsmadvisor_integration_loader.h (from the HSMAdvisor installation,
 * falling back to a copy shipped with the host) and call it through the hsma_api table. Hosts that
 * link the import library directly can call the functions declared below.
 *
 * (c) Eldar Gerfanov, HSMAdvisor Inc. You may distribute this header and HSMAdvisorIntegration.dll
 * with your integration.
 */
#ifndef HSMADVISOR_INTEGRATION_H
#define HSMADVISOR_INTEGRATION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this C API. Bumped when functions are added; existing functions never change. */
#define HSMA_API_VERSION 1

#define HSMA_CALL __cdecl

#if defined(HSMA_BUILDING_DLL)
#define HSMA_API __declspec(dllexport)
#else
#define HSMA_API __declspec(dllimport)
#endif

/* hsma_request() results. */
#define HSMA_OK        0  /* *out_payload holds the response payload                              */
#define HSMA_CANCELLED 1  /* the user cancelled the HSMAdvisor dialog, or hsma_cancel_signal() ran */
#define HSMA_ERROR     2  /* *out_error holds {"code":"...","message":"..."}                      */
#define HSMA_BUSY      3  /* another HSMAdvisor dialog is open; *out_error says so                */

/*
 * Error codes (the "code" in *out_error). Server codes: unknown_message, bad_request,
 * unsupported_protocol, unsupported_tool_type, not_found, internal, busy.
 * Client codes, raised by this library:
 */
#define HSMA_ERR_NOT_INSTALLED   "not_installed"    /* HSMAdvisor.exe was not found                 */
#define HSMA_ERR_NOT_RUNNING     "not_running"      /* only from hsma_request_no_launch()           */
#define HSMA_ERR_VERSION_TOO_OLD "version_too_old"  /* older than hsma_client_set_minimum_version() */
#define HSMA_ERR_TIMEOUT         "timeout"          /* HSMAdvisor did not start listening in time   */
#define HSMA_ERR_IO              "io"               /* the pipe failed or the response was invalid  */
#define HSMA_ERR_BAD_ARGUMENT    "bad_argument"     /* e.g. payload_json is not valid JSON          */

/* HSMAdvisor download page, for users who need to install or update it. */
#define HSMA_DOWNLOAD_URL "https://hsmadvisor.com/view_downloads"

typedef struct hsma_client hsma_client;
typedef struct hsma_cancel hsma_cancel;
typedef void (HSMA_CALL *hsma_launching_fn)(void* user);

#ifndef HSMA_NO_PROTOTYPES

/* HSMA_API_VERSION of the loaded DLL. */
HSMA_API int HSMA_CALL hsma_api_version(void);

/*
 * Creates a client. Keep one for the life of your application: it remembers which HSMAdvisor
 * process it already version-checked and which one it started. name/version identify your
 * integration to HSMAdvisor (either may be NULL).
 */
HSMA_API hsma_client* HSMA_CALL hsma_client_create(const char* name, const char* version);
HSMA_API void HSMA_CALL hsma_client_destroy(hsma_client* client);

/* Optional "action" sent with every request (e.g. the CAM command that triggered it). */
HSMA_API void HSMA_CALL hsma_client_set_action(hsma_client* client, const char* action);

/*
 * Oldest HSMAdvisor this integration works with, e.g. "3.2.0" (NULL = any). Checked against the
 * executable's file version before starting it and against the running instance once per
 * process. Only the given components are compared: "3.2" accepts 3.2.0.1234.
 * An older HSMAdvisor fails every request with HSMA_ERR_VERSION_TOO_OLD.
 * Returns 0 when the version string is invalid.
 */
HSMA_API int HSMA_CALL hsma_client_set_minimum_version(hsma_client* client, const char* version);

/* HSMAdvisor.exe to start when none is running (NULL = find the installed one). */
HSMA_API void HSMA_CALL hsma_client_set_executable_path(hsma_client* client, const char* exe);

/* How long to wait for a freshly started HSMAdvisor to start listening. Default 120000 ms. */
HSMA_API void HSMA_CALL hsma_client_set_launch_timeout_ms(hsma_client* client, unsigned int ms);

/* Called on the requesting thread right before HSMAdvisor is started ("Starting HSMAdvisor..."). */
HSMA_API void HSMA_CALL hsma_client_set_launching_callback(hsma_client* client, hsma_launching_fn fn, void* user);

/* Pipe to talk to (NULL = the current user's and session's, see hsma_get_pipe_name). For tests. */
HSMA_API void HSMA_CALL hsma_client_set_pipe_name(hsma_client* client, const char* pipe_name);

/* Version of the HSMAdvisor last checked, e.g. "3.2.0.4321"; NULL before the first check. */
HSMA_API char* HSMA_CALL hsma_client_server_version(hsma_client* client);

/*
 * Cancellation token. Signal it from any thread to abort the request it was passed to: the pipe
 * is closed, HSMAdvisor closes the dialog it opened for that request, and hsma_request() returns
 * HSMA_CANCELLED. A signalled token stays signalled; create a new one per request.
 */
HSMA_API hsma_cancel* HSMA_CALL hsma_cancel_create(void);
HSMA_API void HSMA_CALL hsma_cancel_signal(hsma_cancel* cancel);
HSMA_API void HSMA_CALL hsma_cancel_destroy(hsma_cancel* cancel);

/*
 * Sends one request and waits for the response, starting HSMAdvisor first when it is not running.
 *   type          message type, e.g. "Hello", "Calculate", "EditCalculation"
 *   payload_json  the message's request object, or NULL (or "") for none
 *   owner_window  HWND that owns HSMAdvisor's dialog (interactive messages), or 0
 *   cancel        NULL, or a token that aborts the request
 *   out_payload   receives the payload JSON on HSMA_OK ("{}" when the response has none), else NULL
 *   out_error     receives {"code","message"} JSON on HSMA_ERROR/HSMA_BUSY, else NULL (may be NULL)
 * Returns HSMA_OK, HSMA_CANCELLED, HSMA_ERROR or HSMA_BUSY.
 */
HSMA_API int HSMA_CALL hsma_request(hsma_client* client, const char* type, const char* payload_json,
                                     long long owner_window, hsma_cancel* cancel,
                                     char** out_payload, char** out_error);

/* hsma_request() that fails with HSMA_ERR_NOT_RUNNING instead of starting HSMAdvisor. */
HSMA_API int HSMA_CALL hsma_request_no_launch(hsma_client* client, const char* type, const char* payload_json,
                                               long long owner_window, hsma_cancel* cancel,
                                               char** out_payload, char** out_error);

/* 1 when an HSMAdvisor instance is listening for this client's pipe. Never starts HSMAdvisor. */
HSMA_API int HSMA_CALL hsma_is_running(hsma_client* client);

/*
 * Installed HSMAdvisor.exe, or NULL. Order: the HSMADVISOR_EXE environment variable,
 * Software\HSMAdvisor\ExePath (HKCU, HKLM; 64- and 32-bit views), the uninstall entry, then
 * Program Files\HSMAdvisor.
 */
HSMA_API char* HSMA_CALL hsma_find_executable(void);

/* The current user's and session's pipe name, e.g. "HSMAdvisor.Integration.v1.1.CORP.jdoe". */
HSMA_API char* HSMA_CALL hsma_get_pipe_name(void);

/* Releases a string returned by this library. NULL is ignored. */
HSMA_API void HSMA_CALL hsma_free(void* p);

#endif /* HSMA_NO_PROTOTYPES */

/* Function pointer types, for GetProcAddress (see hsmadvisor_integration_loader.h). */
typedef int          (HSMA_CALL *hsma_pfn_api_version)(void);
typedef hsma_client* (HSMA_CALL *hsma_pfn_client_create)(const char*, const char*);
typedef void         (HSMA_CALL *hsma_pfn_client_destroy)(hsma_client*);
typedef void         (HSMA_CALL *hsma_pfn_client_set_action)(hsma_client*, const char*);
typedef int          (HSMA_CALL *hsma_pfn_client_set_minimum_version)(hsma_client*, const char*);
typedef void         (HSMA_CALL *hsma_pfn_client_set_executable_path)(hsma_client*, const char*);
typedef void         (HSMA_CALL *hsma_pfn_client_set_launch_timeout_ms)(hsma_client*, unsigned int);
typedef void         (HSMA_CALL *hsma_pfn_client_set_launching_callback)(hsma_client*, hsma_launching_fn, void*);
typedef void         (HSMA_CALL *hsma_pfn_client_set_pipe_name)(hsma_client*, const char*);
typedef char*        (HSMA_CALL *hsma_pfn_client_server_version)(hsma_client*);
typedef hsma_cancel* (HSMA_CALL *hsma_pfn_cancel_create)(void);
typedef void         (HSMA_CALL *hsma_pfn_cancel_signal)(hsma_cancel*);
typedef void         (HSMA_CALL *hsma_pfn_cancel_destroy)(hsma_cancel*);
typedef int          (HSMA_CALL *hsma_pfn_request)(hsma_client*, const char*, const char*, long long,
                                                   hsma_cancel*, char**, char**);
typedef int          (HSMA_CALL *hsma_pfn_is_running)(hsma_client*);
typedef char*        (HSMA_CALL *hsma_pfn_find_executable)(void);
typedef char*        (HSMA_CALL *hsma_pfn_get_pipe_name)(void);
typedef void         (HSMA_CALL *hsma_pfn_free)(void*);

#ifdef __cplusplus
}
#endif

#endif /* HSMADVISOR_INTEGRATION_H */

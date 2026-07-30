// horizon_ipc.h — HIPC/CMIF, the way a Horizon guest asks for anything.
//
// Almost nothing on Horizon is a system call. svcSetHeapSize and svcCreateThread
// are, but reading a button, drawing a pixel or opening a file are all IPC: the
// guest fills the 0x100-byte command buffer at the head of its own TLS block,
// calls svcSendSyncRequest on a session handle, and reads the reply out of the
// same buffer. So this file is the other half of the guest-facing surface, and
// the SVC layer is the smaller half.
//
// The layering is Horizon's own, not an invention here:
//
//   svcConnectToNamedPort("sm:")  -> a session with the service manager. This
//        is the ONLY named port a real Horizon process may connect to; every
//        other service is reached by asking sm: for it. A guest connecting to
//        anything else is refused by name.
//   sm:  GetService("hid")        -> a session with that service, returned as
//        a move handle in the reply.
//   svcSendSyncRequest(session)   -> one command on that session, dispatched
//        here to the service's handler.
//
// ── the wire format ────────────────────────────────────────────────────────
//
// Two headers stacked. HIPC is the transport: a command type, descriptor
// counts, an optional handle descriptor, buffer descriptors, then a raw data
// section aligned to 16 bytes. CMIF is what rides inside the raw section: magic
// 'SFCI' going out, 'SFCO' coming back, with the command id at raw+8 and the
// Result at raw+8 of the reply.
//
// The reply layout -- including the deliberately over-reported raw word count
// -- is transcribed from build_result_response in
// Reference/horizon-linux/kernel/horizon/sys.c, which is itself adapted from
// yuzu's Response_Builder. That over-report is not a mistake being copied
// forward: the field is the size of the raw section including its mandatory
// padding, and both implementations round it the same way, so a guest parsing
// the reply expects exactly this.
//
// ── what is NOT here, and why that is the honest state ─────────────────────
//
// The transport is complete. The SERVICES are not: `sm:` is implemented,
// because it is the kernel-adjacent one and every guest needs it, and the
// service table exists and dispatches -- but there is no entry in it yet for
// hid, vi, nvdrv, fsp-srv or the rest. Each of those is a substantial
// subsystem in its own right, and MVII's real devices (/dev/fb0, /dev/input0,
// /dev/dac0) are what they would have to be built on.
//
// So a request for an unbacked service fails LOUDLY AND BY NAME, and does not
// return a plausible Result. Returning success from a service that did not run
// would leave the guest reading an uninitialised reply and failing somewhere
// else entirely, which is precisely the class of bug that makes a front-end
// unmaintainable. horizon-linux itself has a path that "stubs the response" for
// a session with no handler; that path is not copied here, on purpose.

#ifndef HORIZON_IPC_H
#define HORIZON_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "horizon_result.h"

#ifdef __cplusplus
extern "C" {
#endif

// Horizon's own Result module for IPC-layer failures.
#define HZN_RESULT_IPC_REMOTE_PROCESS_DEAD HZN_MAKE_RESULT(HZN_MODULE_HIPC, 301)
// sm: description 6 is "the requested service is not registered".
#define HZN_RESULT_SM_NOT_REGISTERED       HZN_MAKE_RESULT(HZN_MODULE_SM, 6)
#define HZN_RESULT_SM_INVALID_NAME         HZN_MAKE_RESULT(HZN_MODULE_SM, 7)

#define HZN_SERVICE_NAME_MAX 9   // 8 characters and a terminator

// One in-flight command, as handed to a service handler.
//
// `in_payload` points into the guest's own command buffer, past the 16-byte
// CMIF input header. `out_payload` points into the same buffer, past the
// 16-byte CMIF output header the dispatcher has already written -- a handler
// fills it and sets out_payload_size; it never writes the headers itself.
typedef struct horizon_ipc_context {
    const char* service;          // which session this arrived on
    uint32_t    command_id;

    const void* in_payload;
    size_t      in_payload_size;
    void*       out_payload;
    size_t      out_payload_capacity;
    size_t      out_payload_size;

    // Handles the client sent with the request.
    const uint32_t* in_copy_handles; size_t in_copy_handle_count;
    const uint32_t* in_move_handles; size_t in_move_handle_count;
    bool            has_client_pid;
    uint64_t        client_pid;

    // Handles the handler is sending back. Move transfers ownership to the
    // guest (a new session, an event); copy does not.
    uint32_t move_handles[4]; size_t move_handle_count;
    uint32_t copy_handles[4]; size_t copy_handle_count;
} horizon_ipc_context;

typedef horizon_result (*horizon_ipc_handler_fn)(horizon_ipc_context* ctx);

bool horizon_ipc_init(void);
void horizon_ipc_shutdown(void);

// Register a service under `name`, so sm:'s GetService can hand out sessions
// for it. Names are at most 8 characters, as Horizon encodes them in a u64.
//
// This is the seam a subsystem plugs into: implement the handler against MVII's
// real devices and register it here. Returns false, with the reason logged, on a
// bad name or a full table.
bool horizon_ipc_register_service(const char* name, horizon_ipc_handler_fn handler);

// svcConnectToNamedPort. Only "sm:" is accepted -- see the note above.
horizon_result horizon_ipc_connect_to_named_port(const char* name, uint32_t* out_handle);

// svcSendSyncRequest. Reads the calling thread's command buffer, dispatches, and
// writes the reply back into it.
horizon_result horizon_ipc_send_sync_request(uint32_t session_handle);

// Called by the kernel when a session handle is closed, so the session slot can
// be reused. Harmless for a payload that is not a session.
void horizon_ipc_release_session(void* payload);

#ifdef __cplusplus
}
#endif

#endif  // HORIZON_IPC_H

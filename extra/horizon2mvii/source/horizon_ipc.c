#include "horizon_ipc.h"

#include <string.h>

#include "horizon_kernel.h"
#include "vwine/vwine_log.h"

// ── the wire ───────────────────────────────────────────────────────────────

#define HIPC_BUFFER_WORDS (HZN_IPC_BUFFER_SIZE / sizeof(uint32_t))   // 0x40

#define HIPC_TYPE_CLOSE                2u
#define HIPC_TYPE_REQUEST              4u
#define HIPC_TYPE_REQUEST_WITH_CONTEXT 6u
#define HIPC_TYPE_TIPC_CLOSE           15u
#define HIPC_TYPE_TIPC_COMMAND_REGION  16u

#define CMIF_MAGIC_IN  0x49434653u   // 'SFCI'
#define CMIF_MAGIC_OUT 0x4F434653u   // 'SFCO'

#define CMIF_HEADER_WORDS 4          // magic, version, command/result, token

// A parsed request. Every pointer is into the guest's own command buffer.
typedef struct hipc_request {
    uint32_t type;
    bool     has_handle_desc;
    bool     send_pid;
    uint64_t pid;

    const uint32_t* copy_handles; size_t copy_handle_count;
    const uint32_t* move_handles; size_t move_handle_count;

    const uint32_t* raw;          // the 16-byte-aligned raw data section
    size_t          raw_words;    // words available from `raw` to the end
} hipc_request;

// ── sessions ───────────────────────────────────────────────────────────────

#define HZN_MAX_SESSIONS 32
#define HZN_MAX_SERVICES 32

typedef struct hzn_session {
    bool                   used;
    char                   name[HZN_SERVICE_NAME_MAX + 4];
    horizon_ipc_handler_fn handler;
} hzn_session;

typedef struct hzn_service {
    bool                   used;
    char                   name[HZN_SERVICE_NAME_MAX];
    horizon_ipc_handler_fn handler;
} hzn_service;

static hzn_session g_sessions[HZN_MAX_SESSIONS];
static hzn_service g_services[HZN_MAX_SERVICES];

static horizon_result sm_handler(horizon_ipc_context* ctx);

// ── parsing ────────────────────────────────────────────────────────────────

static bool hipc_parse(const uint32_t* buf, hipc_request* out)
{
    memset(out, 0, sizeof(*out));

    const uint32_t w0 = buf[0];
    const uint32_t w1 = buf[1];

    out->type = w0 & 0xFFFFu;
    const uint32_t num_x = (w0 >> 16) & 0xFu;
    const uint32_t num_a = (w0 >> 20) & 0xFu;
    const uint32_t num_b = (w0 >> 24) & 0xFu;
    const uint32_t num_w = (w0 >> 28) & 0xFu;
    const uint32_t recv_flags = (w1 >> 10) & 0xFu;
    out->has_handle_desc = ((w1 >> 31) & 1u) != 0;

    size_t idx = 2;

    if (out->has_handle_desc) {
        if (idx >= HIPC_BUFFER_WORDS) return false;
        const uint32_t hd = buf[idx++];
        out->send_pid = (hd & 1u) != 0;
        out->copy_handle_count = (hd >> 1) & 0xFu;
        out->move_handle_count = (hd >> 5) & 0xFu;

        if (out->send_pid) {
            if (idx + 2 > HIPC_BUFFER_WORDS) return false;
            out->pid = (uint64_t)buf[idx] | ((uint64_t)buf[idx + 1] << 32);
            idx += 2;
        }
        if (idx + out->copy_handle_count > HIPC_BUFFER_WORDS) return false;
        out->copy_handles = &buf[idx];
        idx += out->copy_handle_count;

        if (idx + out->move_handle_count > HIPC_BUFFER_WORDS) return false;
        out->move_handles = &buf[idx];
        idx += out->move_handle_count;
    }

    // X descriptors are two words each; A, B and W are three.
    idx += (size_t)num_x * 2u;
    idx += (size_t)(num_a + num_b + num_w) * 3u;

    // The receive list. 0 means none and 1 means "to the raw data section";
    // 2 is a single entry, and anything above that is a count biased by two.
    if (recv_flags >= 2u) {
        const size_t entries = (recv_flags == 2u) ? 1u : (size_t)(recv_flags - 2u);
        idx += entries * 2u;
    }

    if (idx > HIPC_BUFFER_WORDS) return false;

    // The raw section is 16-byte aligned from the start of the buffer.
    idx = (idx + 3u) & ~(size_t)3u;
    if (idx + CMIF_HEADER_WORDS > HIPC_BUFFER_WORDS) return false;

    out->raw = &buf[idx];
    out->raw_words = HIPC_BUFFER_WORDS - idx;
    return true;
}

// Assemble a reply into `buf`, which is the guest's command buffer.
//
// Layout and the raw word count follow build_result_response in
// Reference/horizon-linux/kernel/horizon/sys.c. The count is the size of the
// raw section *including* its mandatory 16-byte padding, which is why it is
// larger than the bytes actually written.
static void hipc_build_reply(uint32_t* buf, horizon_result result,
                             const void* payload, size_t payload_size,
                             const uint32_t* copy, size_t copy_count,
                             const uint32_t* move, size_t move_count)
{
    memset(buf, 0, HZN_IPC_BUFFER_SIZE);

    size_t idx = 2;
    const bool has_handle_desc = (copy_count > 0 || move_count > 0);

    if (has_handle_desc) {
        buf[idx++] = (uint32_t)((copy_count << 1) | (move_count << 5));
        for (size_t i = 0; i < copy_count; ++i) buf[idx++] = copy[i];
        for (size_t i = 0; i < move_count; ++i) buf[idx++] = move[i];
    }

    while (idx & 3u) buf[idx++] = 0;   // pad to the 16-byte raw alignment

    uint32_t* raw = &buf[idx];
    raw[0] = CMIF_MAGIC_OUT;
    raw[1] = 0;
    raw[2] = (uint32_t)result;
    raw[3] = 0;

    const size_t payload_words = (payload_size + 3u) / 4u;
    if (payload && payload_size > 0)
        memcpy(&raw[CMIF_HEADER_WORDS], payload, payload_size);

    // 2 result words + the 2-word payload header + 4 words of mandatory
    // padding + the 2 words yuzu's builder adds and every client expects.
    const uint32_t raw_size_words = (uint32_t)(10u + payload_words);

    buf[0] = 0;   // reply: no command type, no descriptors
    buf[1] = raw_size_words | (has_handle_desc ? 0x80000000u : 0u);
}

// ── services ───────────────────────────────────────────────────────────────

bool horizon_ipc_register_service(const char* name, horizon_ipc_handler_fn handler)
{
    if (!name || !handler) return false;
    const size_t len = strnlen(name, HZN_SERVICE_NAME_MAX);
    if (len == 0 || len >= HZN_SERVICE_NAME_MAX) {
        vwine_logf("horizon ipc: refusing to register \"%s\": a Horizon service "
                   "name is at most 8 characters (it travels as a u64)\n", name);
        return false;
    }
    for (size_t i = 0; i < HZN_MAX_SERVICES; ++i) {
        if (g_services[i].used) continue;
        g_services[i].used = true;
        memcpy(g_services[i].name, name, len);
        g_services[i].name[len] = '\0';
        g_services[i].handler = handler;
        return true;
    }
    vwine_logf("horizon ipc: the service table is full (%u); \"%s\" was not "
               "registered\n", (unsigned)HZN_MAX_SERVICES, name);
    return false;
}

static const hzn_service* service_find(const char* name)
{
    for (size_t i = 0; i < HZN_MAX_SERVICES; ++i)
        if (g_services[i].used && strcmp(g_services[i].name, name) == 0)
            return &g_services[i];
    return NULL;
}

static uint32_t session_open(const char* name, horizon_ipc_handler_fn handler)
{
    for (size_t i = 0; i < HZN_MAX_SESSIONS; ++i) {
        if (g_sessions[i].used) continue;
        g_sessions[i].used = true;
        g_sessions[i].handler = handler;
        strncpy(g_sessions[i].name, name, sizeof(g_sessions[i].name) - 1);
        g_sessions[i].name[sizeof(g_sessions[i].name) - 1] = '\0';

        const uint32_t handle = horizon_handle_create(HZN_OBJECT_SESSION, &g_sessions[i]);
        if (handle == HZN_HANDLE_INVALID) g_sessions[i].used = false;
        return handle;
    }
    vwine_logf("horizon ipc: out of session slots (%u); \"%s\" could not be "
               "opened\n", (unsigned)HZN_MAX_SESSIONS, name);
    return HZN_HANDLE_INVALID;
}

void horizon_ipc_release_session(void* payload)
{
    for (size_t i = 0; i < HZN_MAX_SESSIONS; ++i)
        if (payload == &g_sessions[i]) g_sessions[i].used = false;
}

// ── sm:, the service manager ───────────────────────────────────────────────
//
// The one service that is not a device driver. A Horizon process connects to
// it by name (it is the only named port there is) and asks it for everything
// else. Commands are transcribed from switchbrew's sm: page; the ones a client
// can issue are 0-4, and 2/3 are for a process registering a service of its
// own, which a guest running here is not.

static void sm_decode_name(uint64_t encoded, char out[HZN_SERVICE_NAME_MAX])
{
    // Eight characters packed low byte first, NUL-padded.
    for (size_t i = 0; i < 8; ++i) out[i] = (char)((encoded >> (i * 8)) & 0xFFu);
    out[8] = '\0';
}

static horizon_result sm_handler(horizon_ipc_context* ctx)
{
    switch (ctx->command_id) {
    case 0:   // RegisterClient / Initialize
        // The client sends its PID and gets nothing back. There is one process
        // here, so there is nothing to record -- but it is answered rather than
        // refused because it genuinely has no other effect.
        return HZN_RESULT_SUCCESS;

    case 1: {  // GetServiceHandle
        if (ctx->in_payload_size < sizeof(uint64_t)) {
            vwine_logf("horizon ipc: sm:GetServiceHandle with a %zu-byte "
                       "payload; the service name is a u64\n", ctx->in_payload_size);
            return HZN_RESULT_SM_INVALID_NAME;
        }
        uint64_t encoded = 0;
        memcpy(&encoded, ctx->in_payload, sizeof(encoded));

        char name[HZN_SERVICE_NAME_MAX];
        sm_decode_name(encoded, name);
        if (name[0] == '\0') return HZN_RESULT_SM_INVALID_NAME;

        const hzn_service* service = service_find(name);
        if (!service) {
            vwine_logf("horizon ipc: the guest asked sm: for \"%s\", which this "
                       "front-end does not implement. The IPC transport is "
                       "complete; the service is not -- it would have to be "
                       "built on MVII's own devices (/dev/fb0, /dev/input0, "
                       "/dev/dac0). Refusing rather than handing back a session "
                       "that answers nothing.\n", name);
            return HZN_RESULT_SM_NOT_REGISTERED;
        }

        const uint32_t handle = session_open(name, service->handler);
        if (handle == HZN_HANDLE_INVALID) return HZN_RESULT_OUT_OF_SESSIONS;

        ctx->move_handles[ctx->move_handle_count++] = handle;
        return HZN_RESULT_SUCCESS;
    }

    case 2:   // RegisterService
    case 3:   // UnregisterService
        vwine_logf("horizon ipc: sm: command %u registers or unregisters a "
                   "service. A guest here is a client, not a service provider, "
                   "and honouring it would let it claim a name this front-end "
                   "then routes to it.\n", ctx->command_id);
        return HZN_RESULT_SM_NOT_REGISTERED;

    case 4:   // DetachClient
        return HZN_RESULT_SUCCESS;

    default:
        vwine_logf("horizon ipc: sm: command %u is not one this front-end "
                   "knows\n", ctx->command_id);
        return HZN_RESULT_SM_NOT_REGISTERED;
    }
}

// ── the SVCs this file backs ───────────────────────────────────────────────

bool horizon_ipc_init(void)
{
    memset(g_sessions, 0, sizeof(g_sessions));
    memset(g_services, 0, sizeof(g_services));
    return true;
}

void horizon_ipc_shutdown(void)
{
    memset(g_sessions, 0, sizeof(g_sessions));
    memset(g_services, 0, sizeof(g_services));
}

horizon_result horizon_ipc_connect_to_named_port(const char* name, uint32_t* out_handle)
{
    if (!name) return HZN_RESULT_INVALID_POINTER;

    // Horizon has exactly one named port a process may connect to. Everything
    // else goes through sm:, and a guest reaching for another name is either
    // system software this front-end is not, or a mistake worth seeing.
    if (strcmp(name, "sm:") != 0) {
        vwine_logf("horizon ipc: svcConnectToNamedPort(\"%s\"). \"sm:\" is the "
                   "only named port a Horizon process may connect to; every "
                   "other service is obtained from it.\n", name);
        return HZN_RESULT_NOT_FOUND;
    }

    const uint32_t handle = session_open("sm:", sm_handler);
    if (handle == HZN_HANDLE_INVALID) return HZN_RESULT_OUT_OF_SESSIONS;
    *out_handle = handle;
    return HZN_RESULT_SUCCESS;
}

horizon_result horizon_ipc_send_sync_request(uint32_t session_handle)
{
    hzn_session* session =
        (hzn_session*)horizon_handle_lookup(session_handle, HZN_OBJECT_SESSION);
    if (!session) return HZN_RESULT_INVALID_HANDLE;

    uint32_t* cmdbuf = (uint32_t*)horizon_kernel_current_ipc_buffer();
    if (!cmdbuf) {
        vwine_logf("horizon ipc: svcSendSyncRequest from a thread with no TLS "
                   "block. That is a front-end bug -- every thread this kernel "
                   "starts is given one.\n");
        return HZN_RESULT_INVALID_ADDRESS;
    }

    hipc_request request;
    if (!hipc_parse(cmdbuf, &request)) {
        vwine_logf("horizon ipc: the command buffer on session \"%s\" does not "
                   "parse -- descriptor counts run past the 0x100-byte buffer "
                   "(word0=0x%08x word1=0x%08x)\n",
                   session->name, cmdbuf[0], cmdbuf[1]);
        return HZN_RESULT_INVALID_ARGUMENT;
    }

    // Closing the session. The reply is success and the Result is Horizon's
    // "remote process dead", which is what a client expects to see once it has
    // told the other end to go away -- see send_sync_request in
    // Reference/horizon-linux/kernel/horizon/sys.c.
    if (request.type == HIPC_TYPE_CLOSE || request.type == HIPC_TYPE_TIPC_CLOSE) {
        hipc_build_reply(cmdbuf, HZN_RESULT_SUCCESS, NULL, 0, NULL, 0, NULL, 0);
        return HZN_RESULT_IPC_REMOTE_PROCESS_DEAD;
    }

    if (request.type != HIPC_TYPE_REQUEST &&
        request.type != HIPC_TYPE_REQUEST_WITH_CONTEXT) {
        vwine_logf("horizon ipc: command type %u on session \"%s\". This "
                   "front-end speaks CMIF requests (4 and 6) and Close (2); "
                   "TIPC and the legacy types are not implemented, and a reply "
                   "in the wrong format would be read as a valid one.\n",
                   request.type, session->name);
        return HZN_RESULT_INVALID_ARGUMENT;
    }

    if (request.raw[0] != CMIF_MAGIC_IN) {
        vwine_logf("horizon ipc: raw section on session \"%s\" starts 0x%08x, "
                   "not 'SFCI'. Either the descriptor walk landed in the wrong "
                   "place or this is not a CMIF request.\n",
                   session->name, request.raw[0]);
        return HZN_RESULT_INVALID_ARGUMENT;
    }

    // The reply is assembled in the same buffer the request occupies, so the
    // payload is staged out of line: a handler must be able to read its
    // arguments while it writes its results.
    uint8_t out_payload[HZN_IPC_BUFFER_SIZE - 0x20];

    horizon_ipc_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.service = session->name;
    ctx.command_id = request.raw[2];
    ctx.in_payload = &request.raw[CMIF_HEADER_WORDS];
    ctx.in_payload_size = (request.raw_words - CMIF_HEADER_WORDS) * sizeof(uint32_t);
    ctx.out_payload = out_payload;
    ctx.out_payload_capacity = sizeof(out_payload);
    ctx.in_copy_handles = request.copy_handles;
    ctx.in_copy_handle_count = request.copy_handle_count;
    ctx.in_move_handles = request.move_handles;
    ctx.in_move_handle_count = request.move_handle_count;
    ctx.has_client_pid = request.send_pid;
    ctx.client_pid = request.pid;

    const horizon_result result = session->handler(&ctx);

    if (ctx.out_payload_size > ctx.out_payload_capacity) {
        vwine_logf("horizon ipc: \"%s\" command %u wrote %zu payload bytes into "
                   "a %zu-byte reply\n", session->name, ctx.command_id,
                   ctx.out_payload_size, ctx.out_payload_capacity);
        return HZN_RESULT_OUT_OF_RANGE;
    }

    hipc_build_reply(cmdbuf, result, out_payload, ctx.out_payload_size,
                     ctx.copy_handles, ctx.copy_handle_count,
                     ctx.move_handles, ctx.move_handle_count);

    // The Result of svcSendSyncRequest itself is about the transport: the
    // command was delivered and answered. Whatever the service thought of the
    // request travels inside the reply, where the guest's own IPC layer reads
    // it.
    return HZN_RESULT_SUCCESS;
}

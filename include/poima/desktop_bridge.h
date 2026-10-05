// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#if defined(_WIN32)
#if defined(POIMA_DESKTOP_EXPORTS)
#define POIMA_DESKTOP_API __declspec(dllexport)
#else
#define POIMA_DESKTOP_API __declspec(dllimport)
#endif
#else
#define POIMA_DESKTOP_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
// One host per process. All operations, including destruction, must run on the
// creating UI thread. Arguments are valid, NUL-terminated UTF-8 strings. Null or
// empty endpoint disables local IPC; world is required. No graphics at create.
POIMA_DESKTOP_API void* poima_desktop_create(const char* world,const char* endpoint,int32_t gpu,uint32_t samples);
// Results are owned by the host until its next bridge call. Copy immediately.
// call returns a JSON-RPC response (empty string for a notification), or NULL on
// bridge failure. Capture is asynchronous; discover its job API via desktop.describe.
POIMA_DESKTOP_API const char* poima_desktop_call(void* host,const char* jsonrpc);
// Pumps bounded local IPC and returns revision/runtime/presentation/capture state.
POIMA_DESKTOP_API const char* poima_desktop_poll(void* host);
// HWND remains owned by the frontend, valid until detach returns. Attach returns
// 1 on success, 0 on failure. Detaching preserves the world and IPC endpoint.
POIMA_DESKTOP_API int poima_desktop_attach(void* host,void* hwnd);
// 1 presented, 0 transient/unattached, -1 failure. desktop.inspect graphics_error
// distinguishes poisoned graphics (detach/reattach required) from retryable scene
// preparation errors. Authoring stays available; draw never advances physics.
POIMA_DESKTOP_API int poima_desktop_draw(void* host);
POIMA_DESKTOP_API void poima_desktop_detach(void* host);
POIMA_DESKTOP_API void poima_desktop_destroy(void* host);
// Error text is owned by host (or thread-local for NULL/invalid host), not freed
// by the caller. Exceptions never cross this ABI. There is no cross-thread API.
POIMA_DESKTOP_API const char* poima_desktop_error(void* host);
#ifdef __cplusplus
}
#endif

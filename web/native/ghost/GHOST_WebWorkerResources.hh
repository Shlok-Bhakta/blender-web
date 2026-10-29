/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Register process-lifetime callbacks once. apply/detach execute with registry
 * synchronization held: never publish, proxy, or reenter the registry there.
 * Resource users must synchronize native ownership before retirement.
 * Publication synchronizes the runtime thread and notifies other workers
 * without waiting for busy workers. Call refresh before using a new resource.
 */
struct GHOST_WebWorkerResource {
  void (*apply)(void *);
  void (*detach)(void *);
  void *data;
};
extern "C" void GHOST_web_register_worker_resource(GHOST_WebWorkerResource resource);
extern "C" void GHOST_web_publish_worker_resources();
extern "C" void GHOST_web_invoke_runtime_thread(void (*function)(void *), void *data);
extern "C" void GHOST_web_refresh_worker_resources();

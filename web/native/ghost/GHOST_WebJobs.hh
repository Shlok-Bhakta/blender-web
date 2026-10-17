/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
bool GHOST_web_schedule_gpu_job(bool uses_gpu, void *(*run)(void *), void *job, bool *ready);
bool GHOST_web_gpu_job_active();

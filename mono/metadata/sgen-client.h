/*
 * sgen-client.h: SGen client interface.
 *
 * Copyright (C) 2014 Xamarin Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License 2.0 as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License 2.0 along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "mono/metadata/sgen-pointer-queue.h"

void sgen_client_init (void);

mword sgen_client_slow_object_get_size (GCVTable *vtable, GCObject* o);

/*
 * Fill the given range with a dummy object.  If the range is too short to be filled with an
 * object, null it.  Return `TRUE` if the range was filled with an object, `FALSE` if it was
 * nulled.
 */
gboolean sgen_client_array_fill_range (char *start, size_t size);

/*
 * This is called if the nursery clearing policy at `clear-at-gc`, which is usually only
 * used for debugging.  If `size` is large enough for the memory to have been filled with a
 * dummy, object, zero its header.  Note that there might not actually be a header there.
 */
void sgen_client_zero_array_fill_header (void *p, size_t size);

gboolean sgen_client_object_is_array_fill (GCObject *o);

void sgen_client_object_register_finalizer_if_necessary (GCObject *obj);

gboolean sgen_client_object_has_critical_finalizer (GCObject *obj);

/*
 * Called after an object is enqueued for finalization.  This is a very low-level callback.
 * It should almost certainly be a NOP.
 *
 * FIXME: Can we merge this with `sgen_client_object_has_critical_finalizer()`?
 */
void sgen_client_object_queued_for_finalization (GCObject *obj);

void sgen_client_run_finalize (GCObject *obj);

void sgen_client_finalize_notify (void);

gboolean sgen_client_mark_ephemerons (ScanCopyContext ctx);

/*
 * Clear ephemeron pairs with unreachable keys.
 * We pass the copy func so we can figure out if an array was promoted or not.
 */
void sgen_client_clear_unreachable_ephemerons (ScanCopyContext ctx);

/*
 * This is called for objects that are larger than one card.  If it's possible to scan only
 * parts of the object based on which cards are marked, do so and return TRUE.  Otherwise,
 * return FALSE.
 */
gboolean sgen_client_cardtable_scan_object (char *obj, mword block_obj_size, guint8 *cards, gboolean mod_union, SgenGrayQueue *queue);

void sgen_client_nursery_objects_pinned (void **definitely_pinned, int count);

void sgen_client_collecting_minor (SgenPointerQueue *fin_ready_queue, SgenPointerQueue *critical_fin_queue);
void sgen_client_collecting_major_1 (void);
void sgen_client_pinned_los_object (char *obj);
void sgen_client_collecting_major_2 (void);
void sgen_client_collecting_major_3 (SgenPointerQueue *fin_ready_queue, SgenPointerQueue *critical_fin_queue);

void sgen_client_degraded_allocation (size_t size);

void sgen_client_total_allocated_heap (mword allocated_heap);

gpointer sgen_client_out_of_memory (size_t size);

/*
 * If the client has registered any internal memory types, this must return a string
 * describing the given type.  Only used for debugging.
 */
const char* sgen_client_description_for_internal_mem_type (int type);

const char* sgen_client_vtable_get_namespace (GCVTable *vtable);
const char* sgen_client_vtable_get_name (GCVTable *vtable);

void sgen_client_pre_collection_checks (void);

size_t sgen_client_page_size (void);
void* sgen_client_valloc (size_t size, gboolean activate);
void* sgen_client_valloc_aligned (size_t size, size_t alignment, gboolean activate);
void sgen_client_vfree (void *addr, size_t size);

void sgen_client_thread_register (SgenThreadInfo* info, void *stack_bottom_fallback);
void sgen_client_thread_unregister (SgenThreadInfo *p);

void sgen_client_thread_register_worker (void);

void sgen_client_scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise, SgenGrayQueue *queue);

void sgen_client_stop_world (int generation);
void sgen_client_restart_world (int generation, GGTimingInfo *timing);

gboolean sgen_client_bridge_need_processing (void);
void sgen_client_bridge_reset_data (void);
void sgen_client_bridge_processing_stw_step (void);
void sgen_client_bridge_wait_for_processing (void);
void sgen_client_bridge_processing_finish (int generation);
gboolean sgen_client_bridge_is_bridge_object (GCObject *obj);
void sgen_client_bridge_register_finalized_object (GCObject *object);

void sgen_client_log_timing (GGTimingInfo *info, mword last_major_num_sections, mword last_los_memory_usage);

gboolean sgen_client_handle_gc_param (const char *opt);
void sgen_client_print_gc_params_usage (void);

gboolean sgen_client_handle_gc_debug (const char *opt);
void sgen_client_print_gc_debug_usage (void);

void sgen_client_protocol_collection_requested (int generation, size_t requested_size, gboolean force);
void sgen_client_protocol_collection_begin (int minor_gc_count, int generation);
void sgen_client_protocol_collection_end (int minor_gc_count, int generation, long long num_objects_scanned, long long num_unique_objects_scanned);
void sgen_client_protocol_concurrent_start (void);
void sgen_client_protocol_concurrent_update (void);
void sgen_client_protocol_concurrent_finish (void);
void sgen_client_protocol_world_stopping (int generation);
void sgen_client_protocol_world_stopped (int generation);
void sgen_client_protocol_world_restarting (int generation);
void sgen_client_protocol_world_restarted (int generation);
void sgen_client_protocol_mark_start (int generation);
void sgen_client_protocol_mark_end (int generation);
void sgen_client_protocol_reclaim_start (int generation);
void sgen_client_protocol_reclaim_end (int generation);
void sgen_client_protocol_alloc (gpointer obj, gpointer vtable, size_t size, gboolean pinned);
void sgen_client_protocol_alloc_degraded (gpointer obj, gpointer vtable, size_t size);
void sgen_client_protocol_pin (gpointer obj, gpointer vtable, size_t size);
void sgen_client_protocol_cement (gpointer ptr, gpointer vtable, size_t size);
void sgen_client_protocol_copy (gpointer from, gpointer to, gpointer vtable, size_t size);
void sgen_client_protocol_global_remset (gpointer ptr, gpointer value, gpointer value_vtable);
void sgen_client_protocol_dislink_update (gpointer link, gpointer obj, gboolean track, gboolean staged);
void sgen_client_protocol_empty (gpointer start, size_t size);

void sgen_client_counter_register_time (const char *name, guint64 *value, gboolean monotonic);
void sgen_client_counter_register_uint64 (const char *name, guint64 *value);
void sgen_client_counter_register_byte_count (const char *name, mword *value, gboolean monotonic);

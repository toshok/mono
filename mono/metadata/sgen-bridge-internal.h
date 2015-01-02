/*
 * sgen-bridge-internal.h: The cross-GC bridge.
 *
 * Copyright (C) 2015 Xamarin Inc
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

#ifndef __MONO_SGENBRIDGEINTERNAL_H__
#define __MONO_SGENBRIDGEINTERNAL_H__

#include "config.h"

#ifdef HAVE_SGEN_GC

#include "utils/mono-compiler.h"

#include "metadata/sgen-gc.h"
#include "metadata/sgen-bridge.h"

extern gboolean bridge_processing_in_progress;
extern MonoGCBridgeCallbacks bridge_callbacks;

gboolean sgen_need_bridge_processing (void) MONO_INTERNAL;
void sgen_bridge_reset_data (void) MONO_INTERNAL;
void sgen_bridge_processing_stw_step (void) MONO_INTERNAL;
void sgen_bridge_processing_finish (int generation) MONO_INTERNAL;
gboolean sgen_is_bridge_object (GCObject *obj) MONO_INTERNAL;
MonoGCBridgeObjectKind sgen_bridge_class_kind (MonoClass *klass) MONO_INTERNAL;
void sgen_bridge_register_finalized_object (GCObject *object) MONO_INTERNAL;
void sgen_bridge_describe_pointer (GCObject *object) MONO_INTERNAL;

gboolean sgen_is_bridge_object (GCObject *obj) MONO_INTERNAL;
void sgen_mark_bridge_object (GCObject *obj) MONO_INTERNAL;

gboolean sgen_bridge_handle_gc_debug (const char *opt) MONO_INTERNAL;
void sgen_bridge_print_gc_debug_usage (void) MONO_INTERNAL;

typedef struct {
	void (*reset_data) (void);
	void (*processing_stw_step) (void);
	void (*processing_build_callback_data) (int generation);
	void (*processing_after_callback) (int generation);
	MonoGCBridgeObjectKind (*class_kind) (MonoClass *class);
	void (*register_finalized_object) (GCObject *object);
	void (*describe_pointer) (GCObject *object);
	void (*enable_accounting) (void);
	void (*set_dump_prefix) (const char *prefix);

	/*
	 * These are set by processing_build_callback_data().
	 */
	int num_sccs;
	MonoGCBridgeSCC **api_sccs;

	int num_xrefs;
	MonoGCBridgeXRef *api_xrefs;
} SgenBridgeProcessor;

void sgen_old_bridge_init (SgenBridgeProcessor *collector) MONO_INTERNAL;
void sgen_new_bridge_init (SgenBridgeProcessor *collector) MONO_INTERNAL;
void sgen_tarjan_bridge_init (SgenBridgeProcessor *collector) MONO_INTERNAL;
void sgen_set_bridge_implementation (const char *name) MONO_INTERNAL;
void sgen_bridge_set_dump_prefix (const char *prefix) MONO_INTERNAL;

gboolean sgen_compare_bridge_processor_results (SgenBridgeProcessor *a, SgenBridgeProcessor *b) MONO_INTERNAL;

#endif

#endif

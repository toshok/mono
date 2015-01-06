/*
 * sgen-cardtable.c: Card table implementation for sgen
 *
 * Author:
 * 	Rodrigo Kumpera (rkumpera@novell.com)
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Copyright (C) 2012 Xamarin Inc
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

#include "config.h"
#ifdef HAVE_SGEN_GC

#include "metadata/sgen-gc.h"
#include "metadata/sgen-cardtable.h"
#include "metadata/sgen-memory-governor.h"
#include "metadata/sgen-protocol.h"
#include "metadata/sgen-layout-stats.h"
#include "metadata/sgen-client.h"
#include "metadata/gc-internal-agnostic.h"
#include "utils/mono-memory-model.h"
#include <string.h>


//#define CARDTABLE_STATS

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#include <sys/types.h>

guint8 *sgen_cardtable;

static gboolean need_mod_union;

#ifdef HEAVY_STATISTICS
guint64 marked_cards;
guint64 scanned_cards;
guint64 scanned_objects;
guint64 remarked_cards;

static guint64 los_marked_cards;
static guint64 large_objects;
static guint64 bloby_objects;
static guint64 los_array_cards;
static guint64 los_array_remsets;

#endif
static guint64 major_card_scan_time;
static guint64 los_card_scan_time;

static guint64 last_major_scan_time;
static guint64 last_los_scan_time;

static void sgen_card_tables_collect_stats (gboolean begin);

mword
sgen_card_table_number_of_cards_in_range (mword address, mword size)
{
	mword end = address + MAX (1, size) - 1;
	return (end >> CARD_BITS) - (address >> CARD_BITS) + 1;
}

static void
sgen_card_table_wbarrier_set_field (GCObject *obj, gpointer field_ptr, GCObject* value)
{
	*(void**)field_ptr = value;
	if (need_mod_union || sgen_ptr_in_nursery (value))
		sgen_card_table_mark_address ((mword)field_ptr);
	sgen_dummy_use (value);
}

static void
sgen_card_table_wbarrier_arrayref_copy (gpointer dest_ptr, gpointer src_ptr, int count)
{
	gpointer *dest = dest_ptr;
	gpointer *src = src_ptr;

	/*overlapping that required backward copying*/
	if (src < dest && (src + count) > dest) {
		gpointer *start = dest;
		dest += count - 1;
		src += count - 1;

		for (; dest >= start; --src, --dest) {
			gpointer value = *src;
			SGEN_UPDATE_REFERENCE_ALLOW_NULL (dest, value);
			if (need_mod_union || sgen_ptr_in_nursery (value))
				sgen_card_table_mark_address ((mword)dest);
			sgen_dummy_use (value);
		}
	} else {
		gpointer *end = dest + count;
		for (; dest < end; ++src, ++dest) {
			gpointer value = *src;
			SGEN_UPDATE_REFERENCE_ALLOW_NULL (dest, value);
			if (need_mod_union || sgen_ptr_in_nursery (value))
				sgen_card_table_mark_address ((mword)dest);
			sgen_dummy_use (value);
		}
	}	
}

static void
sgen_card_table_wbarrier_value_copy (gpointer dest, gpointer src, int count, size_t element_size)
{
	size_t size = count * element_size;

#ifdef DISABLE_CRITICAL_REGION
	LOCK_GC;
#else
	TLAB_ACCESS_INIT;
	ENTER_CRITICAL_REGION;
#endif
	mono_gc_memmove_atomic (dest, src, size);
	sgen_card_table_mark_range ((mword)dest, size);
#ifdef DISABLE_CRITICAL_REGION
	UNLOCK_GC;
#else
	EXIT_CRITICAL_REGION;
#endif
}

static void
sgen_card_table_wbarrier_object_copy (GCObject* obj, GCObject *src)
{
	size_t size = sgen_client_vtable_get_instance_size (SGEN_LOAD_VTABLE_UNCHECKED(obj));

#ifdef DISABLE_CRITICAL_REGION
	LOCK_GC;
#else
	TLAB_ACCESS_INIT;
	ENTER_CRITICAL_REGION;
#endif
	mono_gc_memmove_aligned ((char*)obj + SGEN_CLIENT_OBJECT_HEADER_SIZE, (char*)src + SGEN_CLIENT_OBJECT_HEADER_SIZE,
			size - SGEN_CLIENT_OBJECT_HEADER_SIZE);
	sgen_card_table_mark_range ((mword)obj, size);
#ifdef DISABLE_CRITICAL_REGION
	UNLOCK_GC;
#else
	EXIT_CRITICAL_REGION;
#endif	
}

static void
sgen_card_table_wbarrier_generic_nostore (gpointer ptr)
{
	sgen_card_table_mark_address ((mword)ptr);	
}

#ifdef SGEN_HAVE_OVERLAPPING_CARDS

guint8 *sgen_shadow_cardtable;

#define SGEN_CARDTABLE_END (sgen_cardtable + CARD_COUNT_IN_BYTES)

static gboolean
sgen_card_table_region_begin_scanning (mword start, mword end)
{
	/*XXX this can be improved to work on words and have a single loop induction var */
	while (start <= end) {
		if (sgen_card_table_card_begin_scanning (start))
			return TRUE;
		start += CARD_SIZE_IN_BYTES;
	}
	return FALSE;
}

#else

static gboolean
sgen_card_table_region_begin_scanning (mword start, mword size)
{
	gboolean res = FALSE;
	guint8 *card = sgen_card_table_get_card_address (start);
	guint8 *end = card + sgen_card_table_number_of_cards_in_range (start, size);

	/*XXX this can be improved to work on words and have a branchless body */
	while (card != end) {
		if (*card++) {
			res = TRUE;
			break;
		}
	}

	memset (sgen_card_table_get_card_address (start), 0, size >> CARD_BITS);

	return res;
}

#endif

/*FIXME this assumes that major blocks are multiple of 4K which is pretty reasonable */
gboolean
sgen_card_table_get_card_data (guint8 *data_dest, mword address, mword cards)
{
	mword *start = (mword*)sgen_card_table_get_card_scan_address (address);
	mword *dest = (mword*)data_dest;
	mword *end = (mword*)(data_dest + cards);
	mword mask = 0;

	for (; dest < end; ++dest, ++start) {
		mword v = *start;
		*dest = v;
		mask |= v;

#ifndef SGEN_HAVE_OVERLAPPING_CARDS
		*start = 0;
#endif
	}

	return mask != 0;
}

void*
sgen_card_table_align_pointer (void *ptr)
{
	return (void*)((mword)ptr & ~(CARD_SIZE_IN_BYTES - 1));
}

void
sgen_card_table_mark_range (mword address, mword size)
{
	memset (sgen_card_table_get_card_address (address), 1, sgen_card_table_number_of_cards_in_range (address, size));
}

static gboolean
sgen_card_table_is_range_marked (guint8 *cards, mword address, mword size)
{
	guint8 *end = cards + sgen_card_table_number_of_cards_in_range (address, size);

	/*This is safe since this function is only called by code that only passes continuous card blocks*/
	while (cards != end) {
		if (*cards++)
			return TRUE;
	}
	return FALSE;

}

static void
sgen_card_table_record_pointer (gpointer address)
{
	*sgen_card_table_get_card_address ((mword)address) = 1;
}

static gboolean
sgen_card_table_find_address (char *addr)
{
	return sgen_card_table_address_is_marked ((mword)addr);
}

static gboolean
sgen_card_table_find_address_with_cards (char *cards_start, guint8 *cards, char *addr)
{
	cards_start = sgen_card_table_align_pointer (cards_start);
	return cards [(addr - cards_start) >> CARD_BITS];
}

static void
update_mod_union (guint8 *dest, gboolean init, guint8 *start_card, size_t num_cards)
{
	if (init) {
		memcpy (dest, start_card, num_cards);
	} else {
		int i;
		for (i = 0; i < num_cards; ++i)
			dest [i] |= start_card [i];
	}
}

static guint8*
alloc_mod_union (size_t num_cards)
{
	return sgen_alloc_internal_dynamic (num_cards, INTERNAL_MEM_CARDTABLE_MOD_UNION, TRUE);
}

guint8*
sgen_card_table_update_mod_union_from_cards (guint8 *dest, guint8 *start_card, size_t num_cards)
{
	gboolean init = dest == NULL;

	if (init)
		dest = alloc_mod_union (num_cards);

	update_mod_union (dest, init, start_card, num_cards);

	return dest;
}

guint8*
sgen_card_table_update_mod_union (guint8 *dest, char *obj, mword obj_size, size_t *out_num_cards)
{
	guint8 *start_card = sgen_card_table_get_card_address ((mword)obj);
#ifndef SGEN_HAVE_OVERLAPPING_CARDS
	guint8 *end_card = sgen_card_table_get_card_address ((mword)obj + obj_size - 1) + 1;
#endif
	size_t num_cards;
	guint8 *result = NULL;

#ifdef SGEN_HAVE_OVERLAPPING_CARDS
	size_t rest;

	rest = num_cards = sgen_card_table_number_of_cards_in_range ((mword) obj, obj_size);

	while (start_card + rest > SGEN_CARDTABLE_END) {
		size_t count = SGEN_CARDTABLE_END - start_card;
		dest = sgen_card_table_update_mod_union_from_cards (dest, start_card, count);
		if (!result)
			result = dest;
		dest += count;
		rest -= count;
		start_card = sgen_cardtable;
	}
	num_cards = rest;
#else
	num_cards = end_card - start_card;
#endif

	dest = sgen_card_table_update_mod_union_from_cards (dest, start_card, num_cards);
	if (!result)
		result = dest;

	if (out_num_cards)
		*out_num_cards = num_cards;

	return result;
}

#ifdef SGEN_HAVE_OVERLAPPING_CARDS

static void
move_cards_to_shadow_table (mword start, mword size)
{
	guint8 *from = sgen_card_table_get_card_address (start);
	guint8 *to = sgen_card_table_get_shadow_card_address (start);
	size_t bytes = sgen_card_table_number_of_cards_in_range (start, size);

	if (bytes >= CARD_COUNT_IN_BYTES) {
		memcpy (sgen_shadow_cardtable, sgen_cardtable, CARD_COUNT_IN_BYTES);
	} else if (to + bytes > SGEN_SHADOW_CARDTABLE_END) {
		size_t first_chunk = SGEN_SHADOW_CARDTABLE_END - to;
		size_t second_chunk = MIN (CARD_COUNT_IN_BYTES, bytes) - first_chunk;

		memcpy (to, from, first_chunk);
		memcpy (sgen_shadow_cardtable, sgen_cardtable, second_chunk);
	} else {
		memcpy (to, from, bytes);
	}
}

static void
clear_cards (mword start, mword size)
{
	guint8 *addr = sgen_card_table_get_card_address (start);
	size_t bytes = sgen_card_table_number_of_cards_in_range (start, size);

	if (bytes >= CARD_COUNT_IN_BYTES) {
		memset (sgen_cardtable, 0, CARD_COUNT_IN_BYTES);
	} else if (addr + bytes > SGEN_CARDTABLE_END) {
		size_t first_chunk = SGEN_CARDTABLE_END - addr;

		memset (addr, 0, first_chunk);
		memset (sgen_cardtable, 0, bytes - first_chunk);
	} else {
		memset (addr, 0, bytes);
	}
}


#else

static void
clear_cards (mword start, mword size)
{
	memset (sgen_card_table_get_card_address (start), 0, sgen_card_table_number_of_cards_in_range (start, size));
}


#endif

static void
sgen_card_table_prepare_for_major_collection (void)
{
	/*XXX we could do this in 2 ways. using mincore or iterating over all sections/los objects */
	sgen_major_collector_iterate_live_block_ranges (clear_cards);
	sgen_los_iterate_live_block_ranges (clear_cards);
}

static void
sgen_card_table_finish_minor_collection (void)
{
	sgen_card_tables_collect_stats (FALSE);
}

static void
sgen_card_table_finish_scan_remsets (void *start_nursery, void *end_nursery, SgenGrayQueue *queue)
{
	SGEN_TV_DECLARE (atv);
	SGEN_TV_DECLARE (btv);

	sgen_card_tables_collect_stats (TRUE);

#ifdef SGEN_HAVE_OVERLAPPING_CARDS
	/*FIXME we should have a bit on each block/los object telling if the object have marked cards.*/
	/*First we copy*/
	sgen_major_collector_iterate_live_block_ranges (move_cards_to_shadow_table);
	sgen_los_iterate_live_block_ranges (move_cards_to_shadow_table);

	/*Then we clear*/
	sgen_card_table_prepare_for_major_collection ();
#endif
	SGEN_TV_GETTIME (atv);
	sgen_major_collector_scan_card_table (queue);
	SGEN_TV_GETTIME (btv);
	last_major_scan_time = SGEN_TV_ELAPSED (atv, btv); 
	major_card_scan_time += last_major_scan_time;
	sgen_los_scan_card_table (FALSE, queue);
	SGEN_TV_GETTIME (atv);
	last_los_scan_time = SGEN_TV_ELAPSED (btv, atv);
	los_card_scan_time += last_los_scan_time;
}

guint8*
sgen_get_card_table_configuration (int *shift_bits, gpointer *mask)
{
#ifndef MANAGED_WBARRIER
	return NULL;
#else
	if (!sgen_cardtable)
		return NULL;

	*shift_bits = CARD_BITS;
#ifdef SGEN_HAVE_OVERLAPPING_CARDS
	*mask = (gpointer)CARD_MASK;
#else
	*mask = NULL;
#endif

	return sgen_cardtable;
#endif
}

#if 0
void
sgen_card_table_dump_obj_card (char *object, size_t size, void *dummy)
{
	guint8 *start = sgen_card_table_get_card_scan_address (object);
	guint8 *end = start + sgen_card_table_number_of_cards_in_range (object, size);
	int cnt = 0;
	printf ("--obj %p %d cards [%p %p]--", object, size, start, end);
	for (; start < end; ++start) {
		if (cnt == 0)
			printf ("\n\t[%p] ", start);
		printf ("%x ", *start);
		++cnt;
		if (cnt == 8)
			cnt = 0;
	}
	printf ("\n");
}
#endif

void
sgen_cardtable_scan_object (char *obj, mword block_obj_size, guint8 *cards, gboolean mod_union, SgenGrayQueue *queue)
{
	HEAVY_STAT (++large_objects);

	if (sgen_client_cardtable_scan_object (obj, block_obj_size, cards, mod_union, queue))
		return;

	HEAVY_STAT (++bloby_objects);
	if (cards) {
		if (sgen_card_table_is_range_marked (cards, (mword)obj, block_obj_size))
			sgen_get_current_object_ops ()->scan_object (obj, sgen_obj_get_descriptor (obj), queue);
	} else if (sgen_card_table_region_begin_scanning ((mword)obj, block_obj_size)) {
		sgen_get_current_object_ops ()->scan_object (obj, sgen_obj_get_descriptor (obj), queue);
	}

	binary_protocol_card_scan (obj, sgen_safe_object_get_size ((GCObject*)obj));
}

#ifdef CARDTABLE_STATS

typedef struct {
	int total, marked, remarked, gc_marked;	
} card_stats;

static card_stats major_stats, los_stats;
static card_stats *cur_stats;

static void
count_marked_cards (mword start, mword size)
{
	mword end = start + size;
	while (start <= end) {
		guint8 card = *sgen_card_table_get_card_address (start);
		++cur_stats->total;
		if (card)
			++cur_stats->marked;
		if (card == 2)
			++cur_stats->gc_marked;
		start += CARD_SIZE_IN_BYTES;
	}
}

static void
count_remarked_cards (mword start, mword size)
{
	mword end = start + size;
	while (start <= end) {
		if (sgen_card_table_address_is_marked (start)) {
			++cur_stats->remarked;
			*sgen_card_table_get_card_address (start) = 2;
		}
		start += CARD_SIZE_IN_BYTES;
	}
}

#endif

static void
sgen_card_tables_collect_stats (gboolean begin)
{
#ifdef CARDTABLE_STATS
	if (begin) {
		memset (&major_stats, 0, sizeof (card_stats));
		memset (&los_stats, 0, sizeof (card_stats));
		cur_stats = &major_stats;
		sgen_major_collector_iterate_live_block_ranges (count_marked_cards);
		cur_stats = &los_stats;
		sgen_los_iterate_live_block_ranges (count_marked_cards);
	} else {
		cur_stats = &major_stats;
		sgen_major_collector_iterate_live_block_ranges (count_remarked_cards);
		cur_stats = &los_stats;
		sgen_los_iterate_live_block_ranges (count_remarked_cards);
		printf ("cards major (t %d m %d g %d r %d)  los (t %d m %d g %d r %d) major_scan %.2fms los_scan %.2fms\n", 
			major_stats.total, major_stats.marked, major_stats.gc_marked, major_stats.remarked,
			los_stats.total, los_stats.marked, los_stats.gc_marked, los_stats.remarked,
			last_major_scan_time / 10000.0f, last_los_scan_time / 10000.0f);
	}
#endif
}

void
sgen_card_table_init (SgenRemeberedSet *remset)
{
	sgen_cardtable = sgen_alloc_os_memory (CARD_COUNT_IN_BYTES, SGEN_ALLOC_INTERNAL | SGEN_ALLOC_ACTIVATE, "card table");

#ifdef SGEN_HAVE_OVERLAPPING_CARDS
	sgen_shadow_cardtable = sgen_alloc_os_memory (CARD_COUNT_IN_BYTES, SGEN_ALLOC_INTERNAL | SGEN_ALLOC_ACTIVATE, "shadow card table");
#endif

#ifdef HEAVY_STATISTICS
	sgen_client_counter_register_uint64 ("marked cards", &marked_cards);
	sgen_client_counter_register_uint64 ("scanned cards", &scanned_cards);
	sgen_client_counter_register_uint64 ("remarked cards", &remarked_cards);

	sgen_client_counter_register_uint64 ("los marked cards", &los_marked_cards);
	sgen_client_counter_register_uint64 ("los array cards scanned ", &los_array_cards);
	sgen_client_counter_register_uint64 ("los array remsets", &los_array_remsets);
	sgen_client_counter_register_uint64 ("cardtable scanned objects", &scanned_objects);
	sgen_client_counter_register_uint64 ("cardtable large objects", &large_objects);
	sgen_client_counter_register_uint64 ("cardtable bloby objects", &bloby_objects);
#endif
	sgen_client_counter_register_time ("cardtable major scan time", &major_card_scan_time, FALSE);
	sgen_client_counter_register_time ("cardtable los scan time", &los_card_scan_time, FALSE);


	remset->wbarrier_set_field = sgen_card_table_wbarrier_set_field;
	remset->wbarrier_arrayref_copy = sgen_card_table_wbarrier_arrayref_copy;
	remset->wbarrier_value_copy = sgen_card_table_wbarrier_value_copy;
	remset->wbarrier_object_copy = sgen_card_table_wbarrier_object_copy;
	remset->wbarrier_generic_nostore = sgen_card_table_wbarrier_generic_nostore;
	remset->record_pointer = sgen_card_table_record_pointer;

	remset->finish_scan_remsets = sgen_card_table_finish_scan_remsets;

	remset->finish_minor_collection = sgen_card_table_finish_minor_collection;
	remset->prepare_for_major_collection = sgen_card_table_prepare_for_major_collection;

	remset->find_address = sgen_card_table_find_address;
	remset->find_address_with_cards = sgen_card_table_find_address_with_cards;

	need_mod_union = sgen_get_major_collector ()->is_concurrent;
}

#endif /*HAVE_SGEN_GC*/

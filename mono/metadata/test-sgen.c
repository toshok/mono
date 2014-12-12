#include <stdint.h>
#include <stdio.h>
#include "sgen-gc.h"
#include "sgen-descriptor.h"
#include "../io-layer/wapi.h" // just for wapi_init() below
#include "mono-gc.h"
#include "gc-internal.h"

#define ITERS 100LL
#define OBJS_PER_ITER 10000000LL
#define BYTES_PER_OBJ sizeof(struct ObjectType)

struct VTableType {
  void* klass;     // or shape, or whatever
  void* gc_descr;  // the gc expects the descriptor to be the second pointer
};

struct ObjectType {
  struct VTableType* vtable;
  struct ObjectType* objslot;
};

static size_t object_gc_bitmap = 1 << 1; // objslot is slot 1
static struct VTableType test_vtable;

static struct ObjectType*
allocate_some_things(int i, struct ObjectType* prev)
{
  if (i == 0) return prev;

  struct ObjectType* next = mono_gc_alloc_obj((MonoVTable*)&test_vtable, 16);
  mono_gc_wbarrier_generic_store(&next->objslot, (MonoObject*)prev);

  return allocate_some_things(i-1, next);
}

#define MEGS(x) ((x) / 1024.0 / 1024.0)
int
main(int argc, char** argv)
{
  wapi_init();
  mono_perfcounters_init();
  mono_gc_init();

  test_vtable.klass = NULL;
  test_vtable.gc_descr = mono_gc_make_descr_from_bitmap(&object_gc_bitmap, 1);

  // put something on the stack that we can take the address of to give the gc an idea where to start
  int unused_field;
  struct ObjectType* last_chain;

  mono_gc_register_thread(&unused_field);

  printf ("allocate a bunch of objects. %lld objects (%gM), %lld for each '.' (%gM).\n",
	  ITERS * OBJS_PER_ITER,
	  MEGS(ITERS * OBJS_PER_ITER * BYTES_PER_OBJ),
	  OBJS_PER_ITER,
	  MEGS(OBJS_PER_ITER * BYTES_PER_OBJ));

  for (int n = 0; n < ITERS; n ++) {
    printf ("."); fflush(stdout);

    struct ObjectType* chain = allocate_some_things(OBJS_PER_ITER, NULL);

    // now walk the entire list to make sure nothing got collected
    last_chain = chain;
    while (chain) {
      //printf ("chain = %p\n", chain);
      chain = chain->objslot;
    }
  }

  // perform one major collection at the end
  mono_gc_collect(mono_gc_max_generation());

  printf ("\n");

  // this crashes because not all mono thread state is initialized
  // mono_gc_cleanup();


  // dump out our collection stats
  printf ("minor collections: %u\n", gc_stats.minor_gc_count);
  printf ("major collections: %u\n", gc_stats.major_gc_count);
  printf ("heap_size: %gM\n", MEGS(mono_gc_get_heap_size()));
  printf ("used_size: %gM\n", MEGS(mono_gc_get_used_size()));
}

/*

 Copyright 2016 Christian Hoene, Symonics GmbH

 */

#include "reader.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/*  III.E. Disk Format: Level 1E - Global Heap
 */

static int readGCOL(struct READER *reader) {

  uint16_t reference_count, address;
  uint64_t collection_size, end;
  struct GCOL *gcol;
  char buf[5];

  UNUSED(reference_count);

  /* read signature */
  if (readfn(reader->fhd, buf, 4) != 4 || strncmp(buf, "GCOL", 4)) {
    mylog("cannot read signature of global heap collection\n");
    return MYSOFA_INVALID_FORMAT;
  }
  buf[4] = 0;

  if (getcfn(reader->fhd) != 1) {
    mylog("object GCOL must have version 1\n");
    return MYSOFA_INVALID_FORMAT;
  }
  if (getcfn(reader->fhd) < 0 || getcfn(reader->fhd) < 0 ||
      getcfn(reader->fhd) < 0)
    return MYSOFA_READ_ERROR;

  address = tellfn(reader->fhd);
  end = address;
  collection_size = readValue(reader, reader->superblock.size_of_lengths);
  if (collection_size > 0x400000000) {
    mylog("collection_size is too large\n");
    return MYSOFA_INVALID_FORMAT;
  }
  end += collection_size - 8;

  while (tellfn(reader->fhd) <= end - 8 - reader->superblock.size_of_lengths) {

    gcol = malloc(sizeof(*gcol));
    if (!gcol)
      return MYSOFA_NO_MEMORY;
    gcol->heap_object_index = readValue(reader, 2);
    if (gcol->heap_object_index == 0) {
      free(gcol);
      break;
    }
    reference_count = readValue(reader, 2);
    if (seekfn(reader->fhd, 4, SEEK_CUR) < 0) {
      free(gcol);
      return errno;
    }
    gcol->object_size = readValue(reader, reader->superblock.size_of_lengths);
    if (gcol->object_size > 8) {
      free(gcol);
      return MYSOFA_UNSUPPORTED_FORMAT;
    }
    gcol->value = readValue(reader, gcol->object_size);
    gcol->address = address;
    mylog(" GCOL object %d size %" PRIu64 " value %08" PRIX64 "\n",
          gcol->heap_object_index, gcol->object_size, gcol->value);

    gcol->next = reader->gcol;
    reader->gcol = gcol;
  }

  mylog(" END %08lX vs. %08" PRIX64 "\n", tellfn(reader->fhd),
        end); /* bug in the normal hdf5 specification */
  /*	seekfn(reader->fhd, end, SEEK_SET); */
  return MYSOFA_OK;
}

int gcolRead(struct READER *reader, uint64_t gcol, int reference,
             uint64_t *dataobject) {
  long pos;
  struct GCOL *p = reader->gcol;

  while (p && p->address != gcol && p->heap_object_index != reference) {
    p = p->next;
  }
  if (!p) {
    pos = tellfn(reader->fhd);
    if (seekfn(reader->fhd, gcol, SEEK_SET) < 0)
      return MYSOFA_READ_ERROR;
    readGCOL(reader);
    if (pos < 0)
      return MYSOFA_READ_ERROR;
    if (seekfn(reader->fhd, pos, SEEK_SET) < 0)
      return MYSOFA_READ_ERROR;

    p = reader->gcol;
    while (p && p->address != gcol && p->heap_object_index != reference) {
      p = p->next;
    }
    if (!p) {
      mylog("unknown gcol %" PRIX64 " %d\n", gcol, reference);
      return MYSOFA_INVALID_FORMAT;
    }
  }
  *dataobject = p->value;

  return MYSOFA_OK;
}
#if 0

gcol = reader->gcol;
for (;;) {
	if (gcol == NULL) {
		mylog("reference unknown!\n");
		return MYSOFA_INVALID_FORMAT;
	}
	if (gcol->heap_object_index == reference) {
		mylog("found reference at %LX\n", gcol->object_pos);
		break;
		pos = ftell(reader->fhd);
		fseek(reader->fhd, gcol->object_pos, SEEK_SET);
		dt2 = *dt;
		dt2.list = 0;
		dt2.size = gcol->object_size;
		readDataVar(reader, &dt2, ds);
		fseek(reader->fhd, pos, SEEK_SET);
		break;
	}
	gcol = gcol->next;
#endif

void gcolFree(struct GCOL *gcol) {
  if (gcol) {
    gcolFree(gcol->next);
    free(gcol);
  }
}

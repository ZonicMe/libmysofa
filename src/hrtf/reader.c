/*

 Copyright 2016 Christian Hoene, Symonics GmbH

 */

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hdf/reader.h"
#include "mysofa.h"
#include "mysofa_export.h"

/* checks file address.
 * NULL is an invalid address indicating a invalid field
 */
int validAddress(struct READER *reader, uint64_t address) {
  return address > 0 && address < reader->superblock.end_of_file_address;
}

int readfn(struct DATAFILE *obj, char *buf, int n) {
  if (obj->pos + n > obj->len) {
    n = obj->len - obj->pos;
  }

  memcpy(buf, obj->buf + obj->pos, n);
  obj->pos += n;

  return n;
}

int seekfn(struct DATAFILE *obj, long offset, int wense) {
  switch ( wense ) {
  case SEEK_SET:
    obj->pos = offset;
    break;
  case SEEK_CUR:
    obj->pos += offset;
    break;
  case SEEK_END:
    obj->pos = obj->len + offset;
    break;
  default:
    errno = EINVAL;
    return -1;
  }

  return 0;
}

long tellfn(struct DATAFILE *obj) {
  return obj->pos;
}

int getcfn(struct DATAFILE *obj) {
  if (obj->pos == obj->len) {
    return -1;
  } else {
    unsigned char ch;
    memcpy(&ch, obj->buf + obj->pos, 1);
    obj->pos++;
    return (int)ch;
  }
}

/* little endian */
uint64_t readValue(struct READER *reader, int size) {
  int i, c;
  uint64_t value;
  c = getcfn(reader->fhd);
  if (c < 0)
    return 0xffffffffffffffffLL;
  value = (uint8_t)c;
  for (i = 1; i < size; i++) {
    c = getcfn(reader->fhd);
    if (c < 0)
      return 0xffffffffffffffffLL;
    value |= ((uint64_t)c) << (i * 8);
  }
  return value;
}

static int mystrcmp(char *s1, char *s2) {
  if (s1 == NULL && s2 == NULL)
    return 0;
  if (s1 == NULL)
    return -1;
  if (s2 == NULL)
    return 1;
  return strcmp(s1, s2);
}

static int checkAttribute(struct MYSOFA_ATTRIBUTE *attribute, char *name,
                          char *value) {
  while (attribute) {
    if (!mystrcmp(attribute->name, name) && !mystrcmp(attribute->value, value))
      return MYSOFA_OK;
    attribute = attribute->next;
  }

  return MYSOFA_INVALID_FORMAT;
}

static int getDimension(unsigned *dim, struct DATAOBJECT *dataobject) {
  int err;
  struct MYSOFA_ATTRIBUTE *attr = dataobject->attributes;

  if (!!(err = checkAttribute(dataobject->attributes, "CLASS",
                              "DIMENSION_SCALE")))
    return err;

  while (attr) {
    mylog(" %s=%s\n", attr->name, attr->value);

    if (!strcmp(attr->name, "NAME") && attr->value &&
        !strncmp(attr->value,
                 "This is a netCDF dimension but not a netCDF variable.", 53)) {
      char *p = attr->value + strlen(attr->value) - 1;
      while (isdigit(*p)) {
        p--;
      }
      p++;
      *dim = atoi(p);
      mylog("NETCDF DIM %u\n", *dim);
      return MYSOFA_OK;
    }
    attr = attr->next;
  }
  return MYSOFA_INVALID_FORMAT;
}

static int getArray(struct MYSOFA_ARRAY *array, struct DATAOBJECT *dataobject) {
  float *p1;
  double *p2;
  unsigned int i;

  struct MYSOFA_ATTRIBUTE *attr = dataobject->attributes;
  while (attr) {
    mylog(" %s=%s\n", attr->name ? attr->name : "(null)",
          attr->value ? attr->value : "(null)");
    attr = attr->next;
  }

  if (dataobject->dt.u.f.bit_precision != 64)
    return MYSOFA_UNSUPPORTED_FORMAT;

  array->attributes = dataobject->attributes;
  dataobject->attributes = NULL;
  array->elements = dataobject->data_len / 8;

  p1 = dataobject->data;
  p2 = dataobject->data;
  for (i = 0; i < array->elements; i++)
    *p1++ = (float)*p2++;
  array->values = realloc(dataobject->data, array->elements * sizeof(float));

  dataobject->data = NULL;

  return MYSOFA_OK;
}

static void arrayFree(struct MYSOFA_ARRAY *array) {
  while (array->attributes) {
    struct MYSOFA_ATTRIBUTE *next = array->attributes->next;
    free(array->attributes->name);
    free(array->attributes->value);
    free(array->attributes);
    array->attributes = next;
  }
  free(array->values);
}

static int addUserDefinedVariable(struct MYSOFA_HRTF *hrtf,
                                  struct DATAOBJECT *dataobject) {
  int err;

  // init variable
  struct MYSOFA_VARIABLE *var = malloc(sizeof(struct MYSOFA_VARIABLE));

  if (!var) {
    return errno;
  }

  memset(var, 0, sizeof(struct MYSOFA_VARIABLE));

  // init values array
  var->value = malloc(sizeof(struct MYSOFA_ARRAY));
  if (!var->value) {
    free(var);
    return errno;
  }
  memset(var->value, 0, sizeof(struct MYSOFA_ARRAY));

  var->next = NULL;
  // copy name
  var->name = malloc(strlen(dataobject->name) + 1);
  if (!var->name) {
    free(var->value);
    free(var);
    return errno;
  }
  strcpy(var->name, dataobject->name);

  err = getArray(var->value, dataobject);
  if (err != MYSOFA_OK) {
    arrayFree(var->value);
    free(var->value);
    free(var->name);
    free(var);
    return err;
  }

  var->next = hrtf->variables;
  hrtf->variables = var;

  return MYSOFA_OK;
}

static struct MYSOFA_HRTF *getHrtf(struct READER *reader, int *err) {
  int dimensionflags = 0;
  struct DIR *dir = reader->superblock.dataobject.directory;

  struct MYSOFA_HRTF *hrtf = malloc(sizeof(struct MYSOFA_HRTF));
  if (!hrtf) {
    *err = errno;
    return NULL;
  }
  memset(hrtf, 0, sizeof(struct MYSOFA_HRTF));

  /* copy SOFA file attributes */
  hrtf->attributes = reader->superblock.dataobject.attributes;
  reader->superblock.dataobject.attributes = NULL;

  /* check SOFA file attributes */
  if (!!(*err = checkAttribute(hrtf->attributes, "Conventions", "SOFA"))) {
    mylog("no Conventions=SOFA attribute\n");
    goto error;
  }

  /* read dimensions */
  while (dir) {
    if (dir->dataobject.name && dir->dataobject.name[0] &&
        dir->dataobject.name[1] == 0) {
      switch (dir->dataobject.name[0]) {
      case 'I':
        *err = getDimension(&hrtf->I, &dir->dataobject);
        dimensionflags |= 1;
        break;
      case 'C':
        *err = getDimension(&hrtf->C, &dir->dataobject);
        dimensionflags |= 2;
        break;
      case 'R':
        *err = getDimension(&hrtf->R, &dir->dataobject);
        dimensionflags |= 4;
        break;
      case 'E':
        *err = getDimension(&hrtf->E, &dir->dataobject);
        dimensionflags |= 8;
        break;
      case 'N':
        *err = getDimension(&hrtf->N, &dir->dataobject);
        dimensionflags |= 0x10;
        break;
      case 'M':
        *err = getDimension(&hrtf->M, &dir->dataobject);
        dimensionflags |= 0x20;
        break;
      case 'S':
        break; /* be graceful, some issues with API version 0.4.4 */
      default:
        mylog("UNKNOWN SOFA VARIABLE %s", dir->dataobject.name);
        goto error;
      }
      if (*err)
        goto error;
    }
    dir = dir->next;
  }

  if (dimensionflags != 0x3f || hrtf->I != 1 || hrtf->C != 3) {
    mylog("dimensions are missing or wrong\n");
    goto error;
  }

  dir = reader->superblock.dataobject.directory;
  while (dir) {

    if (!dir->dataobject.name) {
      mylog("SOFA VARIABLE IS NULL.\n");
    } else if (!strcmp(dir->dataobject.name, "ListenerPosition")) {
      *err = getArray(&hrtf->ListenerPosition, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "ReceiverPosition")) {
      *err = getArray(&hrtf->ReceiverPosition, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "SourcePosition")) {
      *err = getArray(&hrtf->SourcePosition, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "EmitterPosition")) {
      *err = getArray(&hrtf->EmitterPosition, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "ListenerUp")) {
      *err = getArray(&hrtf->ListenerUp, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "ListenerView")) {
      *err = getArray(&hrtf->ListenerView, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "Data.IR")) {
      *err = getArray(&hrtf->DataIR, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "Data.SamplingRate")) {
      *err = getArray(&hrtf->DataSamplingRate, &dir->dataobject);
    } else if (!strcmp(dir->dataobject.name, "Data.Delay")) {
      *err = getArray(&hrtf->DataDelay, &dir->dataobject);
    } else if (!(dir->dataobject.name[0] && !dir->dataobject.name[1])) {
      *err = addUserDefinedVariable(hrtf, &dir->dataobject);
    }
    dir = dir->next;
  }

  return hrtf;

error:
  free(hrtf);
  if (!*err)
    *err = MYSOFA_INVALID_FORMAT;
  return NULL;
}

MYSOFA_EXPORT struct MYSOFA_HRTF *mysofa_load(const char *filename, int *err) {
  FILE* f;

  if (filename == NULL)
    filename = CMAKE_INSTALL_PREFIX "/share/libmysofa/default.sofa";

  if (strcmp(filename, "-"))
    f = fopen(filename, "rb");
  else
    f = stdin;

  if (!f) {
    mylog("cannot open file %s\n", filename);
    *err = errno;
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *data = (char*)malloc(size);
  int n = fread(data, sizeof(char), size, f);
  if ( n != size ) {
      mylog("cannot load file %s\n", filename);
      *err = errno;
      return NULL;
  }
  fclose(f);

  struct MYSOFA_HRTF *hrtf = mysofa_load_data(data, size, err);
  return hrtf;
}

MYSOFA_EXPORT struct MYSOFA_HRTF *mysofa_load_data(const char *data, const long size, int *err) {
  struct READER reader;
  struct MYSOFA_HRTF *hrtf = NULL;
  struct DATAFILE obj;

  obj.buf = data;
  obj.pos = 0L;
  obj.len = size;

  reader.fhd = &obj;
  reader.gcol = NULL;
  reader.all = NULL;
  reader.recursive_counter = 0;

  *err = superblockRead(&reader, &reader.superblock);

  if (!*err) {
    hrtf = getHrtf(&reader, err);
  }

  superblockFree(&reader, &reader.superblock);
  gcolFree(reader.gcol);

  return hrtf;
}

MYSOFA_EXPORT void mysofa_free(struct MYSOFA_HRTF *hrtf) {
  if (!hrtf)
    return;

  while (hrtf->attributes) {
    struct MYSOFA_ATTRIBUTE *next = hrtf->attributes->next;
    free(hrtf->attributes->name);
    free(hrtf->attributes->value);
    free(hrtf->attributes);
    hrtf->attributes = next;
  }

  while (hrtf->variables) {
    struct MYSOFA_VARIABLE *next = hrtf->variables->next;
    free(hrtf->variables->name);
    arrayFree(hrtf->variables->value);
    free(hrtf->variables->value);
    free(hrtf->variables);
    hrtf->variables = next;
  }

  arrayFree(&hrtf->ListenerPosition);
  arrayFree(&hrtf->ReceiverPosition);
  arrayFree(&hrtf->SourcePosition);
  arrayFree(&hrtf->EmitterPosition);
  arrayFree(&hrtf->ListenerUp);
  arrayFree(&hrtf->ListenerView);
  arrayFree(&hrtf->DataIR);
  arrayFree(&hrtf->DataSamplingRate);
  arrayFree(&hrtf->DataDelay);
  free(hrtf);
}

MYSOFA_EXPORT void mysofa_getversion(int *major, int *minor, int *patch) {
  *major = CPACK_PACKAGE_VERSION_MAJOR;
  *minor = CPACK_PACKAGE_VERSION_MINOR;
  *patch = CPACK_PACKAGE_VERSION_PATCH;
}

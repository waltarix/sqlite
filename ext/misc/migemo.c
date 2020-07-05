#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_MIGEMO)

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <assert.h>
#include <string.h>
#include <stdio.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <migemo.h>

#ifdef SQLITE_PCRE2_JIT
#define PCRE2_MATCH_FUNCTION pcre2_jit_match
#else
#define PCRE2_MATCH_FUNCTION pcre2_match
#endif

#ifndef MIGEMO_CACHE_SIZE
#define MIGEMO_CACHE_SIZE 16
#endif

typedef struct {
  char *pattern;
  pcre2_code *regexp;
} MigemoCacheEntry;

typedef struct {
  migemo *pmigemo;
  MigemoCacheEntry *entries;
} MigemoCache;

static void migemoFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  const char *zPattern, *zSubject;
  char *errmsg;
  pcre2_code *regexp;
  int pcre2Rc;

  assert(argc == 2);

  zPattern = (const char*)sqlite3_value_text(argv[0]);
  if( !zPattern ){
    sqlite3_result_error(ctx, "no word", -1);
    return;
  }

  zSubject = (const char*)sqlite3_value_text(argv[1]);
  if( !zSubject ) zSubject = "";

  /* simple LRU cache */
  {
    int i;
    int found = 0;
    MigemoCache *migemoCache = sqlite3_user_data(ctx);
    MigemoCacheEntry *entries = migemoCache->entries;
    assert(entries);

    for (i = 0; i < MIGEMO_CACHE_SIZE && entries[i].pattern; i++)
      if( strcmp(zPattern, entries[i].pattern)==0 ){
        found = 1;
        break;
      }
    if( found ){
      if( i > 0 ){
        MigemoCacheEntry entry = entries[i];
        memmove(entries + 1, entries, i * sizeof(MigemoCacheEntry));
        entries[0] = entry;
      }
    }else{
      unsigned char *migemoPattern;
      MigemoCacheEntry entry;
      int errorCode;
      PCRE2_SIZE errorOffset;
      PCRE2_UCHAR errorBuffer[256];

      migemoPattern = migemo_query(migemoCache->pmigemo, (const unsigned char*)zPattern);
      entry.regexp = pcre2_compile((PCRE2_SPTR)migemoPattern, strlen((const char*)migemoPattern),
          PCRE2_UTF, &errorCode, &errorOffset, NULL);
      migemo_release(migemoCache->pmigemo, migemoPattern);

      if( !entry.regexp ){
        pcre2_get_error_message(errorCode, errorBuffer, sizeof(errorBuffer));
        sqlite3_result_error(ctx, (const char*)errorBuffer, -1);
        return;
      }

#ifdef SQLITE_PCRE2_JIT
      pcre2Rc = pcre2_jit_compile(entry.regexp, PCRE2_JIT_COMPLETE);
      if( pcre2Rc ){
        errmsg = sqlite3_mprintf("Couldn't JIT: %d", pcre2Rc);
        sqlite3_result_error(ctx, errmsg, -1);
        sqlite3_free(errmsg);
        pcre2_code_free(entry.regexp);
        return;
      }
#endif

      entry.pattern = strdup(zPattern);
      if( !entry.pattern ){
        sqlite3_result_error(ctx, "strdup: ENOMEM", -1);
        pcre2_code_free(entry.regexp);
        return;
      }

      i = MIGEMO_CACHE_SIZE - 1;
      if( entries[i].pattern ){
        free(entries[i].pattern);
        assert(entries[i].regexp);
        pcre2_code_free(entries[i].regexp);
      }
      memmove(entries + 1, entries, i * sizeof(MigemoCacheEntry));
      entries[0] = entry;
    }
    regexp = entries[0].regexp;
  }

  {
    assert(regexp);

    pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(regexp, NULL);
    pcre2Rc = PCRE2_MATCH_FUNCTION(regexp, (PCRE2_SPTR)zSubject, strlen(zSubject), 0, 0, matchData, NULL);
    pcre2_match_data_free(matchData);

    sqlite3_result_int(ctx, pcre2Rc >= 0);
  }
}

static void migemoFreeCallback(void *p){
  MigemoCache *migemoCache = (MigemoCache*)p;
  migemo_close(migemoCache->pmigemo);
  for (int i = 0; i < MIGEMO_CACHE_SIZE && migemoCache->entries[i].pattern; i++)
    pcre2_code_free(migemoCache->entries[i].regexp);
  free(migemoCache->entries);
  sqlite3_free(p);
}

int sqlite3MigemoInit(sqlite3 *db){
  MigemoCache *migemoCache = sqlite3_malloc(sizeof(MigemoCache));
  if( !migemoCache ) return SQLITE_NOMEM;

  char* migemo_dict = getenv("MIGEMO_DICT");
  migemoCache->pmigemo = migemo_open(migemo_dict);

  migemoCache->entries = calloc(MIGEMO_CACHE_SIZE, sizeof(MigemoCacheEntry));
  if( !migemoCache->entries ) return SQLITE_NOMEM;

  return sqlite3_create_function_v2(db, "migemo", 2, SQLITE_UTF8,
      migemoCache, migemoFunc, NULL, NULL, migemoFreeCallback
  );
}

#ifndef SQLITE_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_migemo_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi)
  (void)pzErrMsg; /* Unused parameter */
  return sqlite3MigemoInit(db);
}
#endif

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_MIGEMO) */

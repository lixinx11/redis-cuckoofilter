#include "redismodule.h"
#include <stdlib.h>
#include <string.h>

static RedisModuleType *CuckooFilterType;

/**
 * Direct include.
 */
#include "cuckoofilter.c"

/**
 * Terrible macro that does the setup part for cd.add, cf.rem, and cf.check.
 * Parses the hash, the FP, and gets the CuckooFilter from Redis.
 * Since handling errors is gonna be somewhat janky anyway, I'm going full macro.
 * (for now)
 */
#define COMMAND_4_PREAMBLE(hash, fpChar, key, cf, mode) {\
    RedisModule_AutoMemory(ctx);\
    if (argc != 4) return RedisModule_WrongArity(ctx);\
    \
    if (RedisModule_StringToLongLong(argv[2], (long long *)&hash) != REDISMODULE_OK) {\
        RedisModule_ReplyWithError(ctx,"ERR hash is not unsigned long long");\
        return REDISMODULE_OK;\
    }\
    \
    long long fpLong;\
    if (RedisModule_StringToLongLong(argv[3], &fpLong) != REDISMODULE_OK) {\
        RedisModule_ReplyWithError(ctx,"ERR invalid fprint value");\
        return REDISMODULE_OK;\
    }\
    \
    if (fpLong < 0 || fpLong > 255){\
        RedisModule_ReplyWithError(ctx,"ERR fprint is not in range [0, 255]");\
        return REDISMODULE_OK;\
    }\
    \
    fpChar = (char)fpLong;\
    \
    if (fpChar == 0)\
    {\
        fpChar = 1;\
    }\
    \
    key = RedisModule_OpenKey(ctx, argv[1], mode);\
    int type = RedisModule_KeyType(key);\
    if (type == REDISMODULE_KEYTYPE_EMPTY || RedisModule_ModuleTypeGetType(key) != CuckooFilterType)\
    {\
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);\
        return REDISMODULE_OK;\
    }\
    cf = (CuckooFilter*)RedisModule_ModuleTypeGetValue(key);\
    hash = hash & (cf->numBuckets - 1);\
}




/** 
 * CF.INIT key numBuckets bucketSize
 *
 * numBuckets is a power of 2 in the range [64K, ..., 1M, ..., 512M, ..., 1G, ...,  8G]
 */
int CFInit_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 4) return RedisModule_WrongArity(ctx);

    const char *filterType = RedisModule_StringPtrLen(argv[2], NULL);
    unsigned long long size = 1024ULL;

    if      (0 == strcmp("1K",   filterType)) size *= 1;
    else if (0 == strcmp("2K",   filterType)) size *= 2;
    else if (0 == strcmp("4K",   filterType)) size *= 4;
    else if (0 == strcmp("8K",   filterType)) size *= 8;
    else if (0 == strcmp("16K",  filterType)) size *= 16;
    else if (0 == strcmp("32K",  filterType)) size *= 32;
    else if (0 == strcmp("64K",  filterType)) size *= 64;
    else if (0 == strcmp("128K", filterType)) size *= 128;
    else if (0 == strcmp("256K", filterType)) size *= 256;
    else if (0 == strcmp("512K", filterType)) size *= 512;
    else if (0 == strcmp("1M",   filterType)) size *= 1024;
    else if (0 == strcmp("2M",   filterType)) size *= 1024 * 2;
    else if (0 == strcmp("4M",   filterType)) size *= 1024 * 4;
    else if (0 == strcmp("8M",   filterType)) size *= 1024 * 8;
    else if (0 == strcmp("16M",  filterType)) size *= 1024 * 16;
    else if (0 == strcmp("32M",  filterType)) size *= 1024 * 32;
    else if (0 == strcmp("64M",  filterType)) size *= 1024 * 64;
    else if (0 == strcmp("128M", filterType)) size *= 1024 * 128;
    else if (0 == strcmp("256M", filterType)) size *= 1024 * 256;
    else if (0 == strcmp("512M", filterType)) size *= 1024 * 512;
    else if (0 == strcmp("1G",   filterType)) size *= 1024 * 1024;
    else if (0 == strcmp("2G",   filterType)) size *= 1024 * 1024 * 2;
    else if (0 == strcmp("4G",   filterType)) size *= 1024 * 1024 * 4;
    else if (0 == strcmp("8G",   filterType)) size *= 1024 * 1024 * 8;
    else {
        RedisModule_ReplyWithError(ctx,"ERR unsupported filter size");
        return REDISMODULE_OK;
    }

    long long bucketSize;
    if (RedisModule_StringToLongLong(argv[3], &bucketSize) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx,"ERR unable to parse bucketsize");
        return REDISMODULE_OK;
    }

    if (bucketSize != 2 && bucketSize != 4){
        RedisModule_ReplyWithError(ctx,"ERR invalid value for bucketsize");
        return REDISMODULE_OK;   
    }


    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY && RedisModule_ModuleTypeGetType(key) != CuckooFilterType)
    {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_OK;
    }

    CuckooFilter *cf;

    if (type == REDISMODULE_KEYTYPE_EMPTY) {

        /** 
         * New Cuckoo Filter!
         */
        cf = cf_init(size, bucketSize);

        RedisModule_ModuleTypeSetValue(key, CuckooFilterType, cf);
    } else {
        cf = (CuckooFilter*)RedisModule_ModuleTypeGetValue(key);
    }

    RedisModule_ReplyWithLongLong(ctx, cf->numBuckets);
    return REDISMODULE_OK;
}




// CF.ADD key bucket fp
int CFAdd_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    unsigned long long hash;
    char fpChar;
    RedisModuleKey *key;
    CuckooFilter *cf;
    COMMAND_4_PREAMBLE(hash, fpChar, key, cf, REDISMODULE_READ|REDISMODULE_WRITE)

    unsigned long long altHash = cf_alternative_hash(cf, hash, fpChar);

    if (cf_insert_fp(cf, hash, fpChar, NULL) || cf_insert_fp(cf, altHash, fpChar, NULL)) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
        return REDISMODULE_OK;
    }

    char homelessFP = 0;
    unsigned long long homelessHash = altHash;
    for (int n = 0; n < 500; n++) {
        cf_insert_fp(cf, homelessHash, fpChar, &homelessFP);

        if (!homelessFP) {
            RedisModule_ReplyWithSimpleString(ctx, "OK");
            return REDISMODULE_OK;
        } 

        homelessHash = cf_alternative_hash(cf, homelessHash, homelessFP);
        fpChar = homelessFP;
        homelessFP = 0;

    }


    // Return the length of the cuckoo filter
    RedisModule_ReplyWithError(ctx,"ERR too full");
    return REDISMODULE_OK;
}


// CF.REM key slot fp
int CFRem_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    unsigned long long hash;
    char fpChar;
    RedisModuleKey *key;
    CuckooFilter *cf;
    COMMAND_4_PREAMBLE(hash, fpChar, key, cf, REDISMODULE_READ|REDISMODULE_WRITE)

    if (!cf_delete_fp(cf, hash, fpChar)){
        RedisModule_ReplyWithError(ctx,"ERR tried to delete non-existing item. THE FILTER MIGHT BE COMPROMISED.");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}


// CF.CHECK key slot fp
int CFCheck_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    unsigned long long hash;
    char fpChar;
    RedisModuleKey *key;
    CuckooFilter *cf;
    COMMAND_4_PREAMBLE(hash, fpChar, key, cf, REDISMODULE_READ)

    if (cf_search_fp(cf, hash, fpChar)){
        RedisModule_ReplyWithLongLong(ctx, 1);
        return REDISMODULE_OK;
    }
    

    RedisModule_ReplyWithLongLong(ctx, 0);
    return REDISMODULE_OK;
}

// CF.DUMP key
int CFDump_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 2) return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (type == REDISMODULE_KEYTYPE_EMPTY || RedisModule_ModuleTypeGetType(key) != CuckooFilterType)
    {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_OK;
    }

    CuckooFilter *cf = (CuckooFilter*)RedisModule_ModuleTypeGetValue(key);

    RedisModule_ReplyWithStringBuffer(ctx, cf->filter, cf->numBuckets * cf->bucketSize);

    return REDISMODULE_OK;
}







/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx,"cuckoofilter", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = CFLoad,
        .rdb_save = CFSave,
        .aof_rewrite = CFRewrite,
        .free = CFFree
    };

    CuckooFilterType = RedisModule_CreateDataType(ctx, "cuckoof-k", CUCKOO_FILTER_ENCODING_VERSION, &tm);
    if (CuckooFilterType == NULL) return REDISMODULE_ERR;

    /* Log the list of parameters passing loading the module. */
    for (int j = 0; j < argc; j++) {
        const char *s = RedisModule_StringPtrLen(argv[j],NULL);
        printf("Module loaded with ARGV[%d] = %s\n", j, s);
    }

    if (RedisModule_CreateCommand(ctx,"cf.init",
        CFInit_RedisCommand,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"cf.add",
        CFAdd_RedisCommand,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"cf.rem",
        CFRem_RedisCommand,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"cf.check",
        CFCheck_RedisCommand,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"cf.dump",
        CFDump_RedisCommand,"readonly",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;


    return REDISMODULE_OK;
}
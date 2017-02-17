//
// Created by frank on 17-2-10.
// 简单的内存池
// 一次最多分配MEM_POOL_DEFAULT_SIZE字节数
//

#ifndef FANCY_MEM_POOL_H
#define FANCY_MEM_POOL_H

#include "base.h"

#define MEM_POOL_DEFAULT_SIZE   (16 * 1024)
#define MEM_POOL_ALIGNMENT      sizeof(unsigned long)

typedef struct mem_pool mem_pool;

struct mem_pool {
    u_char      *last;
    u_char      *end;
    u_int       failed;
    mem_pool    *next;

    /* 以下字段为mem_pool头结点独有 */
    mem_pool    *current;
    /* 剩余字段 */
    //...
};

mem_pool *mem_pool_create(size_t size);
void mem_pool_destroy(mem_pool *pool);

/* 返回对齐指针，同malloc */
void *palloc(mem_pool *pool, size_t size);
void *pcalloc(mem_pool *pool, size_t size);

#endif //FANCY_MEM_POOL_H
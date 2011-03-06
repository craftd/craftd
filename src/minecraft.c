/*
 * Copyright (c) 2010-2011 Kevin M. Bowling, <kevin.bowling@kev009.com>, USA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <craftd/common.h>

#define CRAFTD_MINECRAFT_IGNORE_EXTERN
#include <craftd/minecraft.h>
#undef CRAFTD_MINECRAFT_IGNORE_EXTERN

const MCEntityId MCMaxEntityId = INT_MAX;

void
MC_DestroyString (MCString self)
{
    CD_DestroyString(self);
}

MCMetadata*
MC_CreateMetadata (void)
{
    MCMetadata* self = CD_malloc(sizeof(MCMetadata));

    if (!self) {
        return NULL;
    }

    return self;
}

void
MC_DestroyMetadata (MCMetadata* self)
{
    for (size_t i = 0; i < self->length; i++) {
        MC_DestroyData(self->item[i]);
    }

    CD_free(self->item);
    CD_free(self);
}

MCData*
MC_CreateData (void)
{
    MCData* self = CD_malloc(sizeof(MCData));

    if (!self) {
        return NULL;
    }

    return self;
}

void
MC_DestroyData (MCData* self)
{
    // Destroy the bstring, the other types lay on the stack
    if (self->type == MCTypeString) {
        CD_DestroyString(self->data.S);
    }

    CD_free(self);
}

MCMetadata*
MC_ConcatDatas (MCMetadata* metadata, MCData** items, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        MC_AppendData(metadata, items[i]);
    }

    return metadata;
}

MCMetadata*
MC_AppendData (MCMetadata* metadata, MCData* data)
{
    metadata->item = CD_realloc(metadata->item, sizeof(MCData*) * ++metadata->length);

    metadata->item[metadata->length - 1] = data;

    return metadata;
}

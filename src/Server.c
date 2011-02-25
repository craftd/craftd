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

#define CRAFTD_SERVER_IGNORE_EXTERN
#include "Server.h"
#undef CRAFTD_SERVER_IGNORE_EXTERN

#include "common.h"

#include "Player.h"

CDServer* CDMainServer = NULL;

CDServer*
CD_CreateServer (const char* path)
{
    CDServer* self = CD_malloc(sizeof(CDServer));

    if (!self) {
        return NULL;
    }

    pthread_spin_init(&self->lock.time, PTHREAD_PROCESS_PRIVATE);

    self->logger   = CDConsoleLogger;
    self->timeloop = CD_CreateTimeLoop(self);
    self->config   = CD_ParseConfig(path);
    self->workers  = CD_CreateWorkers(self);
    self->plugins  = CD_CreatePlugins(self);

    if (!self->config || !self->workers) {
        CD_DestroyServer(self);
    }

    self->entities = CD_CreateMap();
    self->players  = CD_CreateHash();

    self->event.callbacks = CD_CreateHash();

/*
    size_t i;

    for (i = 0; i < self->config->plugins->length) {
        CD_LoadPlugin(self->plugins, self->config->plugins->item[i]);
    }
*/

    CD_ServerSetTime(self, 0);

    PRIVATE(self) = CD_CreateHash();

    return self;
}

void
CD_DestroyServer (CDServer* self)
{
    CD_DestroyConfig(self->config);
    CD_DestroyWorkers(self->workers);
    CD_DestroyPlugins(self->plugins);

    if (self->event.base) {
        event_base_free(self->event.base);
        self->event.base = NULL;
    }

    if (self->event.listener) {
        event_free(self->event.listener);
        self->event.listener = NULL;
    }

    CD_DestroyHash(self->event.callbacks);

    CD_DestroyHash(PRIVATE(self));

    pthread_spin_destroy(&self->lock.time);

    CD_free(self->name);
}

char*
CD_ServerToString (CDServer* self)
{
    return self->name;
}

short
CD_ServerGetTime (CDServer* self)
{
    short result;

    pthread_spin_lock(&self->lock.time);
    result = self->time;
    pthread_spin_unlock(&self->lock.time);

    return result;
}

short
CD_ServerSetTime (CDServer* self, short time)
{
    pthread_spin_lock(&self->lock.time);
    self->time = time;
    pthread_spin_unlock(&self->lock.time);

    return time;
}

static
void
cd_ReadCallback (struct bufferevent* event, CDPlayer* player)
{
    pthread_rwlock_wrlock(&player->lock.pending);
    if (!player->pending) {
        player->pending = true;

        CD_AddJob(player->server->workers, CD_CreateJob(CDPlayerInput, player));
    }
    pthread_rwlock_unlock(&player->lock.pending);
}

static
void
cd_ErrorCallback (struct bufferevent* event, short error, CDPlayer* player)
{

}

static
void
cd_Accept (evutil_socket_t listener, short event, void* arg)
{
    CDServer*               self = arg;
    CDPlayer*               player;
    struct sockaddr_storage storage;
    socklen_t               length = sizeof(storage);
    int                     fd     = accept(listener, (struct sockaddr*) &storage, &length);

    if (fd < 0) {
        SERR(self, "accept error");
        return;
    }

    if (CD_HashLength(self->players) >= self->config->cache.maxPlayers) {
        close(fd);
        SERR(self, "too many clients");
        return;
    }

    if (getpeername(fd, (struct sockaddr*) &storage, &length) < 0) {
        SERR(self, "could not get peer IP");
        close(fd);
        return;
    }

    player = CD_CreatePlayer(self);

    if (storage.ss_family == AF_INET) {
        evutil_inet_ntop(storage.ss_family, &((struct sockaddr_in*) &storage)->sin_addr, player->ip, sizeof(player->ip));
    }
    else if (storage.ss_family == AF_INET6) {
        evutil_inet_ntop(storage.ss_family, &((struct sockaddr_in6*) &storage)->sin6_addr, player->ip, sizeof(player->ip));
    }
    else {
        SERR(self, "weird address family");
        close(fd);
        CD_DestroyPlayer(player);
    }

    player->socket = fd;
    evutil_make_socket_nonblocking(player->socket);

    player->buffer = bufferevent_socket_new(self->event.base, player->socket, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    bufferevent_setcb(player->buffer, cd_ReadCallback, NULL, cd_ErrorCallback, player);
    bufferevent_enable(player->buffer, EV_READ | EV_WRITE);

    CD_MapSet(self->entities, player->entity.id, player);
}

bool
CD_RunServer (CDServer* self)
{
    if ((self->event.base = event_base_new()) == NULL) {
        SERR(self, "could not create MC libevent base!");

        return false;
    }

    if ((self->socket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        SERR(self, "could not create socket!");

        return false;
    }

    evutil_make_socket_nonblocking(self->socket);

    #ifndef WIN32
    {
        int one = 1;
        setsockopt(self->socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    #endif

    pthread_create(&self->timeloop->thread, &self->timeloop->attributes, CD_RunTimeLoop, self->timeloop);

    self->event.listener = event_new(self->event.base, self->socket, EV_READ | EV_PERSIST, cd_Accept, self);

    event_add(self->event.listener, NULL);

    return event_base_dispatch(self->event.base) != 0;
}

// FIXME: This is just a dummy function
MCEntityId
CD_ServerGenerateEntityId (CDServer* self)
{
    MCEntityId result;

    if (CD_MapLength(self->entities) != 0) {
        result = ((MCEntity*) CD_MapLast(self->entities))->id + 1;
    }
    else {
        result = INT_MIN;
    }

    return result;
}

bool
cd_EventBeforeDispatch (CDServer* self, const char* eventName, ...)
{
    CDList* callbacks = CD_HashGet(self->event.callbacks, "Event.dispatch:before");
    va_list ap;

    va_start(ap, eventName);

    CD_LIST_FOREACH(callbacks, it) {
        if (!CD_ListIteratorValue(it)) {
            continue;
        }

        if (!((CDEventCallback) CD_ListIteratorValue(it))(self, eventName, ap)) {
            return false;
        }
    }

    return true;
}

bool
cd_EventAfterDispatch (CDServer* self, const char* eventName, ...)
{
    CDList* callbacks = CD_HashGet(self->event.callbacks, "Event.dispatch:after");
    va_list ap;

    va_start(ap, eventName);

    CD_LIST_FOREACH(callbacks, it) {
        if (!CD_ListIteratorValue(it)) {
            continue;
        }

        if (!((CDEventCallback) CD_ListIteratorValue(it))(self, eventName, ap)) {
            return false;
        }
    }

    return true;
}

void
CD_EventRegister (CDServer* self, const char* eventName, CDEventCallback callback)
{
    CDList* callbacks = CD_HashGet(self->event.callbacks, eventName);

    if (!callbacks) {
        callbacks = CD_CreateList();
    }

    CD_ListPush(callbacks, callback);

    CD_HashSet(self->event.callbacks, eventName, callbacks);
}

CDEventCallback*
CD_EventUnregister (CDServer* self, const char* eventName, CDEventCallback callback)
{
    CDList*          callbacks = CD_HashGet(self->event.callbacks, eventName);
    CDEventCallback* result    = NULL;

    if (!callbacks) {
        return NULL;
    }

    if (callback) {
        result    = CD_calloc(2, sizeof(CDEventCallback));
        result[0] = CD_ListDeleteAll(callbacks, callback);
    }
    else {
        result = CD_ListClear(callbacks);
    }

    if (CD_ListLength(callbacks) == 0) {
        CD_DestroyList(callbacks);
    }

    return result;
}
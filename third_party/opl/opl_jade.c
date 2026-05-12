//
// Copyright(C) 2005-2014 Simon Howard
//
// Jade / Emscripten: OPL timer + Nuked OPL3 output without SDL_mixer.
// Based on opl_sdl.c (same GPL-2+). Single-threaded: queue "locks" are no-ops.
//

#include "config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opl3.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_jade.h"
#include "opl_queue.h"

#define MAX_SOUND_SLICE_TIME 100

typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t     expire_time;
} opl_timer_t;

#define LOCK_Q()   ((void)0)
#define UNLOCK_Q() ((void)0)
#define LOCK_CB()  ((void)0)
#define UNLOCK_CB() ((void)0)

static opl_callback_queue_t *callback_queue;
static uint64_t              current_time;
static int                   opl_jade_paused;
static uint64_t              pause_offset;
static opl3_chip             opl_chip;
static int                   opl_opl3mode_hw;
static int                   register_num;
static opl_timer_t           timer1 = {12500, 0, 0, 0};
static opl_timer_t           timer2 = {3125, 0, 0, 0};
static int                   mixing_freq;

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void          *callback_data;
    uint64_t       us;

    LOCK_Q();
    us = ((uint64_t)nsamples * OPL_SECOND) / (unsigned int)mixing_freq;
    current_time += us;

    if (opl_jade_paused)
        pause_offset += us;

    while (!OPL_Queue_IsEmpty(callback_queue)
           && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
            break;

        UNLOCK_Q();
        LOCK_CB();
        callback(callback_data);
        UNLOCK_CB();
        LOCK_Q();
    }
    UNLOCK_Q();
}

static void FillBuffer(int16_t *buffer, unsigned int nsamples)
{
    if (nsamples == 0)
        return;
    if (nsamples >= (unsigned int)mixing_freq)
        nsamples = (unsigned int)mixing_freq - 1u;
    OPL3_GenerateStream(&opl_chip, buffer, nsamples);
}

void OPL_Jade_Render_Add(int16_t *buffer, unsigned int buffer_len)
{
    unsigned int filled = 0;

    if (callback_queue == NULL || mixing_freq <= 0)
        return;

    while (filled < buffer_len)
    {
        uint64_t next_callback_time;
        uint64_t nsamples;

        LOCK_Q();

        if (opl_jade_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = buffer_len - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;
            nsamples             = (next_callback_time - current_time) * (uint64_t)mixing_freq;
            nsamples             = (nsamples + OPL_SECOND - 1) / OPL_SECOND;
            if (nsamples > buffer_len - filled)
                nsamples = buffer_len - filled;
            if (nsamples == 0)
                nsamples = 1;
        }

        UNLOCK_Q();

        {
            static int16_t chunk[4096 * 2];
            unsigned int   n = (unsigned int)nsamples;
            unsigned int   i;
            if (n > 4096u)
                n = 4096u;
            if (n > buffer_len - filled)
                n = buffer_len - filled;
            FillBuffer(chunk, n);
            for (i = 0; i < n * 2u; ++i)
            {
                int32_t s = (int32_t)buffer[(size_t)filled * 2u + i] + (int32_t)chunk[i];
                if (s > 32767)
                    s = 32767;
                else if (s < -32768)
                    s = -32768;
                buffer[(size_t)filled * 2u + i] = (int16_t)s;
            }
            filled += n;
            AdvanceTime(n);
        }
    }
}

static void OPL_Jade_Shutdown(void)
{
    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
    mixing_freq = 0;
}

static int OPL_Jade_Init(unsigned int port_base)
{
    (void)port_base;

    mixing_freq = (int)opl_sample_rate;
    if (mixing_freq <= 0)
        mixing_freq = 44100;

    opl_jade_paused = 0;
    pause_offset    = 0;
    callback_queue  = OPL_Queue_Create();
    current_time    = 0;

    OPL3_Reset(&opl_chip, (uint32_t)mixing_freq);
    opl_opl3mode_hw = 0;

    return 1;
}

static unsigned int OPL_Jade_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
        return 0xff;

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;
        result |= 0x40;
    }
    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;
        result |= 0x20;
    }
    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;
    if (timer->enabled)
    {
        tics               = 0x100 - (int)timer->value;
        timer->expire_time = current_time + ((uint64_t)tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
    case OPL_REG_TIMER1:
        timer1.value = value;
        OPLTimer_CalculateEndTime(&timer1);
        break;
    case OPL_REG_TIMER2:
        timer2.value = value;
        OPLTimer_CalculateEndTime(&timer2);
        break;
    case OPL_REG_TIMER_CTRL:
        if (value & 0x80)
        {
            timer1.enabled = 0;
            timer2.enabled = 0;
        }
        else
        {
            if ((value & 0x40) == 0)
            {
                timer1.enabled = (value & 0x01) != 0;
                OPLTimer_CalculateEndTime(&timer1);
            }
            if ((value & 0x20) == 0)
            {
                timer2.enabled = (value & 0x02) != 0;
                OPLTimer_CalculateEndTime(&timer2);
            }
        }
        break;
    case OPL_REG_NEW:
        opl_opl3mode_hw = value & 0x01;
        /* fallthrough */
    default:
        OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
        break;
    }
}

static void OPL_Jade_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
        register_num = (int)value;
    else if (port == OPL_REGISTER_PORT_OPL3)
        register_num = (int)(value | 0x100u);
    else if (port == OPL_DATA_PORT)
        WriteRegister((unsigned int)register_num, value);
}

static void OPL_Jade_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    LOCK_Q();
    OPL_Queue_Push(callback_queue, callback, data, current_time - pause_offset + us);
    UNLOCK_Q();
}

static void OPL_Jade_ClearCallbacks(void)
{
    LOCK_Q();
    OPL_Queue_Clear(callback_queue);
    UNLOCK_Q();
}

static void OPL_Jade_Lock(void) {}
static void OPL_Jade_Unlock(void) {}

static void OPL_Jade_SetPaused(int paused)
{
    opl_jade_paused = paused;
}

static void OPL_Jade_AdjustCallbacks(float factor)
{
    LOCK_Q();
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    UNLOCK_Q();
}

void OPL_Jade_DelayUs(uint64_t us)
{
    uint64_t target;
    unsigned int step;

    if (callback_queue == NULL || mixing_freq <= 0)
        return;

    target = current_time + us;
    step   = (unsigned int)(mixing_freq / 400);
    if (step < 1u)
        step = 1u;

    while (current_time < target)
    {
        AdvanceTime(step);
        if (current_time > target + OPL_SECOND * 2)
            break;
    }
}

opl_driver_t opl_jade_driver = {
    "jade",
    OPL_Jade_Init,
    OPL_Jade_Shutdown,
    OPL_Jade_PortRead,
    OPL_Jade_PortWrite,
    OPL_Jade_SetCallback,
    OPL_Jade_ClearCallbacks,
    OPL_Jade_Lock,
    OPL_Jade_Unlock,
    OPL_Jade_SetPaused,
    OPL_Jade_AdjustCallbacks,
};
